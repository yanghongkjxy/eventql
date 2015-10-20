/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include "stx/wallclock.h"
#include "stx/assets.h"
#include "stx/protobuf/msg.h"
#include "stx/io/BufferedOutputStream.h"
#include "zbase/api/MapReduceAPIServlet.h"
#include "zbase/mapreduce/MapReduceTask.h"

using namespace stx;

namespace zbase {

MapReduceAPIServlet::MapReduceAPIServlet(
    MapReduceService* service,
    ConfigDirectory* cdir,
    const String& cachedir) :
    service_(service),
    cdir_(cdir),
    cachedir_(cachedir) {}

void MapReduceAPIServlet::handle(
    const AnalyticsSession& session,
    RefPtr<stx::http::HTTPRequestStream> req_stream,
    RefPtr<stx::http::HTTPResponseStream> res_stream) {
  const auto& req = req_stream->request();
  URI uri(req.uri());

  http::HTTPResponse res;
  res.populateFromRequest(req);

  if (uri.path() == "/api/v1/mapreduce/execute") {
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session, &uri, &req, &res] {
      executeMapReduceScript(session, uri, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }

  if (uri.path() == "/api/v1/mapreduce/tasks/map_partition") {
    req_stream->readBody();
    catchAndReturnErrors(&res, [this, &session, &uri, &req, &res] {
      executeMapPartitionTask(session, uri, &req, &res);
    });
    res_stream->writeResponse(res);
    return;
  }

  res.setStatus(http::kStatusNotFound);
  res.addHeader("Content-Type", "text/html; charset=utf-8");
  res.addBody(Assets::getAsset("zbase/webui/404.html"));
  res_stream->writeResponse(res);
}

void MapReduceAPIServlet::executeMapPartitionTask(
    const AnalyticsSession& session,
    const URI& uri,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {
  const auto& params = uri.queryParams();

  String table_name;
  if (!URI::getParam(params, "table", &table_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?table=... parameter");
    return;
  }

  String partition_key;
  if (!URI::getParam(params, "partition", &partition_key)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?partition=... parameter");
    return;
  }

  String program_source;
  if (!URI::getParam(params, "program_source", &program_source)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?program_source=... parameter");
    return;
  }

  String method_name;
  if (!URI::getParam(params, "method_name", &method_name)) {
    res->setStatus(http::kStatusBadRequest);
    res->addBody("missing ?method_name=... parameter");
    return;
  }

  auto shard_id = service_->mapPartition(
      session,
      table_name,
      SHA1Hash::fromHexString(partition_key),
      program_source,
      method_name);

  if (shard_id.isEmpty()) {
    res->setStatus(http::kStatusNoContent);
  } else {
    res->setStatus(http::kStatusCreated);
    res->addBody(shard_id.get().toString());
  }
}

void MapReduceAPIServlet::executeMapReduceScript(
    const AnalyticsSession& session,
    const URI& uri,
    const http::HTTPRequest* req,
    http::HTTPResponse* res) {

  auto job_spec = mkRef(new MapReduceJobSpec{});
  job_spec->program_source = req->body().toString();

  service_->executeScript(session, job_spec);

  res->setStatus(http::kStatusCreated);
}

}
