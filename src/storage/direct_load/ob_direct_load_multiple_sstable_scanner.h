// Copyright (c) 2022-present Oceanbase Inc. All Rights Reserved.
// Author:
//   suzhi.yt <suzhi.yt@oceanbase.com>

#pragma once

#include "storage/direct_load/ob_direct_load_i_table.h"
#include "storage/direct_load/ob_direct_load_multiple_datum_row.h"
#include "storage/direct_load/ob_direct_load_multiple_datum_rowkey.h"
#include "storage/direct_load/ob_direct_load_multiple_sstable_data_block_scanner.h"
#include "storage/direct_load/ob_direct_load_sstable_data_block_reader.h"
#include "storage/direct_load/ob_direct_load_table_data_desc.h"

namespace oceanbase
{
namespace storage
{
class ObDirectLoadMultipleSSTable;
class ObDirectLoadMultipleDatumRange;
class ObDirectLoadMultipleDatumRow;

class ObDirectLoadMultipleSSTableScanner
{
  typedef ObDirectLoadMultipleDatumRow RowType;
  typedef ObDirectLoadSSTableDataBlockReader<RowType> DataBlockReader;

public:
  ObDirectLoadMultipleSSTableScanner();
  ~ObDirectLoadMultipleSSTableScanner();
  int init(ObDirectLoadMultipleSSTable *sstable, const ObDirectLoadTableDataDesc &table_data_desc,
           const ObDirectLoadMultipleDatumRange &range,
           const blocksstable::ObStorageDatumUtils *datum_utils);
  int get_next_row(const ObDirectLoadMultipleDatumRow *&datum_row);
  TO_STRING_KV(KPC_(sstable), KPC_(range));

private:
  int switch_next_fragment();

private:
  common::ObArenaAllocator allocator_;
  ObDirectLoadMultipleSSTable *sstable_;
  ObDirectLoadTableDataDesc table_data_desc_;
  const ObDirectLoadMultipleDatumRange *range_;
  const blocksstable::ObStorageDatumUtils *datum_utils_;
  ObDirectLoadMultipleSSTableDataBlockScanner data_block_scanner_;
  ObDirectLoadSSTableDataBlockDesc data_block_desc_;
  DataBlockReader data_block_reader_;
  bool is_iter_start_;
  bool is_iter_end_;
  bool is_inited_;
};

} // namespace storage
} // namespace oceanbase
