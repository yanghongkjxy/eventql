/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#ifndef _CM_TERMINFOTABLESINK_H
#define _CM_TERMINFOTABLESINK_H
#include "reports/Report.h"
#include "common.h"
#include "ItemRef.h"
#include "TermInfo.h"
#include "fnord-sstable/sstablereader.h"
#include "fnord-sstable/sstablewriter.h"
#include "fnord-sstable/SSTableColumnSchema.h"
#include "fnord-sstable/SSTableColumnReader.h"
#include "fnord-sstable/SSTableColumnWriter.h"

using namespace fnord;

namespace cm {

class TermInfoTableSink : public ReportSink {
public:

  TermInfoTableSink(const String& output_file);

  void open();
  void addRow(const String& key, const TermInfo& term_info);
  void close();

  Set<String> outputFiles() override;

protected:
  String output_file_;
  std::unique_ptr<sstable::SSTableWriter> sstable_writer_;
  sstable::SSTableColumnSchema sstable_schema_;
};

} // namespace cm

#endif
