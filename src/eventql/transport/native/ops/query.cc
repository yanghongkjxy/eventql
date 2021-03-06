/**
 * Copyright (c) 2016 DeepCortex GmbH <legal@eventql.io>
 * Authors:
 *   - Paul Asmuth <paul@eventql.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include "eventql/transport/native/server.h"
#include "eventql/transport/native/connection_tcp.h"
#include "eventql/transport/native/frames/query_result.h"
#include "eventql/transport/native/frames/query_progress.h"
#include "eventql/util/logging.h"
#include "eventql/util/wallclock.h"
#include "eventql/util/util/binarymessagereader.h"
#include "eventql/db/database.h"
#include "eventql/server/session.h"
#include "eventql/server/sql_service.h"
#include "eventql/sql/runtime/runtime.h"
#include "eventql/auth/client_auth.h"

namespace eventql {
namespace native_transport {

ReturnCode performOperation_QUERY(
    Database* database,
    NativeConnection* conn,
    const std::string& payload) {
  auto session = database->getSession();
  auto dbctx = session->getDatabaseContext();

  /* read query frame */
  std::string q_query;
  uint64_t q_flags;
  uint64_t q_maxrows;
  std::string q_database;
  try {
    util::BinaryMessageReader q_frame(payload.data(), payload.size());
    q_query = q_frame.readLenencString();
    q_flags = q_frame.readVarUInt();
    q_maxrows = q_frame.readVarUInt();
    if (q_flags & EVQL_QUERY_SWITCHDB) {
      q_database = q_frame.readLenencString();
    }
  } catch (const std::exception& e) {
    return ReturnCode::error("ERUNTIME", "invalid QUERY frame");
  }

  if (q_maxrows == 0) {
    q_maxrows = 1;
  }

  /* set heartbeat callback */
  session->setHeartbeatCallback([conn] () -> ReturnCode {
    return conn->sendHeartbeatFrame();
  });

  /* switch database */
  if (q_flags & EVQL_QUERY_SWITCHDB) {
    auto rc = dbctx->client_auth->changeNamespace(session, q_database);
    if (!rc.isSuccess()) {
      return conn->sendErrorFrame(rc.message());
    }
  }

  if (session->getEffectiveNamespace().empty()) {
    return conn->sendErrorFrame("No database selected");
  }

  /* execute queries */
  try {
    auto txn = dbctx->sql_service->startTransaction(session);
    auto qplan = dbctx->sql_runtime->buildQueryPlan(txn.get(), q_query);

    /* set progress callback */
    if (q_flags & EVQL_QUERY_PROGRESS) {
      auto progress_interval_op= dbctx->config->getInt(
          "server.query_progress_rate_limit");
      auto progress_interval = progress_interval_op.get();
      uint64_t progress_last = 0;

      qplan->setProgressCallback([
          &qplan,
          &conn,
          &progress_interval,
          &progress_last] () {
        auto now = MonotonicClock::now();
        if (now < progress_last + progress_interval) {
          return;
        }

        progress_last = now;

        if (conn->isOutboxEmpty()) {
          auto progress = qplan->getProgress();
          QueryProgressFrame progress_frame;
          progress_frame.setQueryProgressPermill(progress * 1000);
          std::string payload;
          auto payload_os = StringOutputStream::fromString(&payload);
          progress_frame.writeTo(payload_os.get());

          conn->sendFrameAsync(
              EVQL_OP_QUERY_PROGRESS,
              0,
              payload.data(),
              payload.size());
        } else {
          conn->flushOutbox(false, 0);
        }
      });
    }

    if (qplan->numStatements() > 1 && !(q_flags & EVQL_QUERY_MULTISTMT)) {
      return conn->sendErrorFrame(
          "you must set EVQL_QUERY_MULTISTMT to enable multiple statements");
    }

    auto num_statements = qplan->numStatements();
    for (int i = 0; i < num_statements; ++i) {
      /* execute query */
      auto result_cursor = qplan->execute(i);
      auto result_ncols = result_cursor->getNumColumns();

      /* send response frames */
      QueryResultFrame r_frame(qplan->getStatementgetResultColumns(i));
      Vector<csql::SValue> row(result_ncols);
      while (result_cursor->next(row.data(), row.size())) {
        r_frame.addRow(row);

        if (r_frame.getRowCount() > q_maxrows ||
            r_frame.getRowBytes() > NativeConnection::kMaxFrameSizeSoft) {
          {
            auto rc = r_frame.writeTo(conn);
            if (!rc.isSuccess()) {
              return rc;
            }
          }
          r_frame.clear();

          /* wait for discard or continue */
          uint16_t n_opcode;
          uint16_t n_flags;
          std::string n_payload;
          {
            auto rc = conn->recvFrame(
                &n_opcode,
                &n_flags,
                &n_payload,
                session->getIdleTimeout());

            if (!rc.isSuccess()) {
              return rc;
            }
          }

          bool cont = true;
          switch (n_opcode) {
            case EVQL_OP_QUERY_CONTINUE:
              break;
            case EVQL_OP_QUERY_DISCARD:
              cont = false;
              break;
            default:
              conn->close();
              return ReturnCode::error("ERUNTIME", "unexpected opcode");
          }

          if (!cont) {
            break;
          }
        }
      }

      bool pending_statement = i + 1 < num_statements;
      r_frame.setIsLast(true);
      r_frame.setHasPendingStatement(pending_statement);
      r_frame.writeTo(conn);

      if (!pending_statement) {
        break;
      }

      /* wait for discard or continue (next query) */
      uint16_t n_opcode;
      uint16_t n_flags;
      std::string n_payload;
      auto rc = conn->recvFrame(
          &n_opcode,
          &n_flags,
          &n_payload,
          session->getIdleTimeout());

      if (!rc.isSuccess()) {
        return rc;
      }

      bool cont = true;
      switch (n_opcode) {
        case EVQL_OP_QUERY_NEXT:
          break;
        case EVQL_OP_QUERY_DISCARD:
          cont = false;
          break;
        default:
          conn->close();
          return ReturnCode::error("ERUNTIME", "unexpected opcode");
      }

      if (!cont) {
        break;
      }
    }
  } catch (const StandardException& e) {
    return conn->sendErrorFrame(e.what());
  }

  return ReturnCode::success();
}

} // namespace native_transport
} // namespace eventql

