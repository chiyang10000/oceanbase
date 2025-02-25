// Copyright (c) 2022 OceanBase
// OceanBase is licensed under Mulan PubL v2.
// You can use this software according to the terms and conditions of the Mulan PubL v2.
// You may obtain a copy of Mulan PubL v2 at:
//          http://license.coscl.org.cn/MulanPubL-2.0
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PubL v2 for more details.

#ifndef OCEANBASE_LOG_META_DATA_SERVICE_H_
#define OCEANBASE_LOG_META_DATA_SERVICE_H_

#include "common/ob_region.h"
#include "lib/mysqlclient/ob_isql_client.h"  // ObISQLClient
#include "lib/allocator/ob_concurrent_fifo_allocator.h"  // ObConcurrentFIFOAllocator
#include "share/backup/ob_backup_struct.h"  // ObBackupPathString
#include "ob_log_fetching_mode.h"           // ClientFetchingMode
#include "ob_log_config.h"
#include "ob_log_task_pool.h"
#include "ob_log_entry_task_pool.h"
#include "ob_log_data_dictionary_in_log_table.h"
#include "ob_log_meta_data_queryer.h"
#include "ob_log_meta_data_baseline_loader.h"
#include "ob_log_meta_data_replayer.h" // ObLogMetaDataReplayer
#include "ob_log_meta_data_fetcher.h"  // ObLogMetaDataFetcher
#include "ob_log_meta_data_fetcher_dispatcher.h"  // ObLogMetaDataFetcherDispatcher

namespace oceanbase
{
namespace libobcdc
{
class IObLogSysLsTaskHandler;
class ObLogSysTableHelper;
class IObLogErrHandler;

class ObLogMetaDataService
{
  // 15 day
  static const int64_t DATADICT_META_RECYCLE_INTERVAL_NS = 15L * 24 * 60 * 60 * 1000 * 1000 * 1000;
public:
  ObLogMetaDataService();
  ~ObLogMetaDataService();

  int init(
      const int64_t start_tstamp_ns,
      const ClientFetchingMode fetching_mode,
      const share::ObBackupPathString &archive_dest,
      IObLogSysLsTaskHandler *sys_ls_handler,
      ObLogSysTableHelper &systable_helper,
      common::ObISQLClient *proxy,
      IObLogErrHandler *err_handler,
      const int64_t cluster_id,
      const ObLogConfig &cfg,
      const int64_t start_seq);
  void destroy();

  static ObLogMetaDataService &get_instance();
  ObLogMetaDataBaselineLoader &get_baseline_loader() { return baseline_loader_; }

public:
  // Refresh baseline meta data based on Tenant ID
  //
  // @param [in]    tenant_id          Tenant ID
  // @param [in]    start_timestamp_ns start timestamp(ns)
  // @param [in]    timeout            Timeout
  //
  // @retval OB_SUCCESS        Success
  // @retval other error code  Fail
  int refresh_baseline_meta_data(
      const uint64_t tenant_id,
      const int64_t start_timestamp_ns,
      const int64_t timeout);

  // Call the function when all tenants are referenced at statup time
  //
  // @retval OB_SUCCESS        Success
  // @retval other error code  Fail
  int finish_when_all_tennats_are_refreshed();

  int read(
      const uint64_t tenant_id,
      datadict::ObDataDictIterator &data_dict_iterator,
      const char *buf,
      const int64_t buf_len,
      const int64_t pos_after_log_header,
      const palf::LSN &lsn,
      const int64_t submit_ts);

  int get_tenant_info_guard(
      const uint64_t tenant_id,
      ObDictTenantInfoGuard &guard);

private:
  int get_data_dict_in_log_info_(
      const uint64_t tenant_id,
      const int64_t start_timestamp_ns,
      DataDictionaryInLogInfo &data_dict_in_log_info);

  int get_data_dict_in_log_info_in_archive_(
      const int64_t start_timestamp_ns,
      DataDictionaryInLogInfo &data_dict_in_log_info);

private:
  bool is_inited_;
  ObLogMetaDataFetcher fetcher_;
  ObLogMetaDataSQLQueryer sql_queryer_;
  ObLogMetaDataBaselineLoader baseline_loader_;
  ObLogMetaDataReplayer incremental_replayer_;
  ObLogMetaDataFetcherDispatcher fetcher_dispatcher_;

  DISALLOW_COPY_AND_ASSIGN(ObLogMetaDataService);
};

#define GLOGMETADATASERVICE (::oceanbase::libobcdc::ObLogMetaDataService::get_instance())

} // namespace libobcdc
} // namespace oceanbase

#endif
