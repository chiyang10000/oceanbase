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

#define USING_LOG_PREFIX ARCHIVE
#include "rootserver/backup/ob_archive_scheduler_service.h"
#include "rootserver/backup/ob_tenant_archive_scheduler.h"
#include "rootserver/ob_server_manager.h"
#include "rootserver/ob_rs_event_history_table_operator.h"
#include "rootserver/ob_unit_manager.h"
#include "storage/tx/ob_ts_mgr.h"
#include "lib/utility/ob_tracepoint.h"
#include "lib/thread/ob_thread_name.h"
#include "share/ob_srv_rpc_proxy.h"
#include "share/backup/ob_backup_lease_info_mgr.h"
#include "share/backup/ob_tenant_archive_round.h"


using namespace oceanbase;
using namespace rootserver;
using namespace common;
using namespace share;
using namespace schema;
using namespace obrpc;

/**
 * ------------------------------ObArchiveThreadIdling---------------------
 */
ObArchiveThreadIdling::ObArchiveThreadIdling(volatile bool &stop)
  : ObThreadIdling(stop), idle_time_us_(MIN_IDLE_INTERVAL_US)
{

}

int64_t ObArchiveThreadIdling::get_idle_interval_us()
{
  return idle_time_us_;
}

void ObArchiveThreadIdling::set_checkpoint_interval(const int64_t interval_us)
{
  const int64_t max_idle_us = interval_us / 2 - RESERVED_FETCH_US;
  int64_t idle_time_us = 0;
  if (interval_us <= 0) {
    idle_time_us = MAX_IDLE_INTERVAL_US;
  } else {
    if (max_idle_us <= MIN_IDLE_INTERVAL_US) {
      idle_time_us = MIN_IDLE_INTERVAL_US;
    } else if (max_idle_us > MAX_IDLE_INTERVAL_US) {
      idle_time_us = MAX_IDLE_INTERVAL_US;
    } else {
      idle_time_us = max_idle_us;
    }
  }

  if (idle_time_us != idle_time_us_) {
    FLOG_INFO("change idle_time_us", K(idle_time_us_), K(idle_time_us));
    idle_time_us_ = idle_time_us;
  }
}


/**
 * ------------------------------ObArchiveSchedulerService---------------------
 */
ObArchiveSchedulerService::ObArchiveSchedulerService()
  : is_inited_(false), is_working_(false), idling_(stop_),
    server_mgr_(nullptr), zone_mgr_(nullptr), unit_mgr_(nullptr),
    rpc_proxy_(nullptr), sql_proxy_(nullptr), schema_service_(nullptr), backup_lease_service_(nullptr)
{

}

int ObArchiveSchedulerService::init(
  ObServerManager &server_mgr,
  ObZoneManager &zone_mgr,
  ObUnitManager &unit_manager,
  share::schema::ObMultiVersionSchemaService *schema_service,
  ObSrvRpcProxy &rpc_proxy,
  common::ObMySQLProxy &sql_proxy,
  share::ObIBackupLeaseService &backup_lease_service)
{
  int ret = OB_SUCCESS;
  const int64_t thread_cnt = 1;

  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("archive scheduler init twice", K(ret));
  } else if (OB_ISNULL(schema_service)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("schema_service is null", K(ret), KP(schema_service));
  } else if (OB_FAIL(create(thread_cnt, "LOG_ARCHIVE_SERVICE"))) {
    LOG_WARN("failed to create log archive thread", K(ret));
  } else {
    server_mgr_ = &server_mgr;
    zone_mgr_ = &zone_mgr;
    unit_mgr_ = &unit_manager;
    schema_service_ = schema_service;
    rpc_proxy_ = &rpc_proxy;
    sql_proxy_ = &sql_proxy;
    backup_lease_service_ = &backup_lease_service;
    is_inited_ = true;
  }

  return ret;
}

int ObArchiveSchedulerService::start()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObReentrantThread::logical_start())) {
    LOG_WARN("failed to start", K(ret));
  } else {
    is_working_ = true;
    LOG_INFO("start archive scheduler service");
  }
  return ret;
}

