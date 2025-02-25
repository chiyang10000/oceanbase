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

#define USING_LOG_PREFIX SHARE_LOCATION

#include "share/location_cache/ob_ls_location_service.h"
#include "share/ob_share_util.h" // ObShareUtil
#include "share/ls/ob_ls_info.h" // ObLSInfo
#include "share/ls/ob_ls_table_operator.h" // ObLSTableOperator
#include "share/ls/ob_ls_status_operator.h" // ObLSStatusOperator
#include "share/cache/ob_cache_name_define.h" // OB_LS_LOCATION_CACHE_NAME
#include "common/ob_timeout_ctx.h" // ObTimeoutCtx
#include "lib/stat/ob_diagnose_info.h" // EVENT_INC
#include "lib/ob_running_mode.h" // lib::is_mini_mode()
#include "share/schema/ob_multi_version_schema_service.h" // ObMultiVersionSchemaService
#include "share/ob_task_define.h" // ObTaskController
#include "observer/ob_server_struct.h"
#include "lib/hash/ob_hashset.h" // ObHashSet
#include "rootserver/ob_rs_async_rpc_proxy.h" // ObGetLeaderLocationsProxy
#include "share/resource_manager/ob_cgroup_ctrl.h" //CGID_DEF

namespace oceanbase
{
using namespace common;
namespace share
{
ObLSLocationUpdateQueueSet::ObLSLocationUpdateQueueSet(
    ObLSLocationService *location_service)
    : inited_(false),
      location_service_(location_service),
      sys_tenant_queue_(),
      meta_tenant_queue_(),
      user_tenant_queue_()
{
}

ObLSLocationUpdateQueueSet::~ObLSLocationUpdateQueueSet()
{
}

int ObLSLocationUpdateQueueSet::init()
{
  int ret = OB_SUCCESS;
  const int64_t user_thread_cnt =
      lib::is_mini_mode()
      ? MINI_MODE_UPDATE_THREAD_CNT
      : static_cast<int64_t>(GCONF.location_refresh_thread_count);
  const int64_t user_queue_size =
      lib::is_mini_mode()
      ? MINI_MODE_USER_TASK_QUEUE_SIZE
      : USER_TASK_QUEUE_SIZE;
  if (inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("queue set has already inited", KR(ret));
  } else if (OB_FAIL(sys_tenant_queue_.init(
      location_service_,
      MINI_MODE_UPDATE_THREAD_CNT,
      LSL_TASK_QUEUE_SIZE,
      "SysLocAsyncUp"))) {
    LOG_WARN("sys_tenant_queue init failed", KR(ret), K(location_service_),
        "thread_cnt", MINI_MODE_UPDATE_THREAD_CNT,
        "queue_size", LSL_TASK_QUEUE_SIZE);
  } else if (OB_FAIL(meta_tenant_queue_.init(
    location_service_,
    MINI_MODE_UPDATE_THREAD_CNT,
    LSL_TASK_QUEUE_SIZE,
    "MetaLocAsyncUp"))) {
    LOG_WARN("meta_tenant_queue init failed", KR(ret), K(location_service_),
        "thread_cnt", MINI_MODE_UPDATE_THREAD_CNT,
        "queue_size", LSL_TASK_QUEUE_SIZE);
  } else if (OB_FAIL(user_tenant_queue_.init(
    location_service_,
    user_thread_cnt,
    user_queue_size,
    "UserLocAsyncUp"))) {
    LOG_WARN("user_tenant_queue init failed",
        KR(ret), K(location_service_), K(user_thread_cnt), K(user_queue_size));
  } else {
    inited_ = true;
  }
  return ret;
}

int ObLSLocationUpdateQueueSet::add_task(const ObLSLocationUpdateTask &task)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (!task.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid task", KR(ret), K(task));
  } else {
    uint64_t tenant_id = task.get_tenant_id();
    ObLSID ls_id = task.get_ls_id();
    if (is_sys_tenant(tenant_id)) { // high priority
      if (OB_FAIL(sys_tenant_queue_.add(task))) {
        if (OB_EAGAIN != ret) {
          LOG_WARN("sys_tenant_queue add_task failed", KR(ret), K(task));
        } else {
          ret = OB_SUCCESS; // same task exist
        }
      }
    } else if (is_meta_tenant(tenant_id)) {
      if (OB_FAIL(meta_tenant_queue_.add(task))) {
        if (OB_EAGAIN != ret) {
          LOG_WARN("meta_tenant_queue add_task failed", KR(ret), K(task));
        } else {
          ret = OB_SUCCESS; // same task exist
        }
      }
    } else {
      if (OB_FAIL(user_tenant_queue_.add(task))) {
        if (OB_EAGAIN != ret) {
          LOG_WARN("user_tenant_queue add_task failed", KR(ret), K(task));
        } else {
          ret = OB_SUCCESS; // same task exist
        }
      }
    }
  }
  return ret;
}

int ObLSLocationUpdateQueueSet::set_thread_count(const int64_t thread_cnt)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (!lib::is_mini_mode() && thread_cnt > 0) {
    if (OB_FAIL(user_tenant_queue_.set_thread_count(thread_cnt))) {
      LOG_WARN("fail to set thread count", KR(ret), K(thread_cnt));
    } else {
      LOG_INFO("location queue may change thread cnt", K(thread_cnt));
    }
  }
  return ret;
}

