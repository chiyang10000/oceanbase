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

#include "observer/virtual_table/ob_gv_sql.h"
#include "observer/ob_req_time_service.h"

#include "common/object/ob_object.h"

#include "sql/plan_cache/ob_plan_cache.h"
#include "sql/plan_cache/ob_plan_cache_callback.h"
#include "sql/plan_cache/ob_plan_cache_value.h"
#include "sql/plan_cache/ob_plan_cache_util.h"

#include "observer/ob_server_utils.h"
#include "observer/ob_server_struct.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "sql/plan_cache/ob_cache_object_factory.h"
#include "pl/ob_pl.h"
#include "pl/ob_pl_package.h"

#include "lib/thread_local/ob_tsi_factory.h"

using namespace oceanbase;
using namespace sql;
using namespace observer;
using namespace common;

ObGVSql::ObGVSql()
    :plan_id_array_(),
     plan_id_array_idx_(OB_INVALID_ID),
     plan_cache_(NULL)
{
}

ObGVSql::~ObGVSql()
{
}

void ObGVSql::reset()
{
  ObAllPlanCacheBase::reset();
  plan_id_array_.reset();
  plan_id_array_idx_ = OB_INVALID_ID;
  plan_cache_ = NULL;
}

int ObGVSql::inner_open()
{
  int ret = OB_SUCCESS;
  // sys tenant show all tenant plan cache stat
  if (is_sys_tenant(effective_tenant_id_)) {
    if (OB_FAIL(GCTX.omt_->get_mtl_tenant_ids(tenant_id_array_))) {
      SERVER_LOG(WARN, "failed to add tenant id", K(ret));
    }
  } else {
    tenant_id_array_.reset();
    // user tenant show self tenant stat
    if (OB_FAIL(tenant_id_array_.push_back(effective_tenant_id_))) {
      SERVER_LOG(WARN, "fail to push back effective_tenant_id_", KR(ret), K(effective_tenant_id_),
          K(tenant_id_array_));
    }
  }
  SERVER_LOG(TRACE,"get tenant_id array", K(effective_tenant_id_), K(is_sys_tenant(effective_tenant_id_)), K(tenant_id_array_));
  return ret;
}

int ObGVSql::get_row_from_specified_tenant(uint64_t tenant_id, bool &is_end)
{
  int ret = OB_SUCCESS;
  // !!! 引用plan cache资源前必须加ObReqTimeGuard
  ObReqTimeGuard req_timeinfo_guard;
  is_end = false;
  if (OB_INVALID_ID == static_cast<uint64_t>(plan_id_array_idx_)) {
    MTL_SWITCH(tenant_id) {
      plan_cache_ = MTL(ObPlanCache*);
      NG_TRACE(trav_ps_map_start);
      ObGetAllCacheIdOp plan_id_op(&plan_id_array_);
      if (OB_FAIL(plan_cache_->foreach_cache_obj(plan_id_op))) {
        SERVER_LOG(WARN, "fail to traverse id2stat_map");
      } else {
        plan_id_array_idx_ = 0;
      }
      NG_TRACE(trav_ps_map_end);
    }
  }
  if (NULL == plan_cache_) {
    // do nothing
    is_end = true;
  } else if (OB_SUCC(ret)) {
    bool is_filled = false;
    while (OB_SUCC(ret) && false == is_filled && false == is_end) {
      if (plan_id_array_idx_ < 0) {
        ret = OB_ERR_UNEXPECTED;
        SERVER_LOG(WARN, "invalid plan_stat_array index", K(plan_id_array_idx_));
      } else if (plan_id_array_idx_ >= plan_id_array_.count()) {
        is_end = true;
        plan_id_array_idx_ = OB_INVALID_ID;
        plan_id_array_.reset();
        if (OB_UNLIKELY(NULL == plan_cache_)) {
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "plan cache is null", K(ret));
        } else {
          plan_cache_ = NULL;
        }
      } else {
        is_end = false;
        uint64_t plan_id= plan_id_array_.at(plan_id_array_idx_);
        ++plan_id_array_idx_;
        ObCacheObjGuard guard(GV_SQL_HANDLE);
        int tmp_ret = plan_cache_->ref_cache_obj(plan_id, guard); //plan引用计数加1

        //如果当前plan_id对应的plan已被淘汰, 则忽略继续获取下一个plan
        if (OB_HASH_NOT_EXIST == tmp_ret) {
          //do nothing;
        } else if (OB_SUCCESS != tmp_ret) {
          ret = tmp_ret;
        } else if (OB_ISNULL(guard.get_cache_obj())) {
          ret = OB_ERR_UNEXPECTED;
          //SERVER_LOG(WARN, "cache object is NULL", K(ret));
        } else if (OB_FAIL(fill_cells(guard.get_cache_obj(), *plan_cache_))) { //plan exist
          SERVER_LOG(WARN, "fail to fill cells", KPC(guard.get_cache_obj()), K(tenant_id));
        } else {
          is_filled = true;
        }
      }
    } //while end
  }
  SERVER_LOG(DEBUG,
             "add plan from a tenant",
             K(ret),
             K(tenant_id),
             K_(cur_row));
  return ret;
}