void ObArchiveSchedulerService::stop()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_ERROR("not init", K(ret));
  } else {
    ObRsReentrantThread::stop();
    idling_.wakeup();
    LOG_INFO("stop archive scheduler service");
  }
}

void ObArchiveSchedulerService::wakeup()
{
  idling_.wakeup();
}

void ObArchiveSchedulerService::run3()
{
  int tmp_ret = OB_SUCCESS;
  int64_t round = 0;
  share::ObLogArchiveStatus::STATUS last_log_archive_status = ObLogArchiveStatus::INVALID;

  lib::set_thread_name("ArcSrv");
  FLOG_INFO("ObArchiveSchedulerService run start");
  if (IS_NOT_INIT) {
    tmp_ret = OB_NOT_INIT;
    LOG_ERROR("not init", K(tmp_ret));
  } else {
    while (true) {
      ++round;
      ObCurTraceId::init(GCONF.self_addr_);
      FLOG_INFO("start do ObArchiveSchedulerService round", K(round));
      if (stop_) {
        tmp_ret = OB_IN_STOP_STATE;
        LOG_WARN("exit for stop state", K(tmp_ret));
        break;
      } else if (OB_SUCCESS != (tmp_ret = process_())) {
        LOG_WARN("failed to do process", K(tmp_ret));
      }

      int64_t checkpoint_interval = 1 * 1000 * 1000L;
      idling_.set_checkpoint_interval(checkpoint_interval);
      if (OB_SUCCESS != (tmp_ret = idling_.idle())) {
        LOG_WARN("failed to to idling", K(tmp_ret));
      }
    }
    is_working_ = false;
  }
  FLOG_INFO("ObArchiveSchedulerService run finish");
}

int ObArchiveSchedulerService::force_cancel(const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  ObArray<uint64_t> archive_tenant_ids;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("archive scheduler not init", K(ret));
  } else if (tenant_id != OB_SYS_TENANT_ID) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("only sys tenant can force cancel archive", K(ret));
  } else if (OB_FAIL(get_all_tenant_ids_(archive_tenant_ids))) {
    LOG_WARN("failed to get all tenant id", K(ret));
  } else if (OB_FAIL(stop_tenant_archive_(archive_tenant_ids, false /* force_stop */))) {
    LOG_WARN("failed to stop tenant archive", K(ret), K(archive_tenant_ids));
  }

  FLOG_WARN("force_cancel archive", K(ret));

  return ret;
}

int ObArchiveSchedulerService::start_archive(const uint64_t tenant_id, const common::ObIArray<uint64_t> &archive_tenant_ids)
{
  int ret = OB_SUCCESS;
  ObArray<uint64_t> bak_archive_tenant_ids;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("archive scheduler not init", K(ret));
  } else if (OB_SYS_TENANT_ID == tenant_id) {
    // If archive_tenant_ids is empty, then start archive for all tenants. Otherwise, just
    // start archive for these tenants in archive_tenant_ids.
    if (archive_tenant_ids.empty()) {
      // No tenants indicated, start archive all tenants, and ignore all error code.
      if (OB_FAIL(get_all_tenant_ids_(bak_archive_tenant_ids))) {
        LOG_WARN("failed to get all tenant ids", K(ret), K(tenant_id));
      } else if (OB_FAIL(start_tenant_archive_(bak_archive_tenant_ids, false /* force_start */))) {
        LOG_WARN("failed to start archive all tenants", K(ret), K(bak_archive_tenant_ids));
      }
    } else {
      // Tenants are indicated, return the first error code which tenant failed to start archive.
      if (OB_FAIL(start_tenant_archive_(archive_tenant_ids, true /* force_start */))) {
        LOG_WARN("failed to start archive indicated tenants", K(ret), K(archive_tenant_ids));
      }
    }
  } else {
    // If start archive is lauched by normal tenant, then archive_tenant_ids must be empty.
    if (!archive_tenant_ids.empty()) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("normal tenant can only archive for itself.", K(ret), K(tenant_id), K(archive_tenant_ids));
    } else if (OB_FAIL(start_tenant_archive_(tenant_id))) {
      LOG_WARN("failed to start archive", K(ret), K(tenant_id));
    }
  }

  return ret;
}

