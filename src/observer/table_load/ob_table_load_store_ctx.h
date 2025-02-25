// Copyright (c) 2022-present Oceanbase Inc. All Rights Reserved.
// Author:
//   suzhi.yt <suzhi.yt@oceanbase.com>

#pragma once

#include "lib/container/ob_se_array.h"
#include "lib/hash/ob_link_hashmap.h"
#include "observer/table_load/ob_table_load_object_allocator.h"
#include "share/ob_autoincrement_param.h"
#include "share/table/ob_table_load_array.h"
#include "share/table/ob_table_load_define.h"
#include "storage/direct_load/ob_direct_load_table_data_desc.h"

namespace oceanbase
{
namespace storage
{
class ObDirectLoadInsertTableContext;
class ObDirectLoadFastHeapTableContext;
class ObDirectLoadTmpFileManager;
}  // namespace storage
namespace share
{
class ObSequenceCache;
} // namespace share
namespace observer
{
class ObTableLoadTableCtx;
class ObTableLoadStoreTrans;
class ObTableLoadTransCtx;
class ObTableLoadTransStore;
class ObITableLoadTaskScheduler;
class ObTableLoadMerger;
class ObTableLoadErrorRowHandler;

class ObTableLoadStoreCtx
{
public:
  ObTableLoadStoreCtx(ObTableLoadTableCtx *ctx);
  ~ObTableLoadStoreCtx();
  int init(int64_t ddl_task_id,
           const table::ObTableLoadArray<table::ObTableLoadLSIdAndPartitionId> &partition_id_array,
           const table::ObTableLoadArray<table::ObTableLoadLSIdAndPartitionId> &target_partition_id_array);
  void stop();
  void destroy();
  bool is_valid() const { return is_inited_; }
  TO_STRING_KV(K_(is_inited));
public:
  OB_INLINE table::ObTableLoadStatusType get_status() const
  {
    lib::ObMutexGuard guard(mutex_);
    return status_;
  }
  OB_INLINE int get_error_code() const
  {
    lib::ObMutexGuard guard(mutex_);
    return error_code_;
  }
  OB_INLINE int set_status_inited()
  {
    return advance_status(table::ObTableLoadStatusType::INITED);
  }
  OB_INLINE int set_status_loading()
  {
    return advance_status(table::ObTableLoadStatusType::LOADING);
  }
  OB_INLINE int set_status_frozen()
  {
    return advance_status(table::ObTableLoadStatusType::FROZEN);
  }
  OB_INLINE int set_status_merging()
  {
    return advance_status(table::ObTableLoadStatusType::MERGING);
  }
  OB_INLINE int set_status_merged()
  {
    return advance_status(table::ObTableLoadStatusType::MERGED);
  }
  OB_INLINE int set_status_commit()
  {
    return advance_status(table::ObTableLoadStatusType::COMMIT);
  }
  int set_status_error(int error_code);
  int set_status_abort();
  int check_status(table::ObTableLoadStatusType status) const;
private:
  int advance_status(table::ObTableLoadStatusType status);
  int check_status_unlock(table::ObTableLoadStatusType status) const;
public:
  int start_trans(const table::ObTableLoadTransId &trans_id, ObTableLoadStoreTrans *&trans);
  int commit_trans(ObTableLoadStoreTrans *trans);
  int abort_trans(ObTableLoadStoreTrans *trans);
  void put_trans(ObTableLoadStoreTrans *trans);
  int get_trans(const table::ObTableLoadTransId &trans_id, ObTableLoadStoreTrans *&trans);
  int get_trans_ctx(const table::ObTableLoadTransId &trans_id,
                    ObTableLoadTransCtx *&trans_ctx) const;
  int get_active_trans_ids(common::ObIArray<table::ObTableLoadTransId> &trans_id_array) const;
  int get_committed_trans_ids(table::ObTableLoadArray<table::ObTableLoadTransId> &trans_id_array,
                              common::ObIAllocator &allocator) const;
  int get_committed_trans_stores(
    common::ObIArray<ObTableLoadTransStore *> &trans_store_array) const;
  int check_exist_trans(bool &exist) const;
  // release disk space
  void clear_committed_trans_stores();
  int commit();
private:
  int alloc_trans_ctx(const table::ObTableLoadTransId &trans_id, ObTableLoadTransCtx *&trans_ctx);
  int alloc_trans(const table::ObTableLoadTransId &trans_id, ObTableLoadStoreTrans *&trans);
  int get_wa_memory_limit(int64_t &wa_mem_limit);
  int init_session_ctx_array();
  int generate_autoinc_params(share::AutoincParam &autoinc_param);
  int init_sequence();
  int commit_autoinc_value();
public:
  ObTableLoadTableCtx * const ctx_;
  common::ObArray<table::ObTableLoadLSIdAndPartitionId> ls_partition_ids_;
  common::ObArray<table::ObTableLoadLSIdAndPartitionId> target_ls_partition_ids_;
  storage::ObDirectLoadTableDataDesc table_data_desc_;
  table::ObTableLoadResultInfo result_info_;
  ObITableLoadTaskScheduler *task_scheduler_;
  ObTableLoadMerger *merger_;
  storage::ObDirectLoadInsertTableContext *insert_table_ctx_;
  bool is_multiple_mode_;
  bool is_fast_heap_table_;
  storage::ObDirectLoadFastHeapTableContext *fast_heap_table_ctx_;
  storage::ObDirectLoadTmpFileManager *tmp_file_mgr_;
  ObTableLoadErrorRowHandler *error_row_handler_;
  share::schema::ObSequenceSchema sequence_schema_;
  struct SessionContext
  {
    share::AutoincParam autoinc_param_;
  };
  SessionContext *session_ctx_array_;
private:
  struct SegmentCtx : public common::LinkHashValue<table::ObTableLoadSegmentID>
  {
  public:
    SegmentCtx() : segment_id_(0), current_trans_(nullptr), committed_trans_store_(nullptr) {}
    TO_STRING_KV(K_(segment_id), KP_(current_trans), KP_(committed_trans_store));
  public:
    table::ObTableLoadSegmentID segment_id_;
    ObTableLoadStoreTrans *current_trans_;
    ObTableLoadTransStore *committed_trans_store_;
  };
private:
  typedef common::hash::ObHashMap<table::ObTableLoadTransId, ObTableLoadStoreTrans *,
                                  common::hash::NoPthreadDefendMode>
    TransMap;
  typedef common::hash::ObHashMap<table::ObTableLoadTransId, ObTableLoadTransCtx *,
                                  common::hash::NoPthreadDefendMode>
    TransCtxMap;
  typedef common::ObLinkHashMap<table::ObTableLoadSegmentID, SegmentCtx> SegmentCtxMap;
private:
  ObTableLoadObjectAllocator<ObTableLoadStoreTrans> trans_allocator_; // 多线程安全
  mutable lib::ObMutex mutex_;
  common::ObArenaAllocator allocator_;
  table::ObTableLoadStatusType status_;
  int error_code_;
  TransMap trans_map_;
  TransCtxMap trans_ctx_map_;
  SegmentCtxMap segment_ctx_map_;
  common::ObSEArray<ObTableLoadTransStore *, 64> committed_trans_store_array_;
  bool is_inited_;
};

}  // namespace observer
}  // namespace oceanbase
