// Copyright (c) 2022-present Oceanbase Inc. All Rights Reserved.
// Author:
//   yiren.ly<yiren.ly@oceanbase.com>

#pragma once

#include "storage/direct_load/ob_direct_load_i_table.h"
#include "storage/direct_load/ob_direct_load_sstable.h"
#include "storage/direct_load/ob_direct_load_table_data_desc.h"

namespace oceanbase
{
namespace storage
{

struct ObDirectLoadSSTableCompactParam
{
public:
  ObDirectLoadSSTableCompactParam();
  ~ObDirectLoadSSTableCompactParam();
  bool is_valid() const;
  TO_STRING_KV(K_(tablet_id), K_(table_data_desc), KP_(datum_utils));
public:
  common::ObTabletID tablet_id_;
  ObDirectLoadTableDataDesc table_data_desc_;
  const blocksstable::ObStorageDatumUtils *datum_utils_;
};

class ObDirectLoadSSTableCompactor : public ObIDirectLoadTabletTableCompactor
{
public:
  ObDirectLoadSSTableCompactor();
  virtual ~ObDirectLoadSSTableCompactor();
  int init(const ObDirectLoadSSTableCompactParam &param);
  int add_table(ObIDirectLoadPartitionTable *table) override;
  int compact() override;
  int get_table(ObIDirectLoadPartitionTable *&table, common::ObIAllocator &allocator) override;
  void stop() override;
private:
  int check_table_compactable(ObDirectLoadSSTable *sstable);
private:
  ObDirectLoadSSTableCompactParam param_;
  int64_t index_item_count_;
  int64_t index_block_count_;
  int64_t row_count_;
  common::ObArray<ObDirectLoadSSTableFragment> fragments_;
  common::ObArenaAllocator start_key_allocator_;
  common::ObArenaAllocator end_key_allocator_;
  blocksstable::ObDatumRowkey start_key_;
  blocksstable::ObDatumRowkey end_key_;
  bool is_inited_;
};

} // namespace storage
} // namespace oceanbase