int ObArchiveSchedulerService::stop_archive(const uint64_t tenant_id, const common::ObIArray<uint64_t> &archive_tenant_ids)
{
  int ret = OB_SUCCESS;
  ObArray<uint64_t> bak_archive_tenant_ids;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("archive scheduler not init", K(ret));
  } else if (OB_SYS_TENANT_ID == tenant_id) {
    // If archive_tenant_ids is empty, then stop archive for all tenants. Otherwise, just
    // stop archive for these tenants in archive_tenant_ids.
    if (archive_tenant_ids.empty()) {
      // No tenants indicated, stop archive all tenants, and ignore all error code.
      if (OB_FAIL(get_all_tenant_ids_(bak_archive_tenant_ids))) {
        LOG_WARN("failed to get all tenant ids", K(ret), K(tenant_id));
      } else if (OB_FAIL(stop_tenant_archive_(bak_archive_tenant_ids, false /* force_stop */))) {
        LOG_WARN("failed to stop archive all tenants", K(ret), K(bak_archive_tenant_ids));
      }
    } else {
      // Tenants are indicated, return the first error code which tenant failed to stop archive.
      if (OB_FAIL(stop_tenant_archive_(archive_tenant_ids, true /* force_stop */))) {
        LOG_WARN("failed to stop archive indicated tenants", K(ret), K(archive_tenant_ids));
      }
    }
  } else {
    // If stop archive is lauched by normal tenant, then archive_tenant_ids must be empty.
    if (!archive_tenant_ids.empty()) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("normal tenant can only archive for itself.", K(ret), K(tenant_id), K(archive_tenant_ids));
    } else if (OB_FAIL(stop_tenant_archive_(tenant_id))) {
      LOG_WARN("failed to stop archive", K(ret), K(tenant_id));
    }
  }

  return ret;
}

int ObArchiveSchedulerService::start_tenant_archive_(const ObIArray<uint64_t> &tenant_ids_array, const bool force_start) 
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; i < tenant_ids_array.count(); i++) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = start_tenant_archive_(tenant_ids_array.at(i)))) {
      // set first error code
      if (force_start && OB_SUCC(ret)) {
        ret = tmp_ret;
        LOG_WARN("failed to start archive, set first error code", K(ret));
      }
      LOG_WARN("failed to start archive", K(tmp_ret), K(i));
    } 
  }
  return ret;
}

int ObArchiveSchedulerService::stop_tenant_archive_(const ObIArray<uint64_t> &tenant_ids_array, const bool force_stop) 
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; i < tenant_ids_array.count(); i++) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = stop_tenant_archive_(tenant_ids_array.at(i)))) {
      // set first error code
      if (force_stop && OB_SUCC(ret)) {
        ret = tmp_ret;
        LOG_WARN("failed to stop archive, set first error code", K(ret));
      }
      LOG_WARN("failed to stop archive", K(tmp_ret), K(i));
    } 
  }
  return ret;
}

int ObArchiveSchedulerService::start_tenant_archive_(const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  ObArchiveHandler archive_handler;
  // Only one dest is supported.
  const int64_t dest_no = 0;
  if (OB_FAIL(archive_handler.init(tenant_id, *server_mgr_, *zone_mgr_, *unit_mgr_, schema_service_, *rpc_proxy_, *sql_proxy_))) {
    LOG_WARN("failed to init archive_handler", K(ret));
  } else if (OB_FAIL(archive_handler.enable_archive(dest_no))) {
    LOG_WARN("failed to enable archive tenant", K(ret), K(tenant_id), K(dest_no));
  } else {
    LOG_INFO("enable archive", K(tenant_id), K(dest_no));
  }

  return ret;
}

