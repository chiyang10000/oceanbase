/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OB_STORAGE_OB_SSTABLE_ROW_WHOLE_SCANNER_H_
#define OB_STORAGE_OB_SSTABLE_ROW_WHOLE_SCANNER_H_

#include "storage/blocksstable/ob_index_block_macro_iterator.h"
#include "storage/blocksstable/ob_micro_block_row_scanner.h"
#include "storage/blocksstable/ob_macro_block_bare_iterator.h"
#include "ob_store_row_iterator.h"
#include "storage/blocksstable/ob_sstable.h"

namespace oceanbase
{
using namespace blocksstable;
namespace storage
{

class ObSSTableRowWholeScanner : public ObStoreRowIterator
{
private:
  struct MacroScanHandle
  {
  public:
    MacroScanHandle()
      : macro_io_handle_(),
        macro_block_desc_(),
        is_left_border_(false),
        is_right_border_(false) {}
    ~MacroScanHandle() {}
    void reset();

    blocksstable::ObMacroBlockHandle macro_io_handle_;
    blocksstable::ObMacroBlockDesc macro_block_desc_;
    bool is_left_border_;
    bool is_right_border_;
    TO_STRING_KV(K_(macro_io_handle), K_(is_left_border), K_(is_right_border));
  private:
    DISALLOW_COPY_AND_ASSIGN(MacroScanHandle);
  };

public:
  ObSSTableRowWholeScanner()
      : iter_param_(nullptr),
      access_ctx_(nullptr),
      sstable_(nullptr),
      allocator_(common::ObModIds::OB_SSTABLE_READER),
      prefetch_macro_cursor_(0),
      cur_macro_cursor_(0),
      is_macro_prefetch_end_(false),
      macro_block_iter_(),
      micro_block_iter_(),
      micro_scanner_(nullptr),
      is_inited_(false),
      last_micro_block_recycled_(false),
      last_mvcc_row_already_output_(false)
  {}

  virtual ~ObSSTableRowWholeScanner();
  virtual void reset() override;
  int open(
      const ObTableIterParam &iter_param,
      ObTableAccessContext &access_ctx,
      const blocksstable::ObDatumRange &query_range,
      const blocksstable::ObMacroBlockDesc &macro_desc,
      blocksstable::ObSSTable &sstable,
      const bool last_mvcc_row_already_output = false);
  int get_first_row_mvcc_info(bool &is_first_row, bool &is_shadow_row) const;
  INHERIT_TO_STRING_KV("ObStoreRowIterator", ObStoreRowIterator, K_(query_range),
                       K_(prefetch_macro_cursor), K_(cur_macro_cursor), K_(is_macro_prefetch_end),
                       K(ObArrayWrap<MacroScanHandle>(scan_handles_, PREFETCH_DEPTH)),
                       K_(macro_block_iter), K_(micro_block_iter), K_(last_micro_block_recycled),
                       K_(last_mvcc_row_already_output));
protected:
  virtual int inner_open(
      const ObTableIterParam &iter_param,
      ObTableAccessContext &access_ctx,
      ObITable *table,
      const void *query_range) override;
  virtual int inner_get_next_row(const blocksstable::ObDatumRow *&row) override;
  virtual void reuse() override;
private:
  int init_micro_scanner(const blocksstable::ObDatumRange *range);
  int open_macro_block();
  int prefetch();
  int open_micro_block();
  OB_INLINE bool is_multi_version_range(const blocksstable::ObDatumRange &range, const int64_t mv_rowkey_col_cnt) const
  {
    const int64_t max_datum_cnt = MAX(range.get_start_key().get_datum_cnt(), range.get_end_key().get_datum_cnt());
    return range.is_whole_range() || max_datum_cnt == mv_rowkey_col_cnt;
  }
  int check_macro_block_recycle(const ObMacroBlockDesc &macro_desc, bool &can_recycle);
  int check_micro_block_recycle(const ObMicroBlockHeader &micro_header, bool &can_recycle);
  int open_next_valid_micro_block();
  int recycle_last_rowkey_in_micro_block();
private:
  static const int64_t PREFETCH_DEPTH = 2;
  const ObTableIterParam *iter_param_;
  ObTableAccessContext *access_ctx_;
  blocksstable::ObSSTable *sstable_;
  blocksstable::ObDatumRange query_range_;
  common::ObArenaAllocator allocator_;
  int64_t prefetch_macro_cursor_;
  int64_t cur_macro_cursor_;
  bool is_macro_prefetch_end_;
  // for minor merge, check whether the first row of the first rowkey is written in the reused macro block
  blocksstable::ObIndexBlockMacroIterator macro_block_iter_;
  blocksstable::ObMicroBlockBareIterator micro_block_iter_;
  MacroScanHandle scan_handles_[PREFETCH_DEPTH];
  blocksstable::ObIMicroBlockRowScanner *micro_scanner_;
  bool is_inited_;
  bool last_micro_block_recycled_;
  bool last_mvcc_row_already_output_;
};

}
}
#endif //OB_STORAGE_OB_SSTABLE_ROW_WHOLE_SCANNER_V2_H_
