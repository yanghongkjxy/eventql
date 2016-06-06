/**
 * Copyright (c) 2016 zScale Technology GmbH <legal@zscale.io>
 * Authors:
 *   - Paul Asmuth <paul@zscale.io>
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
#include <eventql/io/cstable/cstable_arena.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace cstable {

CSTableArena::CSTableArena(
    BinaryFormatVersion version,
    const TableSchema& schema,
    int fd /* = -1 */) :
    version_(version),
    schema_(schema),
    transaction_id_(0),
    num_rows_(0) {
  FileHeader header;
  header.schema = mkRef(new TableSchema(schema));
  header.columns = header.schema->flatColumns();

  auto header_os = BufferOutputStream::fromBuffer(&file_header_);
  size_t header_size;
  switch (version) {
    case BinaryFormatVersion::v0_1_0:
      RAISE(kIllegalArgumentError, "can't use cstable arenas for v0.1.0 files");
    case BinaryFormatVersion::v0_2_0: {
      header_size = cstable::v0_2_0::writeHeader(header, header_os.get());
      break;
    }
  }

  page_mgr_.reset(new PageManager(fd, header_size, {}));
}

BinaryFormatVersion CSTableArena::getBinaryFormatVersion() const {
  return version_;
}

const TableSchema& CSTableArena::getTableSchema() const {
  return schema_;
}

PageManager* CSTableArena::getPageManager() {
  return page_mgr_.get();
}

const PageManager* CSTableArena::getPageManager() const {
  return page_mgr_.get();
}

void CSTableArena::commitTransaction(
    uint64_t transaction_id,
    uint64_t num_rows) {
  std::unique_lock<std::mutex> lk(mutex_);
  transaction_id_ = transaction_id;
  num_rows_ = num_rows;
}

void CSTableArena::getTransaction(
    uint64_t* transaction_id,
    uint64_t* num_rows) const {
  std::unique_lock<std::mutex> lk(mutex_);
  *transaction_id = transaction_id_;
  *num_rows = num_rows_;
}

void CSTableArena::writeFileHeader(int fd, uint64_t* bytes_written) {
  auto ret = pwrite(fd, file_header_.data(), file_header_.size(), 0);
  if (ret < 0) {
    RAISE_ERRNO(kIOError, "write() failed");
  }
  if (ret != file_header_.size()) {
    RAISE(kIOError, "write() failed");
  }
  *bytes_written = file_header_.size();
}

void CSTableArena::writeFileIndex(int fd, uint64_t* bytes_written) {
  auto file_os = FileOutputStream::fromFileDescriptor(fd);

  *bytes_written = v0_2_0::writeIndex(
      page_mgr_->getPageIndex(),
      file_os.get());
}

void CSTableArena::writeFileTransaction(
    int fd,
    uint64_t index_offset,
    uint64_t index_size) {
  MetaBlock mb;
  mb.transaction_id = transaction_id_;
  mb.num_rows = num_rows_;
  mb.index_offset = index_offset;
  mb.index_size = index_size;

  Buffer buf;
  auto os = BufferOutputStream::fromBuffer(&buf);

  switch (version_) {
    case BinaryFormatVersion::v0_1_0:
      RAISE(kIllegalArgumentError, "unsupported version: v0.1.0");
    case BinaryFormatVersion::v0_2_0:
      cstable::v0_2_0::writeMetaBlock(mb, os.get());
      break;
  }

  // write to metablock slot
  auto meta_block_position = cstable::v0_2_0::kMetaBlockPosition;
  auto meta_block_size = cstable::v0_2_0::kMetaBlockSize;
  auto mb_index = mb.transaction_id % 2;
  auto mb_offset = meta_block_position + meta_block_size * mb_index;
  RCHECK(buf.size() == meta_block_size, "invalid meta block size");
  {
    auto ret = pwrite(fd, buf.data(), buf.size(), mb_offset);
    if (ret < 0) {
      RAISE_ERRNO(kIOError, "write() failed");
    }
    if (ret != meta_block_size) {
      RAISE(kIOError, "write() failed");
    }
  }
}

} // namespace cstable