void ObLSLocationUpdateQueueSet::stop()
{
  sys_tenant_queue_.stop();
  meta_tenant_queue_.stop();
  user_tenant_queue_.stop();
}

void ObLSLocationUpdateQueueSet::wait()
{
  sys_tenant_queue_.wait();
  meta_tenant_queue_.wait();
  user_tenant_queue_.wait();
}

ObLSLocationService::ObLSLocationService()
    : inited_(false),
      stopped_(false),
      lst_(NULL),
      schema_service_(NULL),
      rs_mgr_(NULL),
      srv_rpc_proxy_(NULL),
      inner_cache_(),
      local_async_queue_set_(this),
      remote_async_queue_set_(this),
      ls_loc_timer_(),
      ls_loc_by_rpc_timer_(),
      dump_log_timer_(),
      ls_loc_timer_task_(*this),
      ls_loc_by_rpc_timer_task_(*this),
      dump_cache_timer_task_(*this),
      last_cache_clear_ts_(0)
{
}

ObLSLocationService::~ObLSLocationService()
{
}

int ObLSLocationService::init(
    ObLSTableOperator &lst,
    schema::ObMultiVersionSchemaService &schema_service,
    ObRsMgr &rs_mgr,
    obrpc::ObSrvRpcProxy &srv_rpc_proxy)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", KR(ret));
  } else if (!lst.is_inited()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected argument", KR(ret));
  } else if (OB_FAIL(inner_cache_.init())) {
    LOG_WARN("fail to init inner_cache", KR(ret));
  } else if (OB_FAIL(local_async_queue_set_.init())) {
    LOG_WARN("fail to init local_async_queue_set_", KR(ret));
  } else if (OB_FAIL(remote_async_queue_set_.init())) {
    LOG_WARN("fail to init remote_async_queue_set_", KR(ret));
  } else if (OB_FAIL(ls_loc_timer_.init("AutoLSLoc"))) {
    LOG_WARN("fail to init ls_loc_timer_", KR(ret));
  } else if (OB_FAIL(ls_loc_by_rpc_timer_.init("AutoLSLocRpc"))) {
    LOG_WARN("fail to init ls_loc_by_rpc_timer_", KR(ret));
  } else if (OB_FAIL(dump_log_timer_.init("DumpLSLoc"))) {
    LOG_WARN("fail to init dump_log_timer_", KR(ret));
  } else {
    lst_ = &lst;
    schema_service_ = &schema_service;
    rs_mgr_ = &rs_mgr;
    srv_rpc_proxy_ = &srv_rpc_proxy;
    inited_ = true;
  }
  return ret;
}

int ObLSLocationService::check_inner_stat_() const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObLSLocationService not init", KR(ret));
  } else if (OB_ISNULL(lst_)
             || OB_ISNULL(schema_service_)
             || OB_ISNULL(rs_mgr_)
             || OB_ISNULL(srv_rpc_proxy_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ptr is null", KR(ret), KP_(lst),
             KP_(schema_service), KP_(rs_mgr), KP_(srv_rpc_proxy));
  }
  return ret;
}

int ObLSLocationService::start()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (OB_FAIL(ls_loc_timer_.schedule(
      ls_loc_timer_task_,
      RENEW_LS_LOCATION_INTERVAL_US,
      false/*repeat*/))) {
    LOG_WARN("ObLSLocationService timer schedule ls_loc_timer_task failed", KR(ret));
  } else if (OB_FAIL(ls_loc_by_rpc_timer_.schedule(
      ls_loc_by_rpc_timer_task_,
      RENEW_LS_LOCATION_BY_RPC_INTERVAL_US,
      false/*repeat*/))) {
    LOG_WARN("ObLSLocationService timer schedule ls_loc_by_rpc_timer_task failed", KR(ret));
  } else if (OB_FAIL(dump_log_timer_.schedule(
      dump_cache_timer_task_,
      DUMP_CACHE_INTERVAL_US,
      false/*repeat*/))) {
    LOG_WARN("ObLSLocationService timer schedule dump_cache_timer_task failed", KR(ret));
  }
  return ret;
}