int ObArchiveSchedulerService::stop_tenant_archive_(const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  ObArchiveHandler archive_handler;
  // Only one dest is supported.
  const int64_t dest_no = 0;
  if (OB_FAIL(archive_handler.init(tenant_id, *server_mgr_, *zone_mgr_, *unit_mgr_, schema_service_, *rpc_proxy_, *sql_proxy_))) {
    LOG_WARN("failed to init archive_handler", K(ret), K(tenant_id));
  } else if (OB_FAIL(archive_handler.disable_archive(dest_no))) {
    LOG_WARN("failed to disable tenant archive", K(ret), K(tenant_id), K(dest_no));
  } else {
    LOG_INFO("disable tenant archive", K(tenant_id), K(dest_no));
  }

  return ret;
}

int ObArchiveSchedulerService::process_()
{
  int ret = OB_SUCCESS;
  // advance archive state.
  ObArray<uint64_t> tenant_id_array;
  ObArray<ObTenantArchiveRoundAttr> tenant_round_array;
  if (OB_FAIL(get_all_tenant_ids_(tenant_id_array))) {
    LOG_WARN("failed to get all meta tenant ids", K(ret));
  }

  // advance normal tenant state first.
  int tmp_ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < tenant_id_array.count(); i++) {
    const uint64_t &tenant_id = tenant_id_array.at(i);
    if (OB_TMP_FAIL(inner_process_(tenant_id))) {
      LOG_WARN("failed to process", K(tmp_ret), K(tenant_id));
    }
  }

  return ret;
}

int ObArchiveSchedulerService::inner_process_(const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  ObArchivePersistHelper archive_op;
  ObArchiveMode archive_mode;
  ObLogArchiveDestState dest_state;
  ObTenantArchiveRoundAttr round;
  const int64_t dest_no = 0;
  const bool lock = false;
  bool no_dest = false;
  bool no_round = false;

  ObArchiveHandler tenant_scheduler;
  if (OB_FAIL(tenant_scheduler.init(tenant_id, *server_mgr_, *zone_mgr_, *unit_mgr_, schema_service_, *rpc_proxy_, *sql_proxy_))) {
    LOG_WARN("failed to init tenant archive scheduler", K(ret), K(tenant_id));
  } else if (OB_TMP_FAIL(tenant_scheduler.checkpoint())) {
    LOG_WARN("failed to checkpoint", K(tmp_ret), K(tenant_id));
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(archive_op.init(tenant_id))) {
    LOG_WARN("failed to init archive_op", K(ret));
  } else if (OB_FAIL(archive_op.get_archive_mode(*sql_proxy_, archive_mode))) {
    LOG_WARN("failed to get archive mode", K(ret), K(tenant_id));
  } else if (!archive_mode.is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("archive mode not valid", K(ret), K(tenant_id), K(archive_mode));
  } else if (OB_FAIL(archive_op.get_dest_state(*sql_proxy_, lock, dest_no, dest_state))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
       LOG_WARN("failed to get dest state", K(ret), K(tenant_id));
    } else {
      // no dest exist
      no_dest =  true;
      ret = OB_SUCCESS;
    }
  } else if (!dest_state.is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dest state not valid", K(ret), K(tenant_id), K(dest_state));
  } else if (OB_FAIL(archive_op.get_round(*sql_proxy_, dest_no, false, round))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
       LOG_WARN("failed to get round", K(ret), K(tenant_id));
    } else {
      // no round exist
      no_round =  true;
      ret = OB_SUCCESS;
    }
  }

  if (OB_FAIL(ret)) {
  } else if (no_dest) {
  } else if (archive_mode.is_noarchivelog()) {
    if (no_round || round.state_.is_stop()) {
    } else if (OB_FAIL(tenant_scheduler.disable_archive(dest_no))) {
      LOG_WARN("failed to disable archive", K(ret), K(tenant_id), K(dest_no), K(dest_state));
    }
  } else if (dest_state.is_defer()) {
    if (no_round || round.state_.is_stop() || round.state_.is_suspend() || round.state_.is_suspending()) {
    } else if (OB_FAIL(tenant_scheduler.defer_archive(dest_no))) {
      LOG_WARN("failed to defer archive", K(ret), K(tenant_id), K(dest_no), K(dest_state));
    }
  } else {
    // dest is enable
    if (no_round || round.state_.is_suspend() || round.state_.is_stop()) {
      if (OB_FAIL(tenant_scheduler.enable_archive(dest_no))) {
        LOG_WARN("failed to enable archive", K(ret), K(tenant_id), K(dest_no), K(dest_state));
      }
    }
  }

  return ret;
}