int ObGVSql::fill_cells(const ObILibCacheObject *cache_obj, const ObPlanCache &plan_cache)
{
  int ret = OB_SUCCESS;
  const int64_t col_count = output_column_ids_.count();
  ObObj *cells = cur_row_.cells_;
  ObString ipstr;
  ObString stmt;
  const ObPhysicalPlan *plan = NULL;
  const pl::ObPLFunction *pl_func = NULL;
  const pl::ObPLPackage *pl_pkg = NULL;
  if (OB_ISNULL(cache_obj) || OB_ISNULL(allocator_)) {
    ret = OB_INVALID_ARGUMENT;
    SERVER_LOG(WARN, "invalid argument", K(cache_obj), K(ret));
  } else if (cache_obj->is_sql_crsr()) { // sql plan
    if (OB_ISNULL(plan = dynamic_cast<const ObPhysicalPlan *>(cache_obj))) {
      ret = OB_ERR_UNEXPECTED;
      SERVER_LOG(WARN, "unexpected null plan", K(ret), K(plan));
    }
  } else if (cache_obj->is_pkg()) { // pl package
    if (OB_ISNULL(pl_pkg = dynamic_cast<const pl::ObPLPackage *>(cache_obj))) {
      ret = OB_ERR_UNEXPECTED;
      SERVER_LOG(WARN, "unexpected null pl pkg", K(ret));
    }
  } else if (cache_obj->is_anon() || cache_obj->is_sfc() || cache_obj->is_prcr()) { // pl function
    if (OB_ISNULL(pl_func = dynamic_cast<const pl::ObPLFunction *>(cache_obj))) {
      ret = OB_ERR_UNEXPECTED;
      SERVER_LOG(WARN, "unexpected null pl function", K(ret));
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "unexpected cache object type", K(ret), K(cache_obj->get_ns()));
  }

  for (int64_t i =  0; OB_SUCC(ret) && i < col_count; ++i) {
    uint64_t col_id = output_column_ids_.at(i);
    switch(col_id) {
      //tenant id
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::TENANT_ID: {
      cells[i].set_int(plan_cache.get_tenant_id());
      break;
    }
      //ip
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::SVR_IP: {
      // ip
      ipstr.reset();
      if (OB_FAIL(ObServerUtils::get_server_ip(allocator_, ipstr))) {
        SERVER_LOG(ERROR, "get server ip failed", K(ret));
      } else {
        cells[i].set_varchar(ipstr);
        cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
      }
      break;
    }
      //port
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::SVR_PORT: {
      // svr_port
      cells[i].set_int(GCTX.self_addr().get_port());
      break;
    }
    //plan id
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::PLAN_ID: {
      cells[i].set_int(cache_obj->get_object_id());
      break;
    }
      //sql_id
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::SQL_ID: {
      ObString sql_id;
      if (!cache_obj->is_sql_crsr()) {
        cells[i].set_null();
      } else if (OB_FAIL(ob_write_string(*allocator_,
                                         plan->stat_.sql_id_,
                                         sql_id))) {
        SERVER_LOG(ERROR, "copy sql_id failed", K(ret));
      } else {
        cells[i].set_varchar(sql_id);
        cells[i].set_collation_type(ObCharset::get_default_collation(
                                      ObCharset::get_default_charset()));
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::TYPE: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->get_plan_type());
      } else {
        cells[i].set_int(cache_obj->get_ns());
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::IS_BIND_SENSITIVE: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.is_bind_sensitive_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::IS_BIND_AWARE: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.is_bind_aware_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::DB_ID: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_uint64(plan->stat_.db_id_);
      } else {
        cells[i].set_uint64(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::STATEMENT: {
      ObString statement;
      ObString src_stmt;
      ObCollationType tmp_sql_cs_type;
      if (cache_obj->is_sql_crsr()) {
        if (plan->need_param()) {
          src_stmt = plan->stat_.stmt_;
        } else {
          src_stmt = plan->stat_.raw_sql_;
        }
        if (OB_FAIL(ObCharset::charset_convert(*allocator_,
                src_stmt,
                plan->stat_.sql_cs_type_,
                ObCharset::get_system_collation(),
                statement,
                ObCharset::COPY_STRING_ON_SAME_CHARSET | ObCharset::REPLACE_UNKNOWN_CHARACTER))) {
          SERVER_LOG(WARN, "convert raw_sql failed", K(ret));
        } else {
          cells[i].set_lob_value(ObLongTextType, statement.ptr(),
                                 static_cast<int32_t>(statement.length()));
          cells[i].set_collation_type(ObCharset::get_default_collation(
                                        ObCharset::get_default_charset()));
        }
      } else if (cache_obj->is_anon()) {
        if (OB_FAIL(ObCharset::charset_convert(row_calc_buf_,
                pl_func->get_stat().raw_sql_,
                pl_func->get_stat().sql_cs_type_,
                ObCharset::get_system_collation(),
                statement,
                ObCharset::COPY_STRING_ON_SAME_CHARSET | ObCharset::REPLACE_UNKNOWN_CHARACTER))) {
          SERVER_LOG(WARN, "convert raw_sql failed", K(ret));
        } else {
          cells[i].set_lob_value(ObLongTextType, statement.ptr(),
                                 static_cast<int32_t>(statement.length()));
          cells[i].set_collation_type(ObCharset::get_default_collation(
                                        ObCharset::get_default_charset()));
        }
      } else {
        cells[i].set_null();
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::QUERY_SQL: {
      if (cache_obj->is_sql_crsr() || cache_obj->is_anon()) {
        ObString tmp_sql;
        ObCollationType tmp_sql_cs_type;
        if (cache_obj->is_anon()) {
          tmp_sql = pl_func->get_stat().raw_sql_;
          tmp_sql_cs_type = pl_func->get_stat().sql_cs_type_;
        } else {
          tmp_sql = plan->stat_.raw_sql_;
          tmp_sql_cs_type = plan->stat_.sql_cs_type_;
        }
        ObString raw_sql;
        if (!ObCharset::is_valid_collation(tmp_sql_cs_type)) {
          tmp_sql_cs_type = ObCharset::get_system_collation();
        }
        if (OB_FAIL(ObCharset::charset_convert(row_calc_buf_,
                                               tmp_sql,
                                               tmp_sql_cs_type,
                                               ObCharset::get_system_collation(),
                                               raw_sql,
                                               ObCharset::COPY_STRING_ON_SAME_CHARSET | ObCharset::REPLACE_UNKNOWN_CHARACTER))) {
          SERVER_LOG(WARN, "convert raw_sql failed", K(ret));
        } else {
          cells[i].set_lob_value(ObLongTextType, raw_sql.ptr(),
                                 static_cast<int32_t>(raw_sql.length()));
          cells[i].set_collation_type(ObCharset::get_default_collation(
                                        ObCharset::get_default_charset()));
        }
      } else {
        cells[i].set_null();
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::SPECIAL_PARAMS: {
      if (cache_obj->is_sql_crsr()) {
        ObString sp_info_str;
        if (OB_FAIL(ob_write_string(*allocator_,
                                    plan->stat_.sp_info_str_,
                                    sp_info_str))) {
          SERVER_LOG(ERROR, "copy sp_info_str failed", K(ret));
        } else {
          cells[i].set_varchar(sp_info_str);
          cells[i].set_collation_type(ObCharset::get_default_collation(
                                        ObCharset::get_default_charset()));
        }
      } else {
        cells[i].set_null();
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::PARAM_INFOS: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_lob_value(ObLongTextType, plan->stat_.param_infos_.ptr(),
                               static_cast<int32_t>(plan->stat_.param_infos_.length()));
        cells[i].set_collation_type(ObCharset::get_default_collation(
                                      ObCharset::get_default_charset()));
      } else {
        cells[i].set_null();
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::SYS_VARS: {
      if (cache_obj->is_sql_crsr()) {
        ObString sys_vars_str;
        if (OB_FAIL(ob_write_string(*allocator_,
                                    plan->stat_.sys_vars_str_,
                                    sys_vars_str))) {
          SERVER_LOG(ERROR, "copy sys_vars_str failed", K(ret));
        } else {
          cells[i].set_varchar(sys_vars_str);
          cells[i].set_collation_type(ObCharset::get_default_collation(
                                        ObCharset::get_default_charset()));
        }
      } else {
        cells[i].set_null();
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::CONFIGS: {
      if (cache_obj->is_sql_crsr()) {
        ObString config_str;
        if (OB_FAIL(ob_write_string(*allocator_,
                                    plan->stat_.config_str_,
                                    config_str))) {
          SERVER_LOG(ERROR, "copy sys_vars_str failed", K(ret));
        } else {
          cells[i].set_varchar(config_str);
          cells[i].set_collation_type(ObCharset::get_default_collation(
                                        ObCharset::get_default_charset()));
        }
      } else {
        cells[i].set_null();
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::PLAN_HASH: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_uint64(plan->stat_.plan_hash_value_);
      } else {
        cells[i].set_uint64(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::FIRST_LOAD_TIME: {
      int64_t gen_time = 0;
      if (cache_obj->is_sql_crsr()) {
        gen_time = plan->stat_.gen_time_;
      } else if (NULL != pl_func) {
        gen_time = pl_func->get_stat().gen_time_;
      }
      cells[i].set_timestamp(gen_time);
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::SCHEMA_VERSION: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.schema_version_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::LAST_ACTIVE_TIME: {
      int64_t last_active_time = 0;
      if (cache_obj->is_sql_crsr()) {
        last_active_time = plan->stat_.last_active_time_;
      } else if (NULL != pl_func) {
        last_active_time = pl_func->get_stat().last_active_time_;
      }
      cells[i].set_timestamp(last_active_time);
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::AVG_EXE_USEC: {
      if (cache_obj->is_sql_crsr()) {
        if (plan->stat_.execute_times_ != 0) {
          cells[i].set_int(plan->stat_.elapsed_time_ / plan->stat_.execute_times_);
        } else {
          cells[i].set_int(0);
        }
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::SLOWEST_EXE_TIME: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_timestamp(plan->stat_.slowest_exec_time_);
      } else {
        cells[i].set_timestamp(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::SLOWEST_EXE_USEC: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.slowest_exec_usec_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::SLOW_COUNT: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.slow_count_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::HIT_COUNT: {
      int64_t hit_count = 0;
      if (cache_obj->is_sql_crsr()) {
        hit_count = plan->stat_.hit_count_;
      } else if (NULL != pl_func) {
        hit_count = pl_func->get_stat().hit_count_;
      }
      cells[i].set_int(hit_count);
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::PLAN_SIZE: {
      int64_t mem_used = 0;
      if (cache_obj->is_sql_crsr()) {
        mem_used = plan->stat_.mem_used_;
      } else {
        mem_used = cache_obj->get_mem_size();
      }
      cells[i].set_int(mem_used);
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::EXECUTIONS: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.execute_times_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::DISK_READS: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.disk_reads_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::DIRECT_WRITES: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.direct_writes_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::BUFFER_GETS: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.buffer_gets_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::APPLICATION_WAIT_TIME: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_uint64(static_cast<uint64_t>(plan->stat_.application_wait_time_));
      } else {
        cells[i].set_uint64(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::CONCURRENCY_WAIT_TIME: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_uint64(static_cast<uint64_t>(plan->stat_.concurrency_wait_time_));
      } else {
        cells[i].set_uint64(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::USER_IO_WAIT_TIME: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_uint64(static_cast<uint64_t>(plan->stat_.user_io_wait_time_));
      } else {
        cells[i].set_uint64(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::ROWS_PROCESSED: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.rows_processed_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::ELAPSED_TIME: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_uint64(static_cast<uint64_t>(plan->stat_.elapsed_time_));
      } else {
        cells[i].set_uint64(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::CPU_TIME: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_uint64(static_cast<uint64_t>(plan->stat_.cpu_time_));
      } else {
        cells[i].set_uint64(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::LARGE_QUERYS: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.large_querys_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::DELAYED_LARGE_QUERYS: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.delayed_large_querys_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::OUTLINE_VERSION: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.outline_version_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::OUTLINE_ID: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.outline_id_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::HINTS_INFO: {
      if (cache_obj->is_sql_crsr()) {
        ObString hints_info;
        if (OB_FAIL(ob_write_string(*allocator_,
                                    plan->stat_.hints_info_,
                                    hints_info))) {
          SERVER_LOG(ERROR, "copy hints_info failed", K(ret));
        } else {
          cells[i].set_lob_value(ObLongTextType, hints_info.ptr(),
                                 static_cast<int32_t>(hints_info.length()));
          cells[i].set_collation_type(ObCharset::get_default_collation(
                                        ObCharset::get_default_charset()));
        }
      } else {
        cells[i].set_null();
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::OUTLINE_DATA: {
      if (cache_obj->is_sql_crsr()) {
        ObString outline_data;
        if (OB_FAIL(ob_write_string(*allocator_,
                                    plan->stat_.outline_data_,
                                    outline_data))) {
          SERVER_LOG(ERROR, "copy outline_data failed", K(ret));
        } else {
          cells[i].set_lob_value(ObLongTextType, outline_data.ptr(),
                                 static_cast<int32_t>(outline_data.length()));
          cells[i].set_collation_type(ObCharset::get_default_collation(
                                        ObCharset::get_default_charset()));
        }
      } else {
        cells[i].set_null();
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::ACS_SEL_INFO: {
      if (cache_obj->is_sql_crsr()) {
        stmt.assign_ptr(plan->stat_.plan_sel_info_str_, plan->stat_.plan_sel_info_str_len_);
        cells[i].set_lob_value(ObLongTextType, stmt.ptr(),
                               static_cast<int32_t>(stmt.length()));
        cells[i].set_collation_type(ObCharset::get_default_collation(
                                      ObCharset::get_default_charset()));
      } else {
        cells[i].set_null();
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::TABLE_SCAN: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_bool(plan->contain_table_scan());
      } else {
        cells[i].set_bool(false);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::EVOLUTION: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_bool(plan->get_evolution());
      } else {
        cells[i].set_bool(false);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::HINTS_ALL_WORKED: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_bool(plan->stat_.hints_all_worked_);
      } else {
        cells[i].set_bool(false);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::EVO_EXECUTIONS: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.evolution_stat_.executions_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::EVO_CPU_TIME: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_uint64(plan->stat_.evolution_stat_.cpu_time_);
      } else {
        cells[i].set_uint64(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::TIMEOUT_COUNT: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.timeout_count_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::PS_STMT_ID: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.ps_stmt_id_);
      } else if (cache_obj->is_anon()) {
        cells[i].set_int(pl_func->get_stat().pl_schema_id_);
      } else {
        cells[i].set_int(OB_INVALID_ID);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::DELAYED_PX_QUERYS: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.delayed_px_querys_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::SESSID: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_uint64(plan->stat_.sessid_);
      } else {
        cells[i].set_uint64(OB_INVALID_ID);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::TEMP_TABLES: {
      if (cache_obj->is_sql_crsr()) {
        stmt.assign_ptr(plan->stat_.plan_tmp_tbl_name_str_, plan->stat_.plan_tmp_tbl_name_str_len_);
        cells[i].set_lob_value(ObLongTextType, stmt.ptr(),
                               static_cast<int32_t>(stmt.length()));
        cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
      } else {
        cells[i].set_null();
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::IS_USE_JIT: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_bool(plan->stat_.is_use_jit_);
      } else {
        cells[i].set_bool(false);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::OBJECT_TYPE: {
      ObString type_name;
      if (OB_FAIL(ObPlanCacheObject::type_to_name(cache_obj->get_ns(), *allocator_, type_name))) {
        SERVER_LOG(ERROR, "failed to get type_name", K(ret));
      } else {
        // do nothing
      }
      cells[i].set_lob_value(ObLongTextType, type_name.ptr(),
                             static_cast<int32_t>(type_name.length()));
      cells[i].set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::ENABLE_BF_CACHE: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_bool(plan->stat_.enable_bf_cache_);
      } else {
        cells[i].set_bool(false);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::BF_FILTER_CNT: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.bf_filter_cnt_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::BF_ACCESS_CNT: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.bf_access_cnt_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::ENABLE_ROW_CACHE: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_bool(plan->stat_.enable_fuse_row_cache_);
      } else {
        cells[i].set_bool(false);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::ROW_CACHE_HIT_CNT: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.row_cache_hit_cnt_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::ROW_CACHE_MISS_CNT: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.row_cache_miss_cnt_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::ENABLE_FUSE_ROW_CACHE: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_bool(plan->stat_.enable_fuse_row_cache_);
      } else {
        cells[i].set_bool(false);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::FUSE_ROW_CACHE_HIT_CNT: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.fuse_row_cache_hit_cnt_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::FUSE_ROW_CACHE_MISS_CNT: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_int(plan->stat_.fuse_row_cache_miss_cnt_);
      } else {
        cells[i].set_int(0);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::PL_SCHEMA_ID: {
      uint64_t pl_schema_id = 0;
      if (cache_obj->is_anon() || cache_obj->is_sfc() || cache_obj->is_prcr()) {
        pl_schema_id = pl_func->get_stat().pl_schema_id_;
      } else if (cache_obj->is_pkg()) {
        pl_schema_id = pl_pkg->get_id();
      }
      cells[i].set_uint64(pl_schema_id);
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::IS_BATCHED_MULTI_STMT: {
      if (cache_obj->is_sql_crsr()) {
        cells[i].set_bool(plan->get_is_batched_multi_stmt());
      } else {
        cells[i].set_bool(false);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::OBJECT_STATUS: {
      cells[i].set_int(cache_obj->get_obj_status());
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::RULE_NAME: {
      ObString rule_name;
      if (!cache_obj->is_sql_crsr() || plan->get_rule_name()) {
        cells[i].set_null();
      } else if (OB_FAIL(ob_write_string(*allocator_,
                                         plan->get_rule_name(),
                                         rule_name))) {
        SERVER_LOG(ERROR, "copy rule_name failed", K(ret));
      } else {
        cells[i].set_varchar(rule_name);
        cells[i].set_collation_type(ObCharset::get_default_collation(
                                      ObCharset::get_default_charset()));
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::IS_IN_PC: {
      common::hash::ObHashMap<ObCacheObjID, ObILibCacheObject*> &co_map = const_cast<ObPlanCache *>(&plan_cache)->get_cache_obj_mgr().get_cache_obj_map();

      if (NULL != co_map.get(cache_obj->get_object_id())) {
        cells[i].set_bool(true);
      } else {
        cells[i].set_bool(false);
      }
      break;
    }
    case share::ALL_VIRTUAL_PLAN_STAT_CDE::ERASE_TIME: {
      cells[i].set_timestamp(cache_obj->get_logical_del_time());
      break;
    }
    default: {
      ret = OB_ERR_UNEXPECTED;
      SERVER_LOG(WARN,
                 "invalid column id",
                 K(ret),
                 K(i),
                 K(output_column_ids_),
                 K(col_id));
      break;
    }
    }
  } // end for
  return ret;
}

int ObGVSql::get_row_from_tenants()
{
  int ret = OB_SUCCESS;
  bool is_sub_end = false;
  do {
    is_sub_end = false;
    if (tenant_id_array_idx_ < 0) {
      ret = OB_ERR_UNEXPECTED;
      SERVER_LOG(WARN, "invalid tenant_id_array idx", K(ret), K(tenant_id_array_idx_));
    } else if (tenant_id_array_idx_ >= tenant_id_array_.count()) {
      ret = OB_ITER_END;
      tenant_id_array_idx_ = 0;
    } else {
      if (OB_FAIL(get_row_from_specified_tenant(tenant_id_array_.at(tenant_id_array_idx_),
                                                is_sub_end))) {
        SERVER_LOG(WARN,
                   "fail to insert plan by tenant id",
                   K(ret),
                   "tenant id",
                   tenant_id_array_.at(tenant_id_array_idx_),
                   K(tenant_id_array_idx_));
      } else {
        if (is_sub_end) {
          ++tenant_id_array_idx_;
        }
      }
    }
  } while(is_sub_end && OB_SUCCESS == ret);
  return ret;
}