int ObLSLocationService::get(
    const int64_t cluster_id,
    const uint64_t tenant_id,
    const ObLSID &ls_id,
    const int64_t expire_renew_time,
    bool &is_cache_hit,
    ObLSLocation &location)
{
  int ret = OB_SUCCESS;
  location.reset();
  is_cache_hit = false;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if(!is_valid_key(cluster_id, tenant_id, ls_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid key for get",
        KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else {
    ret = get_from_cache(cluster_id, tenant_id, ls_id, location);
    if (OB_SUCCESS != ret && OB_CACHE_NOT_HIT != ret) {
      LOG_WARN("get location from cache failed",
          KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
    } else if (OB_CACHE_NOT_HIT == ret
        || location.get_renew_time() <= expire_renew_time) {
      if (OB_FAIL(renew_location(cluster_id, tenant_id, ls_id, location))) {
        LOG_WARN("renew location failed",
            KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
      }
    } else { // valid cache
      is_cache_hit = true;
    }
  }
  if (OB_SUCC(ret) && is_cache_hit) {
    EVENT_INC(LOCATION_CACHE_HIT);
  } else {
    EVENT_INC(LOCATION_CACHE_MISS);
  }
  return ret;
}

int ObLSLocationService::get_leader(
      const int64_t cluster_id,
      const uint64_t tenant_id,
      const ObLSID &ls_id,
      const bool force_renew,
      common::ObAddr &leader)
{
  int ret = OB_SUCCESS;
  bool is_cache_hit = false;
  int64_t expire_renew_time = force_renew ? INT64_MAX : 0;
  ObLSLocation location;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (!is_valid_key(cluster_id, tenant_id, ls_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid key for get",
        KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else if (OB_FAIL(get(
      cluster_id,
      tenant_id,
      ls_id,
      expire_renew_time,
      is_cache_hit,
      location))) {
    LOG_WARN("fail to get location",
        KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else if (OB_FAIL(location.get_leader(leader))) {
    LOG_WARN("fail to get leader from location",
        KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  }
  return ret;
}

int ObLSLocationService::get_leader_with_retry_until_timeout(
    const int64_t cluster_id,
    const uint64_t tenant_id,
    const ObLSID &ls_id,
    common::ObAddr &leader,
    const int64_t abs_retry_timeout,
    const int64_t retry_interval)
{
  int ret = OB_SUCCESS;
  leader.reset();
  ObLSLocation location;
  ObTimeoutCtx ctx;
  int64_t curr_abs_retry_timeout_ts = abs_retry_timeout;
  const int64_t DEFAULT_RETRY_TIMEOUT = GCONF.location_cache_refresh_sql_timeout;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (OB_UNLIKELY(!is_valid_key(cluster_id, tenant_id, ls_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid key", KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else if (OB_UNLIKELY(abs_retry_timeout < 0 || retry_interval < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", KR(ret), K(abs_retry_timeout), K(retry_interval));
  } else if (0 == abs_retry_timeout) {
    if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(ctx, DEFAULT_RETRY_TIMEOUT))) {
      LOG_WARN("fail to set default_timeout_ctx", KR(ret));
    } else {
      curr_abs_retry_timeout_ts = ctx.get_abs_timeout();
    }
  }
  if (OB_SUCC(ret)) {
    do {
      if (OB_FAIL(nonblock_get_leader(cluster_id, tenant_id, ls_id, leader))) {
        if (is_location_service_renew_error(ret)) {
          int tmp_ret = OB_SUCCESS;
          if (OB_SUCCESS != (tmp_ret = nonblock_renew(cluster_id, tenant_id, ls_id))) {
            LOG_WARN("nonblock renew failed", KR(tmp_ret), K(ls_id), K(cluster_id));
          } else if (ObTimeUtil::current_time() + retry_interval > curr_abs_retry_timeout_ts) {
            break;
          } else {
            ob_usleep(retry_interval);
          }
        } else {
          LOG_WARN("fail to nonblock_get_leader", KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
        }
      }
    } while (is_location_service_renew_error(ret));
  }
  return ret;
}

int ObLSLocationService::nonblock_get(
    const int64_t cluster_id,
    const uint64_t tenant_id,
    const ObLSID &ls_id,
    ObLSLocation &location)
{
  int ret = OB_SUCCESS;
  location.reset();
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (!is_valid_key(cluster_id, tenant_id, ls_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid key for get",
        KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else {
    ret = get_from_cache(cluster_id, tenant_id, ls_id, location);
    if (OB_SUCCESS != ret && OB_CACHE_NOT_HIT != ret) {
      LOG_WARN("get location from cache failed",
          KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
    }
    if (OB_SUCC(ret)) {
      EVENT_INC(LOCATION_CACHE_NONBLOCK_HIT);
    } else if (OB_CACHE_NOT_HIT == ret) {
      ret = OB_LS_LOCATION_NOT_EXIST;
      EVENT_INC(LOCATION_CACHE_NONBLOCK_MISS);
    }
  }
  return ret;
}

int ObLSLocationService::nonblock_get_leader(
    const int64_t cluster_id,
    const uint64_t tenant_id,
    const ObLSID &ls_id,
    common::ObAddr &leader)
{
  int ret = OB_SUCCESS;
  ObLSLocation location;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (!is_valid_key(cluster_id, tenant_id, ls_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid key for get",
        KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else if (OB_FAIL(nonblock_get(cluster_id, tenant_id, ls_id, location))) {
      LOG_WARN("nonblock get location failed",
          KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else if (OB_FAIL(location.get_leader(leader))) {
    LOG_WARN("get leader from location failed", KR(ret), K(location));
  }
  return ret;
}

int ObLSLocationService::nonblock_renew(
    const int64_t cluster_id,
    const uint64_t tenant_id,
    const ObLSID &ls_id)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (!is_valid_key(cluster_id, tenant_id, ls_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid log stream key",
        KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else {
    const int64_t now = ObTimeUtility::current_time();
    ObLSLocationUpdateTask task(cluster_id, tenant_id, ls_id, now);
    if (OB_FAIL(add_update_task(task))) {
      LOG_WARN("add location update task failed", KR(ret), K(task));
    }
  }
  return ret;
}

int ObLSLocationService::add_update_task(const ObLSLocationUpdateTask &task)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (!task.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid task", KR(ret), K(task));
  } else if (GCONF.cluster_id == task.get_cluster_id()) {
    if (OB_FAIL(local_async_queue_set_.add_task(task))) {
      LOG_WARN("fail to add task", KR(ret), K(task));
    } else {
      LOG_TRACE("add update task in local_async_queue_set_", KR(ret), K(task));
    }
  } else {
    if (OB_FAIL(remote_async_queue_set_.add_task(task))) {
      LOG_WARN("fail to add task", KR(ret), K(task));
    } else {
      LOG_TRACE("add update task in remote_async_queue_set_", KR(ret), K(task));
    }
  }
  return ret;
}

int ObLSLocationService::batch_process_tasks(
    const common::ObIArray<ObLSLocationUpdateTask> &tasks,
    bool &stopped)
{
  int ret = OB_SUCCESS;
  ObCurTraceId::init(GCONF.self_addr_);
  UNUSED(stopped);
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (1 != tasks.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected task count", KR(ret), "tasks count", tasks.count());
  } else {
    const uint64_t tenant_id = tasks.at(0).get_tenant_id();
    const uint64_t superior_tenant_id = get_private_table_exec_tenant_id(tenant_id);
    ObLSLocation location;
    if (OB_ISNULL(GCTX.schema_service_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("GCTX.schema_service_ is null", KR(ret));
    } else if (!GCTX.schema_service_->is_tenant_full_schema(superior_tenant_id)) {
      // do not process tasks if tenant schema is not ready
      if (REACH_TIME_INTERVAL(1000 * 1000L)) { // 1s
        LOG_WARN("tenant schema is not ready, need wait", KR(ret), K(superior_tenant_id), K(tasks));
      }
    } else if (OB_FAIL(renew_location(
        tasks.at(0).get_cluster_id(),
        tasks.at(0).get_tenant_id(),
        tasks.at(0).get_ls_id(),
        location))) {
      LOG_WARN("fail to renew location", KR(ret), "task", tasks.at(0));
    }
  }
  return ret;
}

int ObLSLocationService::process_barrier(
    const ObLSLocationUpdateTask &task,
    bool &stopped)
{
  UNUSEDx(task, stopped);
  return OB_NOT_SUPPORTED;
}

void ObLSLocationService::stop()
{
  local_async_queue_set_.stop();
  remote_async_queue_set_.stop();
  ls_loc_timer_.stop();
  ls_loc_by_rpc_timer_.stop();
  dump_log_timer_.stop();
}

void ObLSLocationService::wait()
{
  local_async_queue_set_.wait();
  remote_async_queue_set_.wait();
  ls_loc_timer_.wait();
  ls_loc_by_rpc_timer_.wait();
  dump_log_timer_.wait();
}

int ObLSLocationService::destroy()
{
  int ret = OB_SUCCESS;
  ls_loc_timer_.destroy();
  ls_loc_by_rpc_timer_.destroy();
  dump_log_timer_.destroy();
  inner_cache_.destroy();
  last_cache_clear_ts_ = 0;
  stopped_ = true;
  inited_ = false;
  return ret;
}


int ObLSLocationService::reload_config()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else {
    const int64_t thread_cnt = GCONF.location_refresh_thread_count;
    if (OB_FAIL(local_async_queue_set_.set_thread_count(thread_cnt))) {
      LOG_WARN("local_async_queue_set set thread count failed",
          KR(ret), K(thread_cnt));
    } else if (OB_FAIL(remote_async_queue_set_.set_thread_count(thread_cnt))) {
      LOG_WARN("remote_async_queue_set set thread count failed",
          KR(ret), K(thread_cnt));
    }
  }
  return ret;
}

//FIXME: Not used. The GC logic needs to be reconsidered. Should not rely on __all_ls_status.
int ObLSLocationService::build_tenant_ls_info_hash(ObTenantLsInfoHashMap &hash)
{
  int ret = OB_SUCCESS;
  ObArray<uint64_t> tenant_ids;

  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (OB_FAIL(schema_service_->get_tenant_ids(tenant_ids))) {
    LOG_WARN("get tenant_ids failed", KR(ret));
  } else {
    // get all tenant id
    for (int64_t i = 0; OB_SUCC(ret) && i < tenant_ids.count(); ++i) {
      const uint64_t tenant_id = tenant_ids.at(i);
      ObLSStatusOperator ls_op;
      ObLSStatusInfoArray ls_status_arr;
      // get all ls status info of current tenant
      if (!is_valid_tenant_id(tenant_id)
          || is_virtual_tenant_id(tenant_id)
          || is_sys_tenant(tenant_id)) {
        // skip invalid tenants and sys tenant
      } else if (OB_FAIL(ls_op.get_all_ls_status_by_order(
          tenant_ids.at(i),
          ls_status_arr,
          *GCTX.sql_proxy_))) {
        LOG_WARN("get all ls status by order error", KR(ret), K(tenant_id));
      } else {
        // build hash for current tenant
        for (int64_t j = 0; OB_SUCC(ret) && j < ls_status_arr.count(); ++j) {
          ObTenantLSInfoKey key(tenant_ids.at(i), ls_status_arr.at(j).ls_id_);
          if (OB_FAIL(hash.set_refactored(key, true/*exist*/, true/*overwrite*/))) {
            LOG_WARN("hashmap set refactored error", KR(ret), K(key));
          }
        }
      }
    }
  }

  return ret;
}

//FIXME: Not used. The GC logic needs to be reconsidered
int ObLSLocationService::check_and_clear_dead_cache()
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (common::ObClockGenerator::getClock() - last_cache_clear_ts_ < 1800 * 1000 * 1000 /*30min*/) {
    // do nothing
  } else {
    last_cache_clear_ts_ = common::ObClockGenerator::getClock();
    ObLSLocationArray total_arr;
    ObTenantLsInfoHashMap hash;
    if (OB_FAIL(inner_cache_.check_and_generate_dead_cache(total_arr))) {
      LOG_WARN("check and generate dead cache error", KR(ret));
    } else if (total_arr.count() <= 0) {
      LOG_INFO("no dead cache need to clear", K(total_arr));
    } else if (OB_FAIL(build_tenant_ls_info_hash(hash))) {
      LOG_WARN("build tenant ls info hash error", KR(ret), K(total_arr));
    } else {
      LOG_INFO("start to clear dead cache");
      // ignore ret
      bool exist = false;
      for (int64_t i = 0; i < total_arr.count(); ++i) {
        const ObLSLocationCacheKey &ls_cache_key = total_arr.at(i).get_cache_key();
        ObTenantLSInfoKey key(ls_cache_key.get_tenant_id(),
                              ls_cache_key.get_ls_id());
        if (is_sys_tenant(ls_cache_key.get_tenant_id())) {
          // do not clear sys tenant ls location cache
        } else if (OB_FAIL(hash.get_refactored(key, exist))) {
          if (OB_HASH_NOT_EXIST == ret) {
            if (OB_FAIL(inner_cache_.del(ls_cache_key))) {
              LOG_WARN("inner cache del error", KR(ret), "ls_location", total_arr.at(i));
            } else {
              LOG_INFO("del ls location cache succ", "ls_location_cache", total_arr.at(i));
            }
          } else {
            LOG_WARN("fail to get_refactored", KR(ret), K(key));
          }
        } else {
          // cache hit, do nothing
        }
      }
      LOG_INFO("clear dead cache finish");
    }
  }

  return ret;
}

// Attention: can not detect ls deletion
int ObLSLocationService::renew_all_ls_locations()
{
  ObCurTraceId::init(GCONF.self_addr_);
  int ret = OB_SUCCESS;
  int ret_fail = OB_SUCCESS;
  ObArray<ObLSInfo> ls_infos;
  ObArray<uint64_t> tenant_ids;
  const bool can_erase = true;
  const int64_t renew_all_ls_loc_timeout = GCONF.location_cache_refresh_sql_timeout;
  const bool inner_table_only = false;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (OB_FAIL(schema_service_->get_tenant_ids(tenant_ids))) {
    LOG_WARN("get tenant_ids failed", KR(ret));
  } else {
    ARRAY_FOREACH_NORET(tenant_ids, idx) {
      // ignore ret to ensure that each tenant's renewing is independent.
      ret = OB_SUCCESS;
      ls_infos.reset();
      const uint64_t tenant_id = tenant_ids.at(idx);
      ObTimeoutCtx ctx;
      if (!is_valid_tenant_id(tenant_id)
          || is_virtual_tenant_id(tenant_id)) {
        continue;
      } else if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(
              ctx,
              renew_all_ls_loc_timeout))) {
        LOG_WARN("fail to set default_timeout_ctx", KR(ret));
      } else if (OB_FAIL(lst_->get_by_tenant(tenant_id, inner_table_only, ls_infos))) {
        LOG_WARN("fail to get all ls info", KR(ret), K(tenant_id), K(ls_infos));
      } else {
        ARRAY_FOREACH_N(ls_infos, i, cnt) {
          const ObLSInfo &ls_info = ls_infos.at(i);
          ObLSLocation old_location;
          ObLSLocation new_location;
          bool is_same = false;
          int tmp_ret = OB_SUCCESS;
          // get from cache does not affect renew process
          if (OB_SUCCESS != (tmp_ret = get_from_cache(
              GCONF.cluster_id,
              ls_info.get_tenant_id(),
              ls_info.get_ls_id(),
              old_location))) {
            if (OB_CACHE_NOT_HIT == tmp_ret) {
              tmp_ret = OB_SUCCESS;
            } else {
              LOG_WARN("fail to get from cache", KR(tmp_ret), K(ls_info));
            }
          }
          if (OB_FAIL(fill_location(GCONF.cluster_id, ls_info, new_location))) {
            LOG_WARN("fail to fill location", KR(ret), K(ls_info));
          } else if (OB_FAIL(update_cache(
              GCONF.cluster_id,
              new_location.get_tenant_id(),
              new_location.get_ls_id(),
              can_erase,
              new_location))) {
            LOG_WARN("fail to update cache", KR(ret), K(tenant_id), K(new_location));
          }
          if (OB_SUCC(ret) && (OB_SUCCESS == tmp_ret) && !new_location.is_same_with(old_location)) {
            FLOG_INFO("[LS_LOCATION]ls location cache has changed", KR(ret), K(old_location), K(new_location));
          }
        }
      }
      if (OB_FAIL(ret)) {
        ret_fail = ret;
      }
    } // end ARRAY_FOREACH_NORET
    ret = ret_fail;
  }
  return ret;
}

int ObLSLocationService::renew_all_ls_locations_by_rpc()
{
  ObCurTraceId::init(GCONF.self_addr_);
  int ret = OB_SUCCESS;
  ObArray<ObAddr> dests;
  ObArray<ObLSLeaderLocation> leaders;
  const bool from_rpc = true;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (GET_MIN_CLUSTER_VERSION() < CLUSTER_VERSION_4_1_0_0) {
    // for rpc compatibility
  } else if (OB_FAIL(construct_rpc_dests_(dests))) {
    LOG_WARN("fail to get rpc dests", KR(ret));
  } else if (dests.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("dests count is less than 0", KR(ret));
  } else if (OB_FAIL(detect_ls_leaders_(dests, leaders))) {
    LOG_WARN("fail to detect ls leaders", KR(ret), K(dests));
  } else {
    ObLSLocation location;
    for (int64_t i = 0; OB_SUCC(ret) && i < leaders.count(); i++) {
      const ObLSLeaderLocation &leader = leaders.at(i);
      location.reset();
      if (OB_FAIL(location.init(leader.get_key().get_cluster_id(),
                                leader.get_key().get_tenant_id(),
                                leader.get_key().get_ls_id(),
                                ObTimeUtility::current_time()))) {
        LOG_WARN("fail to init location", KR(ret), K(leader));
      } else if (OB_FAIL(location.add_replica_location(leader.get_location()))) {
        LOG_WARN("fail to add replica", KR(ret), K(leader));
      } else if (OB_FAIL(inner_cache_.update(from_rpc, leader.get_key(), location))) {
        LOG_WARN("fail to update location", KR(ret), K(leader));
      }
    } // end for
    if (REACH_TIME_INTERVAL(10 * 1000 * 1000L)) { // 10s
      FLOG_INFO("[LS_LOCATION] Get ls leaders by RPC", KR(ret), K(dests), K(leaders));
    }
  }
  return ret;
}

int ObLSLocationService::construct_rpc_dests_(
    ObIArray<ObAddr> &dests)
{
  int ret = OB_SUCCESS;
  ObArray<ObAddr> rs_list;
  ObArray<ObAddr> all_server_list;
  const bool check_ls_service = false;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (OB_FAIL(rs_mgr_->construct_initial_server_list(check_ls_service, rs_list))) {
    LOG_WARN("fail to get rs list", KR(ret));
  } else if (OB_FAIL(rs_mgr_->construct_all_server_list(rs_list, all_server_list))) {
    LOG_WARN("fail to get all server list", KR(ret));
  } else if (OB_FAIL(dests.assign(rs_list))) {
    LOG_WARN("fail to assign rs_list", KR(ret));
  } else if (OB_FAIL(append(dests, all_server_list))) {
    LOG_WARN("fail to append array", KR(ret), K(dests), K(all_server_list));
  }
  return ret;
}

int ObLSLocationService::detect_ls_leaders_(
    const ObIArray<ObAddr> &dests,
    ObArray<ObLSLeaderLocation> &leaders)
{
  int ret = OB_SUCCESS;
  leaders.reset();
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else {
    ObTimeoutCtx ctx;
    int64_t timeout = GCONF.rpc_timeout;  // default value is 2s
    int tmp_ret = share::ObShareUtil::set_default_timeout_ctx(ctx, timeout);
    timeout = max(timeout, ctx.get_timeout());  // at least 2s

    rootserver::ObGetLeaderLocationsProxy proxy(
        *srv_rpc_proxy_, &obrpc::ObSrvRpcProxy::get_leader_locations);
    obrpc::ObGetLeaderLocationsArg arg;
    arg.set_addr(GCTX.self_addr());

    for (int64_t i = 0; i < dests.count(); i++) { //ignore ret
      const ObAddr &addr = dests.at(i);
      if (OB_TMP_FAIL(proxy.call(addr, timeout, GCONF.cluster_id,
          OB_SYS_TENANT_ID, share::OBCG_LOC_CACHE, arg))) {
        LOG_WARN("fail to send rpc", KR(tmp_ret), K(addr), K(timeout));
      }
    } // end for

    ObArray<int> return_ret_array;
    if (OB_TMP_FAIL(proxy.wait_all(return_ret_array))) { // ignore ret
      LOG_WARN("wait batch result failed", KR(tmp_ret), KR(ret));
      ret = OB_SUCC(ret) ? tmp_ret : ret;
    } else {
      ObAddr invalid_addr;
      for (int64_t i = 0; OB_SUCC(ret) && i < return_ret_array.count(); i++) {
        int return_ret = return_ret_array.at(i);
        const obrpc::ObGetLeaderLocationsResult *result = proxy.get_results().at(i);
        if (OB_SUCCESS == return_ret) {
          if (OB_NOT_NULL(result)) {
            if (OB_FAIL(append(leaders, result->get_leader_replicas()))) {
              LOG_WARN("fail to append array", KR(ret), KPC(result));
            }
          } else {
            LOG_TRACE("result is null", K(i), K(timeout));
          }
        } else {
          LOG_TRACE("fail to detect ls leader", "ret", return_ret, K(timeout),
                    "addr", OB_ISNULL(result) ? invalid_addr : result->get_addr());
        }
      } // end for
    }


  }
  return ret;
}

bool ObLSLocationService::is_valid_key(
    const int64_t cluster_id,
    const uint64_t tenant_id,
    const ObLSID &ls_id)
{
  return OB_INVALID_CLUSTER_ID != cluster_id
      && OB_INVALID_TENANT_ID != tenant_id
      && ls_id.is_valid_with_tenant(tenant_id);
}

int ObLSLocationService::get_from_cache(
    const int64_t cluster_id,
    const uint64_t tenant_id,
    const ObLSID &ls_id,
    ObLSLocation &location)
{
  int ret = OB_SUCCESS;
  ObLSLocationCacheKey cache_key(cluster_id, tenant_id, ls_id);
  location.reset();
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if(OB_UNLIKELY(!cache_key.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else  if (OB_FAIL(inner_cache_.get(cache_key, location))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_CACHE_NOT_HIT;
      LOG_TRACE("location is not hit in inner cache", KR(ret), K(cache_key));
    } else {
      LOG_WARN("get location from inner cache failed", K(cache_key), KR(ret));
    }
  } else {
    LOG_TRACE("location hit in inner cache", KR(ret), K(cache_key), K(location));
  }
  return ret;
}

int ObLSLocationService::renew_location(
    const int64_t cluster_id,
    const uint64_t tenant_id,
    const ObLSID &ls_id,
    ObLSLocation &location)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  ObLSLocation old_location;
  ObTimeoutCtx ctx;
  location.reset();
  ObLSInfo ls_info;
  const bool can_erase = true;
  int64_t default_timeout = GCONF.location_cache_refresh_sql_timeout;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (!is_valid_key(cluster_id, tenant_id, ls_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else if (OB_ISNULL(lst_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("lst_ is null", KR(ret));
  }

  // get from cache just for printing cache changes log
  if (OB_FAIL(ret)) {
  } else if (OB_SUCCESS != (tmp_ret = get_from_cache(cluster_id, tenant_id, ls_id, old_location))) {
    if (OB_CACHE_NOT_HIT == tmp_ret) {
      tmp_ret = OB_SUCCESS;
    } else {
      LOG_WARN("fail to get from cache", KR(tmp_ret), K(cluster_id), K(tenant_id), K(ls_id));
    }
  }

  if (FAILEDx(ObShareUtil::set_default_timeout_ctx(ctx, default_timeout))) {
    LOG_WARN("fail to set default_timeout_ctx", KR(ret));
  } else if (OB_FAIL(lst_->get(cluster_id, tenant_id,
             ls_id, share::ObLSTable::DEFAULT_MODE, ls_info))) {
    LOG_WARN("fail to get log stream info by operator",
        KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
    if (ObLocationServiceUtility::treat_sql_as_timeout(ret)) {
      ret = OB_GET_LOCATION_TIME_OUT;
    }
  } else if (OB_FAIL(fill_location(cluster_id, ls_info, location))) {
    LOG_WARN("fail to fill location", KR(ret), K(ls_info));
  } else if (OB_FAIL(update_cache(cluster_id, tenant_id, ls_id, can_erase, location))) {
    LOG_WARN("fail to update cache", KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else if (location.get_replica_locations().count() < 1) {
    ret = OB_LS_LOCATION_NOT_EXIST;
    LOG_WARN("get empty location from meta table", KR(ret), K(location));
  }
  // print cache changes
  if (OB_SUCC(ret) && (OB_SUCCESS == tmp_ret) && !location.is_same_with(old_location)) {
    FLOG_INFO("[LS_LOCATION]ls location cache has changed", KR(ret), K(old_location), "new_location", location);
  }
  return ret;
}

int ObLSLocationService::fill_location(
    const int64_t cluster_id,
    const ObLSInfo &ls_info,
    ObLSLocation &location)
{
  int ret = OB_SUCCESS;
  location.reset();
  const ObIArray<ObLSReplica> &replicas = ls_info.get_replicas();
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (!ls_info.is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid ls_info", KR(ret), K(ls_info));
  } else if (OB_FAIL(location.init(cluster_id,
                                   ls_info.get_tenant_id(),
                                   ls_info.get_ls_id(),
                                   ObTimeUtility::current_time()))) {
    LOG_WARN("location init error", KR(ret), K(cluster_id), K(ls_info), K(location));
  } else {
    ObLSReplicaLocation replica_location;
    for(int64_t i = 0; OB_SUCC(ret) && i < replicas.count(); ++i) {
      if (!replicas.at(i).is_valid()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("replica is not valid", KR(ret), "replica", replicas.at(i));
      } else {
        replica_location.reset();
        if(OB_FAIL(replica_location.init(
            replicas.at(i).get_server(),
            replicas.at(i).get_role(),
            replicas.at(i).get_sql_port(),
            replicas.at(i).get_replica_type(),
            replicas.at(i).get_property(),
            replicas.at(i).get_restore_status(),
            replicas.at(i).get_proposal_id()))) {
          LOG_WARN("fail to init", KR(ret));
        } else if (OB_FAIL(location.add_replica_location(replica_location))) {
          LOG_WARN("fail to add replica locaiton", KR(ret), K(replica_location));
        }
      }
    }
  }
  return ret;
}

int ObLSLocationService::update_cache(
    const int64_t cluster_id,
    const uint64_t tenant_id,
    const ObLSID &ls_id,
    const bool can_erase,
    ObLSLocation &location)
{
  int ret = OB_SUCCESS;
  ObLSLocationCacheKey cache_key(cluster_id, tenant_id, ls_id);
  const bool from_rpc = false;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (!cache_key.is_valid() || !location.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(cache_key), K(location));
  } else if (location.get_replica_locations().count() < 1) {
    if (!can_erase) {
      ret = OB_LS_LOCATION_NOT_EXIST;
      LOG_WARN("location is empty", KR(ret),
          K(cluster_id), K(tenant_id), K(ls_id), K(can_erase), K(location));
    } else if (OB_FAIL(erase_location(cluster_id, tenant_id, ls_id))) {
      LOG_WARN("fail to erase location", KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
    }
  } else if (OB_FAIL(inner_cache_.update(from_rpc, cache_key, location))) {
    LOG_WARN("put location to user location cache failed",
        KR(ret), K(from_rpc), K(cache_key), K(location));
  } else {
    LOG_TRACE("renew location in inner_cache succeed",
        KR(ret), K(from_rpc), K(cache_key), K(location));
  }
  return ret;
}

int ObLSLocationService::erase_location(
    const int64_t cluster_id,
    const uint64_t tenant_id,
    const ObLSID &ls_id)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (!is_valid_key(cluster_id, tenant_id, ls_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(cluster_id), K(tenant_id), K(ls_id));
  } else if (is_sys_tenant(tenant_id)) {
    // location of sys ls shouldn't be erased
  } else {
    ObLSLocationCacheKey cache_key(cluster_id, tenant_id, ls_id);
    if (OB_FAIL(inner_cache_.del(cache_key))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        ret = OB_SUCCESS;
        LOG_TRACE("not exist in inner_cache_", K(cache_key));
      } else {
        LOG_WARN("fail to erase location from inner_cache_", KR(ret), K(cache_key));
      }
    } else {
      LOG_TRACE("erase location from inner_cache_", K(cache_key));
    }
  }
  return ret;
}

int ObLSLocationService::schedule_ls_timer_task()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (OB_FAIL(ls_loc_timer_.schedule(
      ls_loc_timer_task_,
      RENEW_LS_LOCATION_INTERVAL_US,
      false/*repeat*/))) {
    LOG_WARN("fail to schedule ls location timer task", KR(ret));
  }
  return ret;
}

int ObLSLocationService::schedule_ls_by_rpc_timer_task()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (OB_FAIL(ls_loc_by_rpc_timer_.schedule(
      ls_loc_by_rpc_timer_task_,
      RENEW_LS_LOCATION_BY_RPC_INTERVAL_US,
      false/*repeat*/))) {
    LOG_WARN("fail to schedule ls location timer task", KR(ret));
  }
  return ret;
}

int ObLSLocationService::schedule_dump_cache_timer_task()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (OB_FAIL(dump_log_timer_.schedule(
      dump_cache_timer_task_,
      DUMP_CACHE_INTERVAL_US,
      false/*repeat*/))) {
    LOG_WARN("fail to schedule dump ls location cache timer task", KR(ret));
  }
  return ret;
}

// TODO: Performance can be optimized
int ObLSLocationService::dump_cache()
{
  ObCurTraceId::init(GCONF.self_addr_);
  int ret = OB_SUCCESS;
  ObLSLocationArray ls_location_cache;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (OB_FAIL(ls_location_cache.reserve(inner_cache_.size()))) {
    LOG_WARN("fail to reserve total_cache", KR(ret), "size", inner_cache_.size());
  } else if (OB_FAIL(inner_cache_.get_all(ls_location_cache))) {
    LOG_WARN("fail to get all cache", KR(ret), "cache_size", inner_cache_.size());
  } else if (OB_UNLIKELY(ls_location_cache.empty())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls location cache on server should not be empty", KR(ret));
  } else {
    ObLSLocationArray tenant_ls_locations;
    hash::ObHashSet<uint64_t> tenant_ids;
    if (OB_FAIL(tenant_ids.create(ls_location_cache.count()))) {
      LOG_WARN("fail to create set", KR(ret), "count", ls_location_cache.count());
    } else {
      // get all tenant_ids
      ARRAY_FOREACH_N(ls_location_cache, idx, cnt) {
        const uint64_t tenant_id = ls_location_cache.at(idx).get_tenant_id();
        if (OB_FAIL(tenant_ids.set_refactored(tenant_id))) {
          if (OB_HASH_EXIST == ret) {
            ret = OB_SUCCESS;
          } else {
            LOG_WARN("fail to set_refactored", KR(ret), K(tenant_id));
          }
        }
      }
      int fail_ret = OB_SUCCESS;
      // print each tenant's ls location caches
      FOREACH_X(it, tenant_ids, OB_SUCC(ret)) {
        tenant_ls_locations.reset();
        const uint64_t tenant_id = it->first;
        ARRAY_FOREACH_N(ls_location_cache, idx, cnt) {
          const ObLSLocation &location = ls_location_cache.at(idx);
          if (tenant_id == location.get_tenant_id()) {
            if (OB_FAIL(tenant_ls_locations.push_back(location))) {
              LOG_WARN("fail to push back location", KR(ret), K(tenant_id), K(location));
            }
          }
        } // end foreach ls_location_cache
        if (OB_FAIL(ret)) {
          fail_ret = ret;
          ret = OB_SUCCESS; // ignore ret between tenants
        } else {
          FLOG_INFO("[LS_LOCATION]dump tenant ls location caches",
              K(tenant_id), K(tenant_ls_locations));
        }
      } // end foreach tenant_ids
      ret = OB_FAIL(ret) ? ret : fail_ret;
    }
  }
  return ret;
}

} // end namespace share
} // end namespace oceanbase