int ObArchiveSchedulerService::get_all_tenant_ids_(common::ObIArray<uint64_t> &tenantid_array)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard schema_guard;
  ObArray<uint64_t> tmp_tenantid_array;
  if (OB_FAIL(schema_service_->get_tenant_schema_guard(OB_SYS_TENANT_ID, schema_guard))) {
    LOG_WARN("failed to get schema guard", K(ret));
  } else if (OB_FAIL(schema_guard.get_tenant_ids(tmp_tenantid_array))) {
    LOG_WARN("failed to get tenant ids", K(ret));
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < tmp_tenantid_array.count(); ++i) {
    const uint64_t tenant_id = tmp_tenantid_array.at(i);
    const ObTenantSchema *tenant_info = nullptr;
    if (tenant_id < OB_USER_TENANT_ID) {
      // do nothing
    } else if (is_meta_tenant(tenant_id)) {
      // do nothing
    } else if (OB_FAIL(schema_guard.get_tenant_info(tenant_id, tenant_info))) {
      LOG_WARN("failed to get tenant info", K(ret), K(tenant_id));
    } else if (tenant_info->is_restore()) {
      // skip restoring tenant
      LOG_INFO("skip tenant which is doing restore", K(tenant_id));
    } else if (tenant_info->is_creating()) {
      LOG_INFO("skip tenant which is creating", K(tenant_id));
    } else if (tenant_info->is_dropping()) {
      LOG_INFO("skip tenant which is dropping", K(tenant_id));
    } else if (tenant_info->is_in_recyclebin()) {
      LOG_INFO("skip tenant which is recyclebin", K(tenant_id));
    } else if (OB_FAIL(tenantid_array.push_back(tenant_id))) {
      LOG_WARN("failed to push back tenant id", K(ret), K(tenant_id));
    }
  }

  return ret;
}

int ObArchiveSchedulerService::open_archive_mode(const uint64_t tenant_id, const common::ObIArray<uint64_t> &archive_tenant_ids)
{
    // TODO: print error trace to user
  int ret = OB_SUCCESS;
  ObArray<uint64_t> bak_archive_tenant_ids;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("archive scheduler not init", K(ret));
  } else if (OB_SYS_TENANT_ID == tenant_id) {
    // If archive_tenant_ids is empty, then open archive mode for all tenants. Otherwise, just
    // open archive mode for these tenants in archive_tenant_ids.
    if (archive_tenant_ids.empty()) {
      // No tenants indicated, open archive mode for all tenants, and ignore all error code.
      if (OB_FAIL(get_all_tenant_ids_(bak_archive_tenant_ids))) {
        LOG_WARN("failed to get all tenant ids", K(ret), K(tenant_id));
      } else if (OB_FAIL(open_tenant_archive_mode_(bak_archive_tenant_ids))) {
        LOG_WARN("failed to open archive mode for all tenants", K(ret), K(bak_archive_tenant_ids));
      }
    } else {
      if (OB_FAIL(open_tenant_archive_mode_(archive_tenant_ids))) {
        LOG_WARN("failed to open archive mode for indicated tenants", K(ret), K(archive_tenant_ids));
      }
    }
  } else {
    // If open archive mode is lauched by normal tenant, then archive_tenant_ids must be empty.
    if (!archive_tenant_ids.empty()) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("normal tenant can only open archive mode for itself.", K(ret), K(tenant_id), K(archive_tenant_ids));
    } else if (OB_FAIL(open_tenant_archive_mode_(tenant_id))) {
      LOG_WARN("failed to open archive mode", K(ret), K(tenant_id));
    }
  }

  return ret;
}

int ObArchiveSchedulerService::open_tenant_archive_mode_(
    const common::ObIArray<uint64_t> &tenant_ids_array)
{
    // TODO: return failed if any tenant failed
  int ret = OB_SUCCESS;
  for (int64_t i = 0; i < tenant_ids_array.count(); i++) {
    int tmp_ret = OB_SUCCESS;
    const uint64_t &tenant_id = tenant_ids_array.at(i);
    if (OB_TMP_FAIL(open_tenant_archive_mode_(tenant_id))) {
      LOG_WARN("failed to open archive mode", K(tmp_ret), K(i));
    }
  }
  return ret;
}

int ObArchiveSchedulerService::open_tenant_archive_mode_(const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  ObArchiveHandler tenant_scheduler;
  if (OB_FAIL(tenant_scheduler.init(tenant_id, *server_mgr_, *zone_mgr_, *unit_mgr_, schema_service_, *rpc_proxy_, *sql_proxy_))) {
    LOG_WARN("failed to init tenant archive scheduler", K(ret), K(tenant_id));
  } else if (OB_FAIL(tenant_scheduler.open_archive_mode())) {
    LOG_WARN("failed to open archive mode", K(ret), K(tenant_id));
  }
  return ret;
}

int ObArchiveSchedulerService::close_archive_mode(
    const uint64_t tenant_id,
    const common::ObIArray<uint64_t> &archive_tenant_ids)
{
  int ret = OB_SUCCESS;
  ObArray<uint64_t> bak_archive_tenant_ids;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("archive scheduler not init", K(ret));
  } else if (OB_SYS_TENANT_ID == tenant_id) {
    // If archive_tenant_ids is empty, then close archive mode for all tenants. Otherwise, just
    // close archive mode for these tenants in archive_tenant_ids.
    if (archive_tenant_ids.empty()) {
      // No tenants indicated, close archive mode for all tenants, and ignore all error code.
      if (OB_FAIL(get_all_tenant_ids_(bak_archive_tenant_ids))) {
        LOG_WARN("failed to get all tenant ids", K(ret), K(tenant_id));
      } else if (OB_FAIL(close_tenant_archive_mode_(bak_archive_tenant_ids))) {
        LOG_WARN("failed to close archive mode for all tenants", K(ret), K(bak_archive_tenant_ids));
      }
    } else {
      if (OB_FAIL(close_tenant_archive_mode_(archive_tenant_ids))) {
        LOG_WARN("failed to close archive mode for indicated tenants", K(ret), K(archive_tenant_ids));
      }
    }
  } else {
    // If close archive mode is lauched by normal tenant, then archive_tenant_ids must be empty.
    if (!archive_tenant_ids.empty()) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("normal tenant can only close archive mode for itself.", K(ret), K(tenant_id), K(archive_tenant_ids));
    } else if (OB_FAIL(close_tenant_archive_mode_(tenant_id))) {
      LOG_WARN("failed to close archive mode", K(ret), K(tenant_id));
    }
  }

  return ret;
}

int ObArchiveSchedulerService::close_tenant_archive_mode_(const common::ObIArray<uint64_t> &tenant_ids_array)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; i < tenant_ids_array.count(); i++) {
    int tmp_ret = OB_SUCCESS;
    const uint64_t &tenant_id = tenant_ids_array.at(i);
    if (OB_FAIL(close_tenant_archive_mode_(tenant_id))) {
      LOG_WARN("failed to close archive mode", K(tmp_ret), K(i));
    }
  }
  return ret;
}

int ObArchiveSchedulerService::close_tenant_archive_mode_(const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  ObArchiveHandler tenant_scheduler;
  if (OB_FAIL(tenant_scheduler.init(tenant_id, *server_mgr_, *zone_mgr_, *unit_mgr_, schema_service_, *rpc_proxy_, *sql_proxy_))) {
    LOG_WARN("failed to init tenant archive scheduler", K(ret), K(tenant_id));
  } else if (OB_FAIL(tenant_scheduler.close_archive_mode())) {
    LOG_WARN("failed to close archive mode", K(ret), K(tenant_id));
  }
  return ret;
}