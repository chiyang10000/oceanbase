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
 * This file contains implementation support for the json table abstraction.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_json_table_op.h"
#include "share/object/ob_obj_cast_util.h"
#include "share/object/ob_obj_cast.h"
#include "common/sql_mode/ob_sql_mode_utils.h"
#include "sql/ob_sql_utils.h"
#include "sql/engine/expr/ob_datum_cast.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/expr/ob_expr_json_func_helper.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

/* json table empty or error */
const static int32_t JSN_TABLE_ERROR    = 0;
const static int32_t JSN_TABLE_NULL     = 1;
const static int32_t JSN_TABLE_DEFAULT  = 2;
const static int32_t JSN_TABLE_IMPLICIT = 3;

/* json query empty or error */
const static int32_t JSN_QUERY_ERROR        = 0;
const static int32_t JSN_QUERY_NULL         = 1;
const static int32_t JSN_QUERY_EMPTY        = 2;
const static int32_t JSN_QUERY_EMPTY_ARRAY  = 3;
const static int32_t JSN_QUERY_EMPTY_OBJECT = 4;
const static int32_t JSN_QUERY_IMPLICIT     = 5;

/* json query on mismatch { error : 0, null : 1, implicit : 2 }*/
const static int32_t JSN_QUERY_MISMATCH_ERROR    = 0;
const static int32_t JSN_QUERY_MISMATCH_NULL     = 1;
const static int32_t JSN_QUERY_MISMATCH_IMPLICIT = 2;

/* json query wrapper type */
const static int32_t JSN_QUERY_WITHOUT_WRAPPER                    = 0;
const static int32_t JSN_QUERY_WITHOUT_ARRAY_WRAPPER              = 1;
const static int32_t JSN_QUERY_WITH_WRAPPER                       = 2;
const static int32_t JSN_QUERY_WITH_ARRAY_WRAPPER                 = 3;
const static int32_t JSN_QUERY_WITH_UNCONDITIONAL_WRAPPER         = 4;
const static int32_t JSN_QUERY_WITH_CONDITIONAL_WRAPPER           = 5;
const static int32_t JSN_QUERY_WITH_UNCONDITIONAL_ARRAY_WRAPPER   = 6;
const static int32_t JSN_QUERY_WITH_CONDITIONAL_ARRAY_WRAPPER     = 7;
const static int32_t JSN_QUERY_WRAPPER_IMPLICIT                   = 8;

/* json query on scalars { allow : 0, disallow : 1, implicit : 2 }*/
const static int32_t JSN_QUERY_SCALARS_ALLOW       = 0;
const static int32_t JSN_QUERY_SCALARS_DISALLOW    = 1;
const static int32_t JSN_QUERY_SCALARS_IMPLICIT    = 2;

/* json value empty or error */
const static int32_t JSN_VALUE_ERROR    = 0;
const static int32_t JSN_VALUE_NULL     = 1;
const static int32_t JSN_VALUE_DEFAULT  = 2;
const static int32_t JSN_VALUE_IMPLICIT = 3;

/*  json value  on mismatch { error : 0, null : 1, ignore : 2 }*/
const static int32_t JSN_VALUE_MISMATCH_ERROR    = 0;
const static int32_t JSN_VALUE_MISMATCH_NULL     = 1;
const static int32_t JSN_VALUE_MISMATCH_IGNORE   = 2;
const static int32_t JSN_VALUE_MISMATCH_IMPLICIT = 3;

/* json value  mismatch type { MISSING : 0, EXTRA : 1, TYPE : 2, EMPTY : 3} */
const static int32_t JSN_VALUE_TYPE_MISSING_DATA    = 0;
const static int32_t JSN_VALUE_TYPE_EXTRA_DATA      = 1;
const static int32_t JSN_VALUE_TYPE_TYPE_ERROR      = 2;
const static int32_t JSN_VALUE_TYPE_IMPLICIT        = 3;

/* json exists */
const static int32_t JSN_EXIST_FALSE = 0;
const static int32_t JSN_EXIST_TRUE  = 1;
const static int32_t JSN_EXIST_ERROR = 2;
const static int32_t JSN_EXIST_DEFAULT = 3;

#define SET_COVER_ERROR(jt_ctx_ptr, error_code) \
{\
  if (!(jt_ctx_ptr)->is_cover_error_) { \
    (jt_ctx_ptr)->is_cover_error_ = true; \
    (jt_ctx_ptr)->error_code_ = error_code; \
  } \
}

#define EVAL_COVER_CODE(jt_ctx_ptr, error_code) \
{\
  if ((jt_ctx_ptr)->is_cover_error_) { \
    error_code = (jt_ctx_ptr)->error_code_; \
  } \
}

#define RESET_COVER_CODE(jt_ctx_ptr) \
{\
  if ((jt_ctx_ptr)->is_cover_error_) { \
    (jt_ctx_ptr)->is_cover_error_ = false; \
    (jt_ctx_ptr)->error_code_ = 0; \
  } \
}

ObJsonNull* ObJsonTableOp::js_null_ = nullptr;
ObJsonArray* ObJsonTableOp::js_arr_ = nullptr;
ObJsonObject* ObJsonTableOp::js_obj_ = nullptr;

int JtFuncHelpler::cast_to_int(ObIJsonBase *j_base, ObObjType dst_type, int64_t &val)
{
  INIT_SUCC(ret);

  if (OB_ISNULL(j_base)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("json base is null", K(ret));
  } else if (OB_FAIL(j_base->to_int(val, true))) {
    ret = OB_OPERATE_OVERFLOW;
    LOG_USER_ERROR(OB_OPERATE_OVERFLOW, "SIGNED", "json_table");
    LOG_WARN("cast to int failed", K(ret), K(*j_base));
  } else if (dst_type < ObIntType &&
    OB_FAIL(int_range_check(dst_type, val, val))) {
    ret = OB_OPERATE_OVERFLOW;
    LOG_USER_ERROR(OB_OPERATE_OVERFLOW, "SIGNED", "json_table");
  }

  return ret;
}

int JtFuncHelpler::cast_to_uint(ObIJsonBase *j_base, ObObjType dst_type, uint64_t &val)
{
  INIT_SUCC(ret);

  if (OB_ISNULL(j_base)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("json base is null", K(ret));
  } else if (OB_FAIL(j_base->to_uint(val, true, true))) {
    LOG_WARN("cast to uint failed", K(ret), K(*j_base));
    if (ret == OB_OPERATE_OVERFLOW) {
      LOG_USER_ERROR(OB_OPERATE_OVERFLOW, "UNSIGNED", "json_table");
    }
  } else if (dst_type < ObUInt64Type &&
    OB_FAIL(uint_upper_check(dst_type, val))) {
    LOG_WARN("uint_upper_check failed", K(ret));
  }

  return ret;
}

int JtFuncHelpler::number_range_check(const ObAccuracy &accuracy,
                              ObIAllocator *allocator,
                              number::ObNumber &val,
                              bool strict)
{
  INIT_SUCC(ret);
  ObPrecision precision = accuracy.get_precision();
  ObScale scale = accuracy.get_scale();
  const number::ObNumber *min_check_num = NULL;
  const number::ObNumber *max_check_num = NULL;
  const number::ObNumber *min_num_mysql = NULL;
  const number::ObNumber *max_num_mysql = NULL;
  bool is_finish = false;
  if (lib::is_oracle_mode()) {
    if (OB_MAX_NUMBER_PRECISION >= precision
        && precision >= OB_MIN_NUMBER_PRECISION
        && number::ObNumber::MAX_SCALE >= scale
        && scale >= number::ObNumber::MIN_SCALE) {
      min_check_num = &(ObNumberConstValue::ORACLE_CHECK_MIN[precision][scale + ObNumberConstValue::MAX_ORACLE_SCALE_DELTA]);
      max_check_num = &(ObNumberConstValue::ORACLE_CHECK_MAX[precision][scale + ObNumberConstValue::MAX_ORACLE_SCALE_DELTA]);
    } else if (ORA_NUMBER_SCALE_UNKNOWN_YET == scale
                && PRECISION_UNKNOWN_YET == precision) {
      is_finish = true;
    } else if (PRECISION_UNKNOWN_YET == precision
              && number::ObNumber::MAX_SCALE >= scale
              && scale >= number::ObNumber::MIN_SCALE) {
      number::ObNumber num;
      if (OB_FAIL(num.from(val, *allocator))) {
      } else if (OB_FAIL(num.round(scale))) {
      } else if (val.compare(num) != 0) {
        ret = OB_OPERATE_OVERFLOW;
        LOG_WARN("input value is out of range.", K(scale), K(val));
      } else {
        is_finish = true;
      }
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid arguments", K(ret), K(precision), K(scale));
    }
  } else {
    if (OB_UNLIKELY(precision < scale)) {
      ret = OB_ERR_M_BIGGER_THAN_D;
      LOG_WARN("Invalid accuracy.", K(ret), K(scale), K(precision));
    } else if (number::ObNumber::MAX_PRECISION >= precision
        && precision >= OB_MIN_DECIMAL_PRECISION
        && number::ObNumber::MAX_SCALE >= scale
        && scale >= 0) {
      min_check_num = &(ObNumberConstValue::MYSQL_CHECK_MIN[precision][scale]);
      max_check_num = &(ObNumberConstValue::MYSQL_CHECK_MAX[precision][scale]);
      min_num_mysql = &(ObNumberConstValue::MYSQL_MIN[precision][scale]);
      max_num_mysql = &(ObNumberConstValue::MYSQL_MAX[precision][scale]);
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid arguments", K(ret), K(precision), K(scale));
    }
  }
  if (OB_SUCC(ret) && !is_finish) {
    if (OB_ISNULL(min_check_num) || OB_ISNULL(max_check_num)
        || (!lib::is_oracle_mode()
          && (OB_ISNULL(min_num_mysql) || OB_ISNULL(max_num_mysql)))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("min_num or max_num is null", K(ret), KPC(min_check_num), KPC(max_check_num));
    } else if (val <= *min_check_num) {
      if (lib::is_oracle_mode()) {
        ret = OB_ERR_VALUE_LARGER_THAN_ALLOWED;
      } else {
        ret = OB_DATA_OUT_OF_RANGE;
      }
      LOG_WARN("val is out of min range check.", K(val), K(*min_check_num));
      is_finish = true;
    } else if (val >= *max_check_num) {
      if (lib::is_oracle_mode()) {
        ret = OB_ERR_VALUE_LARGER_THAN_ALLOWED;
      } else {
        ret = OB_DATA_OUT_OF_RANGE;
      }
      LOG_WARN("val is out of max range check.", K(val), K(*max_check_num));
      is_finish = true;
    } else {
      ObNumStackOnceAlloc tmp_alloc;
      number::ObNumber num;
      if (OB_FAIL(num.from(val, tmp_alloc))) {
      } else if (OB_FAIL(num.round(scale))) {
        LOG_WARN("num.round failed", K(ret), K(scale));
      } else {
        if (strict) {
          if (num.compare(val) != 0) {
            ret = OB_OPERATE_OVERFLOW;
            LOG_WARN("input value is out of range.", K(scale), K(val));
          } else {
            is_finish = true;
          }
        } else {
          if (OB_ISNULL(allocator)) {
            ret = OB_ERR_NULL_VALUE;
            LOG_WARN("allocator is null", K(ret));
          } else if (OB_FAIL(val.deep_copy_v3(num, *allocator))) {
            LOG_WARN("val.deep_copy_v3 failed", K(ret), K(num));
          } else {
            is_finish = true;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && !is_finish) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected situation, res is not set", K(ret));
  }
  LOG_DEBUG("number_range_check_v2 done", K(ret), K(is_finish), K(accuracy), K(val),
            KPC(min_check_num), KPC(max_check_num));

  return ret;
}

int JtFuncHelpler::datetime_scale_check(const ObAccuracy &accuracy, int64_t &value, bool strict)
{
  INIT_SUCC(ret);
  ObScale scale = accuracy.get_scale();

  if (OB_UNLIKELY(scale > MAX_SCALE_FOR_TEMPORAL)) {
    ret = OB_ERR_TOO_BIG_PRECISION;
    LOG_USER_ERROR(OB_ERR_TOO_BIG_PRECISION, scale, "CAST",
        static_cast<int64_t>(MAX_SCALE_FOR_TEMPORAL));
  } else if (OB_UNLIKELY(0 <= scale && scale < MAX_SCALE_FOR_TEMPORAL)) {
    // first check zero
    if (strict &&
        (value == ObTimeConverter::ZERO_DATE ||
        value == ObTimeConverter::ZERO_DATETIME)) {
      ret = OB_INVALID_DATE_VALUE;
      LOG_WARN("Zero datetime is invalid in json_value.", K(value));
    } else {
      int64_t temp_value = value;
      ObTimeConverter::round_datetime(scale, temp_value);
      if (strict && temp_value != value) {
        ret = OB_OPERATE_OVERFLOW;
        LOG_WARN("Invalid input value.", K(value), K(scale));
      } else if (ObTimeConverter::is_valid_datetime(temp_value)) {
        value = temp_value;
      } else {
        ret = OB_ERR_NULL_VALUE; // set null for res
        LOG_DEBUG("Invalid datetime val, return set_null", K(temp_value));
      }
    }
  }

  return ret;
}

int JtFuncHelpler::time_scale_check(const ObAccuracy &accuracy, int64_t &value, bool strict)
{
  INIT_SUCC(ret);
  ObScale scale = accuracy.get_scale();

  if (OB_LIKELY(0 <= scale && scale < MAX_SCALE_FOR_TEMPORAL)) {
    int64_t temp_value = value;
    ObTimeConverter::round_datetime(scale, temp_value);
    if (strict && temp_value != value) { // round success
      ret = OB_OPERATE_OVERFLOW;
      LOG_WARN("Invalid input value.", K(value), K(scale));
    } else {
      value = temp_value;
    }
  } else {
    // consistent with cast process do nothing
  }

  return ret;
}


int JtFuncHelpler::cast_to_string(JtColNode* node,
                                  common::ObIAllocator *allocator,
                                  ObIJsonBase *j_base,
                                  ObCollationType in_cs_type,
                                  ObCollationType dst_cs_type,
                                  common::ObAccuracy &accuracy,
                                  ObObjType dst_type,
                                  ObString &val,
                                  bool is_trunc,
                                  bool is_quote)
{
  INIT_SUCC(ret);

  if (OB_ISNULL(j_base)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("json base is null", K(ret));
  } else if (OB_ISNULL(allocator)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("allocator is null", K(ret));
  } else {
    ObJsonBuffer j_buf(allocator);
    if (OB_FAIL(j_base->print(j_buf, is_quote))) {
      LOG_WARN("fail to_string as json", K(ret));
    } else {
      ObObjType in_type = ObLongTextType;
      ObString temp_str_val(j_buf.length(), j_buf.ptr());
      bool is_need_string_string_convert = ((CS_TYPE_BINARY == dst_cs_type) ||
                                            (ObCharset::charset_type_by_coll(in_cs_type) !=
                                            ObCharset::charset_type_by_coll(dst_cs_type)));
      if (is_need_string_string_convert) {
        if (CS_TYPE_BINARY != in_cs_type
            && CS_TYPE_BINARY != dst_cs_type
            && (ObCharset::charset_type_by_coll(in_cs_type) !=
            ObCharset::charset_type_by_coll(dst_cs_type))) {
          char *buf = NULL;
          const int64_t factor = 2;
          int64_t buf_len = temp_str_val.length() * factor;
          uint32_t result_len = 0;
          buf = static_cast<char*>(allocator->alloc(buf_len));
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            LOG_WARN("alloc memory failed", K(ret));
          } else if (OB_FAIL(ObCharset::charset_convert(in_cs_type, temp_str_val.ptr(),
                                                        temp_str_val.length(), dst_cs_type, buf,
                                                        buf_len, result_len))) {
            LOG_WARN("charset convert failed", K(ret));
          } else {
            val.assign_ptr(buf, result_len);
          }
        } else {
          if (CS_TYPE_BINARY == in_cs_type || CS_TYPE_BINARY == dst_cs_type) {
            // just copy string when in_cs_type or out_cs_type is binary
            const ObCharsetInfo *cs = NULL;
            int64_t align_offset = 0;
            if (CS_TYPE_BINARY == in_cs_type && lib::is_mysql_mode()
                && (NULL != (cs = ObCharset::get_charset(dst_cs_type)))) {
              if (cs->mbminlen > 0 && temp_str_val.length() % cs->mbminlen != 0) {
                align_offset = cs->mbminlen - temp_str_val.length() % cs->mbminlen;
              }
            }
            int64_t len = align_offset + temp_str_val.length();
            char *buf = reinterpret_cast<char*>(allocator->alloc(len));
            if (OB_ISNULL(buf)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
              LOG_WARN("allocate memory failed", K(ret));
            } else {
              MEMMOVE(buf + align_offset, temp_str_val.ptr(), len - align_offset);
              MEMSET(buf, 0, align_offset);
              val.assign_ptr(buf, len);
            }
          } else {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("same charset should not be here, just use cast_eval_arg", K(ret),
                K(in_type), K(dst_type), K(in_cs_type), K(dst_cs_type));
          }
        }
      } else {
        val.assign_ptr(temp_str_val.ptr(), temp_str_val.length());
      }

      ObLengthSemantics senmactics = node->col_info_.data_type_.get_length_semantics();
      // do str length check
      const int32_t str_len_char = static_cast<int32_t>(ObCharset::strlen_char(
        senmactics == LS_BYTE ? CS_TYPE_BINARY : dst_cs_type, val.ptr(), val.length()));
      ObLength max_accuracy_len = (dst_type == ObLongTextType) ? OB_MAX_LONGTEXT_LENGTH : accuracy.get_length();
      max_accuracy_len *= (senmactics == LS_BYTE ? 1 : 2);

      uint32_t byte_len = 0;
      byte_len = ObCharset::charpos(senmactics == LS_BYTE ? CS_TYPE_BINARY : dst_cs_type, val.ptr(), str_len_char, max_accuracy_len);

      if (OB_SUCC(ret)) {
        if (max_accuracy_len == DEFAULT_STR_LENGTH) { // default string len
        } else if (is_trunc && max_accuracy_len < str_len_char) {
          if (node->col_info_.col_type_ == static_cast<int>(COL_TYPE_EXISTS)
              || j_base->json_type() == ObJsonNodeType::J_INT
              || j_base->json_type() == ObJsonNodeType::J_UINT
              || j_base->json_type() == ObJsonNodeType::J_BOOLEAN
              || j_base->json_type() == ObJsonNodeType::J_DOUBLE
              || j_base->json_type() == ObJsonNodeType::J_DECIMAL) {
            ret = OB_OPERATE_OVERFLOW;
          } else {
            // bugfix: https://work.aone.alibaba-inc.com/issue/46640577
            // Q1:SELECT c1 ,jt.ww b_c1 FROM t1, json_table ( c2 columns( ww varchar2(2 char) truncate  path '$.a')) jt ;
            // Q2:SELECT c1 ,jt.ww b_c1 FROM t1, json_table ( c2 columns( ww varchar2(2 byte) truncate path '$.a')) jt;
            // should not split in the middle of char
            if (byte_len == 0) { // value has zero length
              val.assign_ptr("", 0);
            } else if (senmactics == LS_BYTE && dst_cs_type != CS_TYPE_BINARY) {
              int64_t char_len; // not used
              // zero max_accuracy_len not allowed
              byte_len = ObCharset::max_bytes_charpos(dst_cs_type, val.ptr(), str_len_char, max_accuracy_len, char_len);
              if (byte_len == 0) { // buffer not enough for one bytes
                ret = OB_OPERATE_OVERFLOW;
              } else {
                val.assign_ptr(val.ptr(), byte_len);
              }
            } else {
              val.assign_ptr(val.ptr(), byte_len);
            }
          }
        } else if (max_accuracy_len <= 0 || str_len_char > max_accuracy_len) {
          ret = OB_OPERATE_OVERFLOW;
        }
      }
    }
  }

  return ret;
}

bool JtFuncHelpler::type_cast_to_string(JtColNode* node,
                                        ObString &json_string,
                                        common::ObIAllocator *allocator,
                                        ObIJsonBase *j_base,
                                        ObAccuracy &accuracy)
{
  INIT_SUCC(ret);
  ret = cast_to_string(node, allocator, j_base, CS_TYPE_BINARY, CS_TYPE_BINARY, accuracy, ObLongTextType, json_string);
  return ret == 0 ? true : false;
}

int JtFuncHelpler::cast_to_datetime(JtColNode* node,
                                    ObIJsonBase *j_base,
                                    common::ObIAllocator *allocator,
                                    const ObBasicSessionInfo *session,
                                    common::ObAccuracy &accuracy,
                                    int64_t &val)
{
  INIT_SUCC(ret);
  ObString json_string;
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session is NULL", K(ret));
  } else {
    oceanbase::common::ObTimeConvertCtx cvrt_ctx(session->get_timezone_info(), false);
    if (OB_FAIL(common_get_nls_format(session, ObDateTimeType,
                                      true,
                                      cvrt_ctx.oracle_nls_format_))) {
      LOG_WARN("common_get_nls_format failed", K(ret));
    } else if (type_cast_to_string(node, json_string, allocator, j_base, accuracy) && json_string.length() > 0) {
      ObJsonString json_str(json_string.ptr(),json_string.length());
      if (OB_FAIL(json_str.to_datetime(val, &cvrt_ctx))) {
        LOG_WARN("wrapper to datetime failed.", K(ret), K(*j_base));
      }
    } else if (OB_ISNULL(j_base)) {
      ret = OB_ERR_NULL_VALUE;
      LOG_WARN("json base is null", K(ret));
    } else if (OB_FAIL(j_base->to_datetime(val, &cvrt_ctx))) {
      LOG_WARN("wrapper to datetime failed.", K(ret), K(*j_base));
    }
    if (OB_SUCC(ret) && OB_FAIL(datetime_scale_check(accuracy, val))) {
      LOG_WARN("datetime_scale_check failed.", K(ret));
    }
  }

  return ret;
}

int JtFuncHelpler::cast_to_otimstamp(ObIJsonBase *j_base,
                             const ObBasicSessionInfo *session,
                             common::ObAccuracy &accuracy,
                             ObObjType dst_type,
                             ObOTimestampData &out_val)
{
  INIT_SUCC(ret);
  int64_t val;

  oceanbase::common::ObTimeConvertCtx cvrt_ctx(NULL, true);
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session is NULL", K(ret));
  } else if (OB_ISNULL(j_base)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("json base is null", K(ret));
  } else {
    cvrt_ctx.tz_info_ = session->get_timezone_info();
    if (OB_FAIL(common_get_nls_format(session, ObDateTimeType,
                                      true,
                                      cvrt_ctx.oracle_nls_format_))) {
      LOG_WARN("common_get_nls_format failed", K(ret));
    }
  }
  if (OB_FAIL(j_base->to_datetime(val, &cvrt_ctx))) {
    LOG_WARN("wrapper to datetime failed.", K(ret), K(*j_base));
  } else {
    ObScale scale = accuracy.get_scale();
    if (OB_FAIL(ObTimeConverter::odate_to_otimestamp(val, cvrt_ctx.tz_info_, dst_type, out_val))) {
      LOG_WARN("fail to timestamp_to_timestamp_tz", K(ret), K(val), K(dst_type));
    } else if (OB_UNLIKELY(0 <= scale && scale < MAX_SCALE_FOR_ORACLE_TEMPORAL)) {
      ObOTimestampData ot_data = ObTimeConverter::round_otimestamp(scale, out_val);
      if (ObTimeConverter::is_valid_otimestamp(ot_data.time_us_,
          static_cast<int32_t>(ot_data.time_ctx_.tail_nsec_))) {
        out_val = ot_data;
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid otimestamp, set it null ", K(ot_data), K(scale), "orig_date", out_val);
      }
    }
  }

  return ret;
}

int JtFuncHelpler::cast_to_date(ObIJsonBase *j_base, int32_t &val)
{
  INIT_SUCC(ret);

  if (OB_ISNULL(j_base)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("json base is null", K(ret));
  } else if (OB_FAIL(j_base->to_date(val))) {
    LOG_WARN("wrapper to date failed.", K(ret), K(*j_base));
    ret = OB_OPERATE_OVERFLOW;
    LOG_USER_ERROR(OB_OPERATE_OVERFLOW, "DATE", "json_value");
  }

  return ret;
}

int JtFuncHelpler::cast_to_time(ObIJsonBase *j_base,
                        common::ObAccuracy &accuracy,
                        int64_t &val)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(j_base)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("json base is null", K(ret));
  } else if (OB_FAIL(j_base->to_time(val))) {
    LOG_WARN("wrapper to time failed.", K(ret), K(*j_base));
    ret = OB_OPERATE_OVERFLOW;
    LOG_USER_ERROR(OB_OPERATE_OVERFLOW, "TIME", "json_value");
  } else if (OB_FAIL(time_scale_check(accuracy, val))) {
    LOG_WARN("time_scale_check failed.", K(ret));
  }

  return ret;
}

int JtFuncHelpler::cast_to_year(ObIJsonBase *j_base, uint8_t &val)
{
  INIT_SUCC(ret);
  int64_t int_val;
  const uint16 min_year = 1901;
  const uint16 max_year = 2155;

  if (OB_ISNULL(j_base)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("json base is null", K(ret));
  } else if (OB_FAIL(j_base->to_int(int_val))) {
    LOG_WARN("wrapper to year failed.", K(ret), K(*j_base));
  } else if (0 != int_val && (int_val < min_year || int_val > max_year)) {
    // different with cast, if 0 < int val < 100, do not add base year
    LOG_DEBUG("int out of year range", K(int_val));
    ret = OB_DATA_OUT_OF_RANGE;
  } else if(OB_FAIL(ObTimeConverter::int_to_year(int_val, val))) {
    LOG_WARN("int to year failed.", K(ret), K(int_val));
  }

  return ret;
}

int JtFuncHelpler::cast_to_float(ObIJsonBase *j_base, ObObjType dst_type, float &val)
{
  INIT_SUCC(ret);
  double tmp_val;

  if (OB_ISNULL(j_base)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("json base is null", K(ret));
  } else if (OB_FAIL(j_base->to_double(tmp_val))) {
    LOG_WARN("wrapper to date failed.", K(ret), K(*j_base));
  } else {
    val = static_cast<float>(tmp_val);
    if (lib::is_mysql_mode() && OB_FAIL(real_range_check(dst_type, tmp_val, val))) {
      LOG_WARN("real_range_check failed", K(ret), K(tmp_val));
    }
  }

  return ret;
}

int JtFuncHelpler::cast_to_double(ObIJsonBase *j_base, ObObjType dst_type, double &val)
{
  INIT_SUCC(ret);

  if (OB_ISNULL(j_base)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("json base is null", K(ret));
  } else if (OB_FAIL(j_base->to_double(val))) {
    LOG_WARN("wrapper to date failed.", K(ret), K(*j_base));
  } else if (ObUDoubleType == dst_type && OB_FAIL(numeric_negative_check(val))) {
    LOG_WARN("numeric_negative_check failed", K(ret), K(val));
  }

  return ret;
}

int JtFuncHelpler::cast_to_number(common::ObIAllocator *allocator,
                                    ObIJsonBase *j_base,
                                    common::ObAccuracy &accuracy,
                                    ObObjType dst_type,
                                    number::ObNumber &val)
{
  INIT_SUCC(ret);

  if (OB_ISNULL(j_base)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("json base is null", K(ret));
  } else if (OB_FAIL(j_base->to_number(allocator, val))) {
    LOG_WARN("fail to cast json as decimal", K(ret));
  } else if (ObUNumberType == dst_type && OB_FAIL(numeric_negative_check(val))) {
    LOG_WARN("numeric_negative_check failed", K(ret), K(val));
  } else if (OB_FAIL(number_range_check(accuracy, allocator, val))) {
    LOG_WARN("number_range_check failed", K(ret), K(val));
  }

  return ret;
}

int JtFuncHelpler::cast_to_bit(ObIJsonBase *j_base, uint64_t &val)
{
  INIT_SUCC(ret);

  if (OB_ISNULL(j_base)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("json base is null", K(ret));
  } else if (OB_FAIL(j_base->to_bit(val))) {
    LOG_WARN("fail get bit from json", K(ret));
  }

  return ret;
}

int JtFuncHelpler::cast_to_json(common::ObIAllocator *allocator, ObIJsonBase *j_base, ObString &val)
{
  INIT_SUCC(ret);

  if (OB_ISNULL(j_base)) {
    ret = OB_ERR_NULL_VALUE;
    LOG_WARN("json base is null", K(ret));
  } else if (OB_FAIL(j_base->get_raw_binary(val, allocator))) {
    LOG_WARN("failed to get raw binary", K(ret));
  }

  return ret;
}

int JtFuncHelpler::cast_to_res(JtScanCtx* ctx, ObIJsonBase* js_val, JtColNode& col_node, bool enable_error = true)
{
  INIT_SUCC(ret);
  ObJtColInfo& col_info = col_node.get_column_def();
  bool is_truncate = static_cast<bool>(col_info.truncate_);

  ObExpr* expr = ctx->spec_ptr_->column_exprs_.at(col_info.output_column_idx_);
  ObDatum& res = expr->locate_datum_for_write(*ctx->eval_ctx_);
  ctx->res_obj_ = &res;

  ObObjType dst_type = expr->datum_meta_.type_;
  ObCollationType coll_type = expr->datum_meta_.cs_type_;
  ObAccuracy accuracy = col_info.data_type_.get_accuracy();
  ObCollationType dst_coll_type = col_info.data_type_.get_collation_type();
  ObCollationType in_coll_type = ctx->is_charset_converted_
                                 ? CS_TYPE_UTF8MB4_BIN
                                 : ctx->spec_ptr_->value_expr_->datum_meta_.cs_type_;
  ObCollationLevel dst_coll_level = col_info.data_type_.get_collation_level();

  switch (dst_type) {
    case ObNullType : {
      res.set_null();
      break;
    }
    case ObTinyIntType:
    case ObSmallIntType:
    case ObMediumIntType:
    case ObInt32Type:
    case ObIntType: {
      int64_t val;
      ret = cast_to_int(js_val, dst_type, val);
      if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        if (dst_type == ObIntType) {
          res.set_int(val);
        } else {
          res.set_int32(static_cast<int32_t>(val));
        }
      }
      break;
    }
    case ObUTinyIntType:
    case ObUSmallIntType:
    case ObUMediumIntType:
    case ObUInt32Type:
    case ObUInt64Type: {
      uint64_t val;
      ret = cast_to_uint(js_val, dst_type, val);
      if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        if (dst_type == ObUInt64Type) {
          res.set_uint(val);
        } else {
          res.set_uint32(static_cast<uint32_t>(val));
        }
      }
      break;
    }
    case ObDateTimeType: {
      const ObBasicSessionInfo *session = ctx->exec_ctx_->get_my_session();
      int64_t val;
      ret = cast_to_datetime(&col_node, js_val, &ctx->row_alloc_, session, accuracy, val);
      if (ret == OB_ERR_NULL_VALUE) {
        res.set_null();
      } else if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        res.set_datetime(val);
      }
      break;
    }
    case ObTimestampNanoType:
    case ObTimestampTZType:
    case ObTimestampLTZType:
    case ObTimestampType: {
      const ObBasicSessionInfo *session = ctx->exec_ctx_->get_my_session();
      ObOTimestampData val;
      ret = cast_to_otimstamp(js_val, session, accuracy, dst_type, val);
      if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        res.set_otimestamp_tiny(val);
      }
      break;
    }
    case ObDateType: {
      int32_t val;
      ret = cast_to_date(js_val, val);
      if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        res.set_date(val);
      }
      break;
    }
    case ObTimeType: {
      int64_t val;
      ret = cast_to_time(js_val, accuracy, val);
      if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        res.set_time(val);
      }
      break;
    }
    case ObYearType: {
      uint8_t val;
      ret = cast_to_year(js_val, val);
      if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        res.set_year(val);
      }
      break;
    }
    case ObNumberFloatType:
    case ObFloatType:
    case ObUFloatType: {
      float out_val;
      ret = cast_to_float(js_val, dst_type, out_val);
      if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        res.set_float(out_val);
      }
      break;
    }
    case ObDoubleType:
    case ObUDoubleType: {
      double out_val;
      ret = cast_to_double(js_val, dst_type, out_val);
      if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        res.set_double(out_val);
      }
      break;
    }
    case ObUNumberType:
    case ObNumberType: {
      number::ObNumber out_val;
      ret = cast_to_number(&ctx->row_alloc_, js_val, accuracy, dst_type, out_val);
      if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        res.set_number(out_val);
      }
      break;
    }
    case ObVarcharType:
    case ObRawType:
    case ObNVarchar2Type:
    case ObNCharType:
    case ObCharType:
    case ObTinyTextType:
    case ObTextType :
    case ObMediumTextType:
    case ObHexStringType:
    case ObLongTextType: {
      ObString val;
      bool is_quote = (col_info.col_type_ == COL_TYPE_QUERY && js_val->json_type() == ObJsonNodeType::J_STRING);
      ret = cast_to_string(&col_node, &ctx->row_alloc_, js_val, in_coll_type, dst_coll_type, accuracy, dst_type, val, is_truncate, is_quote);
      if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        res.set_string(val);
      }
      break;
    }
    case ObBitType: {
      uint64_t out_val;
      ret = cast_to_bit(js_val, out_val);
      if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        res.set_bit(out_val);
      }
      break;
    }
    case ObJsonType: {
      ObString out_val;
      ret = cast_to_json(&ctx->row_alloc_, js_val, out_val);
      if (OB_FAIL(ret) && enable_error) {
        int tmp_ret = set_error_val(ctx, col_node, ret);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed to set error val.", K(tmp_ret));
        }
      } else {
        char *buf = static_cast<char *>(ctx->row_alloc_.alloc(out_val.length()));
        if (OB_UNLIKELY(buf == NULL)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("fail to alloc memory for json array result", K(ret), K(out_val.length()));
        } else {
          MEMCPY(buf, out_val.ptr(), out_val.length());
          out_val.assign_ptr(buf, out_val.length());
          res.set_string(out_val);
        }
      }
      break;
    }
    default: {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected dst_type", K(dst_type));
      break;
    }
  }

  if (OB_SUCC(ret) && is_lob_storage(dst_type)) {
    ObString val = res.get_string();
    if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(*expr, *ctx->eval_ctx_, res, val, &ctx->row_alloc_))) {
      LOG_WARN("fail to pack res result.", K(ret));
    }
  }
  LOG_DEBUG("finish cast_to_res.", K(ret), K(dst_type));

  return ret;
}

int JtFuncHelpler::set_error_val(JtScanCtx* ctx, JtColNode& col_node, int& ret)
{
  INIT_SUCC(tmp_ret);
  if (ret == OB_SUCCESS) {
  } else {
    const ObJtColInfo& info = col_node.col_info_;
    JtColType col_type = col_node.type();
    if (col_type == COL_TYPE_VALUE) {
      if (info.on_error_ == JSN_VALUE_ERROR || info.on_empty_ == JSN_VALUE_ERROR) {
        EVAL_COVER_CODE(ctx, ret) ;
        if (OB_SUCC(ret) && ctx->is_need_end_) {
          ret = OB_ITER_END;
        }
      } else if (info.on_error_ == JSN_VALUE_DEFAULT) {
        ObExpr* default_expr = ctx->spec_ptr_->err_default_exprs_.at(col_node.col_info_.error_expr_id_);
        ObDatum* err_datum = nullptr;
        tmp_ret = default_expr->eval(*ctx->eval_ctx_, err_datum);
        if (tmp_ret != OB_SUCCESS) {
          LOG_WARN("failed do cast to returning type.", K(tmp_ret));
        } else {
          ObBasicSessionInfo *session = ctx->exec_ctx_->get_my_session();
          const ObDatum& datum = *err_datum;
          const ObString in_str = ob_is_string_type(default_expr->datum_meta_.type_) ? datum.get_string() : ObString();

          if (OB_SUCCESS != (tmp_ret = col_node.check_default_cast_allowed(default_expr))) {
            ret = tmp_ret;
            LOG_WARN("check default value can't cast return type", K(tmp_ret), K(default_expr->datum_meta_));
          } else if (OB_FAIL(ObJsonExprHelper::pre_default_value_check(col_node.col_info_.data_type_.get_obj_type(),
                                                                       in_str,
                                                                       default_expr->datum_meta_.type_))) {
            LOG_WARN("default value pre check fail", K(ret));
          } else if (ObJsonExprHelper::is_convertible_to_json(default_expr->datum_meta_.type_)) {
            if (OB_SUCCESS != (tmp_ret = ObJsonExprHelper::transform_convertible_2jsonBase(datum,
                                                                          default_expr->datum_meta_.type_,
                                                                          &ctx->row_alloc_,
                                                                          default_expr->datum_meta_.cs_type_,
                                                                          col_node.err_val_, false,
                                                                          default_expr->obj_meta_.has_lob_header()))) {
              LOG_WARN("failed: parse value to jsonBase", K(tmp_ret));
            }
          } else if (OB_SUCCESS != (tmp_ret = ObJsonExprHelper::transform_scalar_2jsonBase(datum,
                                                                          default_expr->datum_meta_.type_,
                                                                          &ctx->row_alloc_,
                                                                          default_expr->datum_meta_.scale_,
                                                                          session->get_timezone_info(),
                                                                          session,
                                                                          col_node.err_val_, false))) {
            LOG_WARN("failed do cast to returning type.", K(tmp_ret));
          }
        }
        if (tmp_ret == OB_SUCCESS) {
          if (OB_FAIL(JtFuncHelpler::cast_to_res(ctx, col_node.err_val_, col_node, false))) {
            LOG_WARN("failed do cast defaut value to returning type.", K(ret));
          } else {
            ret = OB_SUCCESS;
          }
        }
      } else if (info.on_error_ == JSN_VALUE_NULL
                || info.on_error_ == JSN_VALUE_IMPLICIT
                || info.on_empty_ == JSN_VALUE_NULL
                || info.on_empty_ == JSN_VALUE_IMPLICIT) {
        col_node.is_null_result_ = true;
        ret = ctx->is_need_end_ ? OB_ITER_END : OB_SUCCESS;
      }
    } else if (col_type == COL_TYPE_QUERY) {
      if (info.on_error_ == JSN_QUERY_EMPTY || info.on_error_ == JSN_QUERY_EMPTY_ARRAY) {
        col_node.curr_ = ObJsonTableOp::get_js_array();
        col_node.is_null_result_ = false;
        ret = ctx->is_need_end_ ? OB_ITER_END : OB_SUCCESS;
      } else if (info.on_error_ == JSN_QUERY_EMPTY_OBJECT) {
        col_node.curr_ = ObJsonTableOp::get_js_object();
        col_node.is_null_result_ = false;
        ret = ctx->is_need_end_ ? OB_ITER_END : OB_SUCCESS;
      } else if (info.on_error_ == JSN_QUERY_NULL || info.on_error_ == JSN_QUERY_IMPLICIT) {
        if (info.on_mismatch_ == JSN_QUERY_MISMATCH_ERROR) {
          ret = ctx->error_code_;
        } else {
          col_node.is_null_result_ = true;
          ret = ctx->is_need_end_ ? OB_ITER_END : OB_SUCCESS;
        }
      }
    } else if (col_type == COL_TYPE_EXISTS) {
      int is_true = 0;
      if (info.on_error_ == JSN_EXIST_ERROR) {
        ret = ctx->error_code_;
        if (OB_SUCC(ret) && ctx->is_need_end_) {
          ret = OB_ITER_END;
        }
      } else if (info.on_error_ == JSN_EXIST_DEFAULT || info.on_error_ == JSN_EXIST_FALSE) {
        col_node.is_null_result_ = false;
        ret = ctx->is_need_end_ ? OB_ITER_END : OB_SUCCESS;
      } else if (info.on_error_ == JSN_EXIST_TRUE) {
        is_true = 0;
        col_node.is_null_result_ = false;
        ret = ctx->is_need_end_ ? OB_ITER_END : OB_SUCCESS;
      }

      if (OB_FAIL(ret)) {
      } else if (ob_is_string_type(info.data_type_.get_obj_type())) {
        ObString value = is_true ? ObString("true") : ObString("false");
        void* buf = ctx->row_alloc_.alloc(sizeof(ObJsonString));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          col_node.curr_ = static_cast<ObJsonString*>(new(buf)ObJsonString(value.ptr(), value.length()));
          col_node.is_null_result_ = false;
        }
      } else if (ob_is_number_tc(info.data_type_.get_obj_type())) {
        void* buf = ctx->row_alloc_.alloc(sizeof(ObJsonInt));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("buf allocate failed", K(ret));
        } else {
          col_node.curr_ = static_cast<ObJsonInt*>(new(buf)ObJsonInt(is_true));
          col_node.is_null_result_ = false;
        }
      } else {
        if (col_node.col_info_.on_error_ != JSN_EXIST_ERROR) {
          col_node.curr_ = nullptr;
          col_node.is_null_result_ = true;
        } else {
          ret = OB_ERR_NON_NUMERIC_CHARACTER_VALUE;
        }
      }

      if (OB_SUCC(ret)) {
        if (col_node.is_null_result_) {
          ObExpr* expr = ctx->spec_ptr_->column_exprs_.at(col_node.col_info_.output_column_idx_);
          expr->locate_datum_for_write(*ctx->eval_ctx_).set_null();
        } else if (OB_FAIL(JtFuncHelpler::cast_to_res(ctx, col_node.curr_, col_node, false))) {
          LOG_WARN("failed do cast defaut value to returning type.", K(ret));
        }
      }
    }
  }
  return ret;
}

int JtFuncHelpler::check_default_val_accuracy(const ObAccuracy &accuracy,
                                              const ObObjType &type,
                                              const ObDatum *obj)
{
  INIT_SUCC(ret);
  ObObjTypeClass tc = ob_obj_type_class(type);

  switch (tc) {
    case ObNumberTC: {
      number::ObNumber temp(obj->get_number());
      ret = number_range_check(accuracy, NULL, temp, true);
      LOG_WARN("number range is invalid for json_value", K(ret));
      break;
    }
    case ObDateTC: {
      int32_t val = obj->get_date();
      if (val == ObTimeConverter::ZERO_DATE) {
        // check zero date for scale over mode
        ret = OB_INVALID_DATE_VALUE;
        LOG_WARN("Zero date is invalid for json_value", K(ret));
      }
      break;
    }
    case ObTimeTC: {
      int64_t val = obj->get_time();
      ret = time_scale_check(accuracy, val, true);
      break;
    }
    case ObStringTC :
    case ObTextTC : {
      ObString val = obj->get_string();
      const int32_t str_len_char = static_cast<int32_t>(ObCharset::strlen_char(CS_TYPE_UTF8MB4_BIN,
          val.ptr(), val.length()));
      const ObLength max_accuracy_len = (lib::is_oracle_mode() && tc == ObTextTC) ? OB_MAX_LONGTEXT_LENGTH : accuracy.get_length();
      if (OB_SUCC(ret)) {
        if (max_accuracy_len == DEFAULT_STR_LENGTH) { // default string len
        } else if (max_accuracy_len <= 0 || str_len_char > max_accuracy_len) {
          if (lib::is_mysql_mode()) {
            ret = OB_OPERATE_OVERFLOW;
            LOG_USER_ERROR(OB_OPERATE_OVERFLOW, "STRING", "json_value");
          } else {
            ret = OB_ERR_VALUE_EXCEEDED_MAX;
            LOG_USER_ERROR(OB_ERR_VALUE_EXCEEDED_MAX, str_len_char, max_accuracy_len);
          }
        }
      }
      break;
    }
    default:
      break;
  }

  return ret;
}


int JtFuncHelpler::check_default_value_inner(JtScanCtx* ctx,
                                            ObJtColInfo &col_info,
                                            ObExpr* col_expr,
                                            ObExpr* default_expr)
{
  INIT_SUCC(ret);

  ObString in_str;
  ObDatum *emp_datum = nullptr;

  if (OB_FAIL(default_expr->eval(*ctx->eval_ctx_, emp_datum))) {
    LOG_WARN("failed do cast to returning type.", K(ret));
  } else {
    in_str.assign_ptr(emp_datum->ptr_, emp_datum->len_);
  }
  if (OB_FAIL(ret)) {
  } else if ((default_expr->datum_meta_.type_ == ObNullType || emp_datum->is_null())
             && ob_is_string_type(col_info.data_type_.get_obj_type())) {
    ret = OB_ERR_DEFAULT_VALUE_NOT_LITERAL;
    LOG_WARN("default value not match returing type", K(ret));
  } else if (OB_FAIL(ObJsonExprHelper::pre_default_value_check(col_expr->datum_meta_.type_, in_str, default_expr->datum_meta_.type_))) {
    LOG_WARN("default value pre check fail", K(ret), K(in_str));
  } else if (OB_FAIL(JtFuncHelpler::check_default_val_accuracy(col_info.data_type_.get_accuracy(), col_expr->datum_meta_.type_, emp_datum))) {
    LOG_WARN("fail to check accuracy", K(ret));
  }

  return ret;
}

int JtFuncHelpler::check_default_value(JtScanCtx* ctx,
                                       ObJtColInfo &col_info,
                                       ObExpr* expr)
{
  INIT_SUCC(ret);
  if (static_cast<JtColType>(col_info.col_type_) == COL_TYPE_VALUE) {
    if (col_info.on_empty_ == JSN_VALUE_DEFAULT) {
      ObExpr* default_expr = ctx->spec_ptr_->emp_default_exprs_.at(col_info.empty_expr_id_);
      if (OB_FAIL(check_default_value_inner(ctx, col_info, expr, default_expr))) {
        LOG_WARN("fail to check empty default value", K(ret));
      }
    }

    if (OB_SUCC(ret) && col_info.on_error_ == JSN_VALUE_DEFAULT) {
      ObExpr* default_expr = ctx->spec_ptr_->err_default_exprs_.at(col_info.error_expr_id_);
      if (OB_FAIL(check_default_value_inner(ctx, col_info, expr, default_expr))) {
        LOG_WARN("fail to check error default value", K(ret));
      }
    }
  }

  return ret;
}

int JtColNode::open()
{
  INIT_SUCC(ret);

  cur_pos_ = 0;
  is_evaled_ = false;
  curr_ = nullptr;
  iter_ = nullptr;
  is_sub_evaled_ = false;
  ord_val_ = 0;
  total_ = 0;
  is_null_result_ = false;

  if (node_type_ == REG_TYPE) {
    total_ = 1;
  }

  return ret;
}

int JtJoinNode::open()
{
  INIT_SUCC(ret);
  if (OB_FAIL(JtColNode::open())) {
    LOG_WARN("fail to open column node.", K(ret));
  } else if (left_ && OB_FAIL(left_->open())) {
    LOG_WARN("fail to open left node.", K(ret));
  } else if (right_ && OB_FAIL(right_->open())) {
    LOG_WARN("fail to open right node.", K(ret));
  }
  return ret;
}


int JtScanNode::assign(const JtScanNode& other)
{
  INIT_SUCC(ret);

  if (OB_FAIL(reg_col_defs_.assign(other.reg_col_defs_))) {
    LOG_WARN("fail to assign col defs.", K(ret));
  } else {
    col_info_ = other.col_info_;
    nest_col_def_ = other.nest_col_def_;
    is_regular_done_ = false;
    is_nested_done_ = false;
  }
  return ret;
}

int JtScanNode::open()
{
  INIT_SUCC(ret);
  if (OB_FAIL(JtColNode::open())) {
    LOG_WARN("fail to open column node.", K(ret));
  } else {
    for (int i = 0; OB_SUCC(ret) && i < reg_col_defs_.count(); ++i) {
      JtColNode* node = reg_col_defs_.at(i);
      if (OB_FAIL(node->open())) {
        LOG_WARN("fail to open reg node.", K(ret));
      }
    }

    if (OB_FAIL(ret)) {
    } else if (reg_col_defs_.count() == 0) {
      is_regular_done_ = true;
    }

    if (OB_FAIL(ret)) {
    } else if (nest_col_def_ && OB_FAIL(nest_col_def_->open())) {
      LOG_WARN("fail to open nest def node.", K(ret));
    }
  }
  return ret;
}

void JtColNode::destroy()
{
  // do nothing
}

void JtJoinNode::destroy()
{
  if (OB_NOT_NULL(left_)) {
    left_->destroy();
  }

  if (OB_NOT_NULL(right_)) {
    right_->destroy();
  }
}

void JtScanNode::destroy()
{
  for (size_t i = 0; i < reg_col_defs_.count(); ++i) {
    reg_col_defs_.at(i)->destroy();
  }

  reg_col_defs_.reset();

  if (OB_NOT_NULL(nest_col_def_)) {
    nest_col_def_->destroy();
  }
}

int JtColNode::check_default_cast_allowed(ObExpr* expr)
{
  INIT_SUCC(ret);
  if ((ob_is_string_type(col_info_.data_type_.get_obj_type()) && ob_is_number_tc(expr->datum_meta_.type_))
      || (col_info_.data_type_.get_obj_type() == ObJsonType && ob_is_number_tc(expr->datum_meta_.type_))) {
    ret = OB_ERR_DEFAULT_VALUE_NOT_MATCH;
  }
  return ret;
}

int JtColNode::check_col_res_type(JtScanCtx* ctx)
{
  INIT_SUCC(ret);
  ObObjType obj_type = col_info_.data_type_.get_obj_type();
  JtColType col_type = type();
  if (col_type == COL_TYPE_EXISTS) {
    if (ob_is_string_type(obj_type)
        || ob_is_numeric_type(obj_type)
        || ob_is_integer_type(obj_type)) {
      // do nothing
    } else {
      if (ob_is_json_tc(obj_type)) {
        ret = OB_ERR_USAGE_KEYWORD;
      } else {
        ret = OB_ERR_NON_NUMERIC_CHARACTER_VALUE;
        SET_COVER_ERROR(ctx, ret);
      }
      curr_ = nullptr;
      is_null_result_ = true;
    }
  } else if (col_type == COL_TYPE_QUERY ) {
    // do nothing
  }

  return ret;
}

int JtColNode::set_val_on_empty(JtScanCtx* ctx, bool& need_cast_res)
{
  INIT_SUCC(ret);
  JtColType col_type = type();

  if (col_type == COL_TYPE_QUERY) {
    switch (col_info_.on_empty_) {
      case JSN_QUERY_ERROR: {
        ret = OB_ERR_JSON_VALUE_NO_VALUE;
        if (col_info_.on_empty_ == JSN_QUERY_ERROR) {
          ctx->is_cover_error_ = 0;
        }
        LOG_WARN("json value seek result empty.");
        break;
      }
      case JSN_QUERY_IMPLICIT:
      case JSN_QUERY_NULL: {
        iter_ = curr_ = nullptr;
        is_null_result_ = true;
        ret = OB_SUCCESS;
        if (col_info_.on_empty_ ==  JSN_QUERY_IMPLICIT
            && col_info_.on_error_ == JSN_QUERY_ERROR) {
          ret = OB_ERR_JSON_VALUE_NO_VALUE;
        } else if (col_info_.on_empty_ ==  JSN_QUERY_IMPLICIT
                   && (col_info_.on_error_ == JSN_QUERY_EMPTY || col_info_.on_error_ == JSN_QUERY_EMPTY_ARRAY)) {
          iter_ = curr_ = ObJsonTableOp::get_js_array();
          is_null_result_ = false;
        } else if (col_info_.on_empty_ ==  JSN_QUERY_IMPLICIT
                   && (col_info_.on_error_ == JSN_QUERY_EMPTY || col_info_.on_error_ == JSN_QUERY_EMPTY_OBJECT)) {
          iter_ = curr_ = ObJsonTableOp::get_js_object();
          is_null_result_ = false;
        }
        break;
      }
      case JSN_QUERY_EMPTY: {
        iter_ = curr_ = nullptr;
        is_null_result_ = true;
        break;
      }
      case JSN_QUERY_EMPTY_ARRAY: {
        iter_ = curr_ = ObJsonTableOp::get_js_array();
        is_null_result_ = false;
        break;
      }
      case JSN_QUERY_EMPTY_OBJECT: {
        iter_ = curr_ = ObJsonTableOp::get_js_object();
        is_null_result_ = false;
        break;
      }
      default:  // error_type from get_on_empty_or_error has done range check, do nothing for default
        break;
    }
  } else if (col_type == COL_TYPE_VALUE) {
    switch (col_info_.on_empty_) {
      case JSN_VALUE_ERROR: {
        ret = OB_ERR_JSON_VALUE_NO_VALUE;
        if (col_info_.on_empty_ == JSN_VALUE_ERROR) {
          SET_COVER_ERROR(ctx, ret);
        }
        break;
      }
      case JSN_VALUE_IMPLICIT: {
        if (col_info_.on_error_ == JSN_VALUE_ERROR) {
          ret = OB_ERR_JSON_VALUE_NO_VALUE;
          SET_COVER_ERROR(ctx, ret);
        } else if (col_info_.on_error_ == JSN_VALUE_DEFAULT) {
          ObExpr* default_expr = ctx->spec_ptr_->err_default_exprs_.at(col_info_.error_expr_id_);
          ObDatum* err_datum = nullptr;
          ret = default_expr->eval(*ctx->eval_ctx_, err_datum);
          if (OB_FAIL(ret)) {
            LOG_WARN("failed eval datum type.", K(ret));
          } else {
            ObBasicSessionInfo *session = ctx->exec_ctx_->get_my_session();
            const ObDatum& datum = *err_datum;
            const ObString in_str = ob_is_string_type(default_expr->datum_meta_.type_) ? datum.get_string() : ObString();

            if (OB_FAIL(check_default_cast_allowed(default_expr))) {
              LOG_WARN("check default value can't cast return type", K(ret), K(default_expr->datum_meta_));
            } else if (OB_FAIL(ObJsonExprHelper::pre_default_value_check(col_info_.data_type_.get_obj_type(),
                                                                        in_str,
                                                                        default_expr->datum_meta_.type_))) {
              LOG_WARN("default value pre check fail", K(ret));
            } else if (ObJsonExprHelper::is_convertible_to_json(default_expr->datum_meta_.type_)) {
              if (OB_FAIL(ObJsonExprHelper::transform_convertible_2jsonBase(datum,
                                                                            default_expr->datum_meta_.type_,
                                                                            &ctx->row_alloc_,
                                                                            default_expr->datum_meta_.cs_type_,
                                                                            err_val_, false,
                                                                            default_expr->obj_meta_.has_lob_header()))) {
                LOG_WARN("failed: parse value to jsonBase", K(ret));
              } else {
                curr_ = iter_ = err_val_;
              }
            } else if (OB_FAIL(ObJsonExprHelper::transform_scalar_2jsonBase(datum,
                                                                            default_expr->datum_meta_.type_,
                                                                            &ctx->row_alloc_,
                                                                            default_expr->datum_meta_.scale_,
                                                                            session->get_timezone_info(),
                                                                            session,
                                                                            err_val_, false))) {
              LOG_WARN("failed do cast to returning type.", K(ret));
            } else {
              curr_ = iter_ = err_val_;
            }
          }
        }
        break;
      }
      case JSN_VALUE_NULL: {
        curr_ = nullptr;
        is_null_result_ = true;
        ret = OB_SUCCESS;
        break;
      }
      case JSN_VALUE_DEFAULT: {
        ObExpr* default_expr = ctx->spec_ptr_->emp_default_exprs_.at(col_info_.empty_expr_id_);
        ObDatum* emp_datum = nullptr;
        if (OB_FAIL(default_expr->eval(*ctx->eval_ctx_, emp_datum))) {
          LOG_WARN("failed do cast to returning type.", K(ret));
        } else {
          ObBasicSessionInfo *session = ctx->exec_ctx_->get_my_session();
          ObIJsonBase* tmp_node = nullptr;
          const ObDatum& datum = *emp_datum;

          if (OB_FAIL(check_default_cast_allowed(default_expr))) {
            LOG_WARN("check default value can't cast return type", K(ret), K(default_expr->datum_meta_));
          } else if (ObJsonExprHelper::is_convertible_to_json(default_expr->datum_meta_.type_)) {
            if (OB_FAIL(ObJsonExprHelper::transform_convertible_2jsonBase(datum,
                                                                          default_expr->datum_meta_.type_,
                                                                          &ctx->row_alloc_,
                                                                          default_expr->datum_meta_.cs_type_,
                                                                          emp_val_, false,
                                                                          default_expr->obj_meta_.has_lob_header()))) {
              LOG_WARN("failed: parse value to jsonBase", K(ret));
            }
          } else if (OB_FAIL(ObJsonExprHelper::transform_scalar_2jsonBase(datum,
                                                                          default_expr->datum_meta_.type_,
                                                                          &ctx->row_alloc_,
                                                                          default_expr->datum_meta_.scale_,
                                                                          session->get_timezone_info(),
                                                                          session, emp_val_, false))) {
            LOG_WARN("failed do cast to returning type.", K(ret));
          } else {
            iter_ = emp_val_;
          }
        }

        if (OB_SUCC(ret)) {
          if (OB_FAIL(JtFuncHelpler::cast_to_res(ctx, emp_val_, *this, false))) {
              LOG_WARN("failed do cast to returning type.", K(ret));
          }
          need_cast_res = false;
        }
        break;
      }
      default:  // error_type from get_on_empty_or_error has done range check, do nothing for default
        break;
    }
  } else if (col_type == COL_TYPE_EXISTS) {
    switch (col_info_.on_empty_) {
      case JSN_EXIST_FALSE:
      case JSN_EXIST_TRUE:
      case JSN_EXIST_ERROR:
      case JSN_EXIST_DEFAULT: {
        if (ob_is_string_type(col_info_.data_type_.get_obj_type())) {
          ObString value = col_info_.on_empty_ == JSN_EXIST_TRUE ? "true" : "false";
          void* buf = ctx->row_alloc_.alloc(sizeof(ObJsonString));
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            iter_ = curr_ = static_cast<ObJsonString*>(new(buf)ObJsonString(value.ptr(), value.length()));
            is_null_result_ = false;
          }
        } else {
          void* buf = ctx->row_alloc_.alloc(sizeof(ObJsonInt));
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            LOG_WARN("buf allocate failed", K(ret));
          } else {
            iter_ = curr_ = static_cast<ObJsonInt*>(new(buf)ObJsonInt(0));
            is_null_result_ = false;
          }
        }
        break;
      }
      default:
        break;
    }
  }
  return ret;
}

int JtColNode::get_next_row(ObIJsonBase* in, JtScanCtx* ctx, bool& is_null_value)
{
  INIT_SUCC(ret);
  JtColType col_type = type();
  ObExpr* col_expr = ctx->spec_ptr_->column_exprs_.at(col_info_.output_column_idx_);
  ctx->res_obj_ = &col_expr->locate_datum_for_write(*ctx->eval_ctx_);
  bool need_cast_res = true;
  bool need_pro_emtpy = false;

  if (col_type == COL_TYPE_ORDINALITY) {
    col_expr->locate_datum_for_write(*ctx->eval_ctx_).set_int(ctx->ord_val_);
    col_expr->get_eval_info(*ctx->eval_ctx_).evaluated_ = true;
  } else if (OB_FAIL(check_col_res_type(ctx))) {
    LOG_WARN("check column res type failed", K(ret), K(col_info_.data_type_), K(col_info_.col_type_));
  } else if (OB_FAIL(init_js_path(ctx))) {
    RESET_COVER_CODE(ctx);
    LOG_WARN("fail to init js path", K(ret));
  } else if (OB_FAIL(JtFuncHelpler::check_default_value(ctx, col_info_, col_expr))) {
    // json value empty need check default value first
    LOG_WARN("default value check fail", K(ret));
  } else if (OB_ISNULL(in)) {
    in_ = in;
    is_null_result_ = true;
    need_pro_emtpy = true;
    EVAL_COVER_CODE(ctx, ret);
  } else if (in != in_ || !is_evaled_) {
    in_ = in;
    is_null_result_ = false;
    ObJsonBaseVector hit;
    in_->set_allocator(&ctx->row_alloc_);
    if (OB_FAIL(in_->seek(*js_path_, js_path_->path_node_cnt(), true, false, hit))) {
      SET_COVER_ERROR(ctx, ret);
      LOG_WARN("json seek failed", K(col_info_.path_), K(ret));
    } else if (hit.size() == 0) {
      curr_ = iter_ = nullptr;
      total_ = 1;
      if (OB_FAIL(set_val_on_empty(ctx, need_cast_res))) {
        LOG_WARN("fail to process on empty", K(ret));
      }
    } else {
      is_null_result_ = false;
      curr_ = hit[0];
      total_ = 1;
      bool is_array_wrapper = false;
      if (col_type == COL_TYPE_QUERY) {
        if (col_info_.wrapper_ == JSN_QUERY_WITHOUT_WRAPPER
            || col_info_.wrapper_ == JSN_QUERY_WITHOUT_ARRAY_WRAPPER
            || col_info_.wrapper_ == JSN_QUERY_WRAPPER_IMPLICIT) {
          if (hit.size() > 1) {
            curr_ = nullptr;
            is_null_result_ = true;
            if (col_info_.on_error_ == JSN_QUERY_ERROR) {
              ret = OB_ERR_WITHOUT_ARR_WRAPPER;
              LOG_WARN("result can't be returned without array wrapper", K(ret));
            }
            SET_COVER_ERROR(ctx, ret);
          } else {
            if ((curr_->json_type() != ObJsonNodeType::J_ARRAY && curr_->json_type() != ObJsonNodeType::J_OBJECT)
                && col_info_.allow_scalar_ == JSN_QUERY_SCALARS_DISALLOW) {
              curr_ = nullptr;
              is_null_result_ = true;
              ret = OB_ERR_WITHOUT_ARR_WRAPPER;
              LOG_WARN("result can't be returned without array wrapper");
              SET_COVER_ERROR(ctx, ret);
            }
          }
        } else if (col_info_.wrapper_ == JSN_QUERY_WITH_WRAPPER
                  || col_info_.wrapper_ == JSN_QUERY_WITH_ARRAY_WRAPPER
                  || col_info_.wrapper_ == JSN_QUERY_WITH_UNCONDITIONAL_WRAPPER
                  || col_info_.wrapper_ == JSN_QUERY_WITH_UNCONDITIONAL_ARRAY_WRAPPER) {
          is_array_wrapper = true;
        } else if (col_info_.wrapper_ == JSN_QUERY_WITH_CONDITIONAL_WRAPPER
                  || col_info_.wrapper_ == JSN_QUERY_WITH_CONDITIONAL_ARRAY_WRAPPER) {
          if (hit.size() == 1) {
            if (col_info_.allow_scalar_ == JSN_QUERY_SCALARS_DISALLOW
                && curr_->json_type() != ObJsonNodeType::J_ARRAY
                && curr_->json_type() != ObJsonNodeType::J_OBJECT) {
              is_array_wrapper = 1;
            } else {
              curr_ = hit[0];
            }

          } else {
            is_array_wrapper = 1;
          }
        }

        if (is_array_wrapper) {
          void* js_arr_buf = ctx->row_alloc_.alloc(sizeof(ObJsonArray));
          ObJsonArray* js_arr_ptr = nullptr;
          if (OB_ISNULL(js_arr_buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            LOG_WARN("failed to allocate json array buf", K(ret));
          } else if (OB_ISNULL(js_arr_ptr = new (js_arr_buf) ObJsonArray(&ctx->row_alloc_))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("failed to new json array node", K(ret));
          } else {
            ObJsonNode *j_node = NULL;
            ObIJsonBase *jb_node = NULL;
            for (int32_t i = 0; OB_SUCC(ret) && i < hit.size(); i++) {
              if (OB_FAIL(ObJsonBaseFactory::transform(&ctx->row_alloc_, hit[i], ObJsonInType::JSON_TREE, jb_node))) { // to tree
                LOG_WARN("fail to transform to tree", K(ret), K(i), K(*(hit[i])));
              } else {
                j_node = static_cast<ObJsonNode *>(jb_node);
                if (OB_FAIL(js_arr_ptr->array_append(j_node->clone(&ctx->row_alloc_)))) {
                  LOG_WARN("failed to array append", K(ret), K(i), K(*j_node));
                }
              }
            }

            if (OB_SUCC(ret)) {
              curr_ = js_arr_ptr;
            }
          }
        }
      } else if (col_type == COL_TYPE_VALUE) {
        if (hit.size() > 1) {
          ret = OB_ERR_JSON_VALUE_NO_SCALAR;
          SET_COVER_ERROR(ctx, ret);
        } else if (!ob_is_json(col_expr->datum_meta_.type_)
                  && (hit[0]->json_type() == ObJsonNodeType::J_ARRAY || hit[0]->json_type() == ObJsonNodeType::J_OBJECT)) {
          ret = OB_ERR_JSON_VALUE_NO_SCALAR;
          SET_COVER_ERROR(ctx, ret);
        } else if (curr_->json_type() == ObJsonNodeType::J_BOOLEAN && ob_is_number_tc(col_info_.data_type_.get_obj_type())) {
          curr_ = nullptr;
          is_null_result_ = true;
          ret = OB_ERR_BOOL_CAST_NUMBER;
          LOG_WARN("boolean cast number cast not support");
          SET_COVER_ERROR(ctx, ret);
        } else if ((curr_->json_type() == ObJsonNodeType::J_INT
                    || curr_->json_type() == ObJsonNodeType::J_INT)
                  && (ob_is_datetime_tc(col_info_.data_type_.get_obj_type()))) {
          char* res_ptr = ctx->buf;
          int len = snprintf(ctx->buf, sizeof(ctx->buf), "%ld", curr_->get_int());
          if (len > 0) {
            ObJsonString* j_string = nullptr;
            if (OB_ISNULL(j_string = static_cast<ObJsonString*>(ctx->row_alloc_.alloc(sizeof(ObJsonString))))) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
              RESET_COVER_CODE(ctx);
              LOG_WARN("fail to allocate json string node", K(ret));
            } else {
              curr_ = new(j_string) ObJsonString(ctx->buf, len);
            }
          } else {
            ret = OB_ERR_UNEXPECTED;
            RESET_COVER_CODE(ctx);
            LOG_WARN("fail to print int value", K(ret));
          }
        }
      } else if (col_type == COL_TYPE_EXISTS) {
        if (ob_is_string_type(col_info_.data_type_.get_obj_type())) {
          ObString value("true");
          void* buf = ctx->row_alloc_.alloc(sizeof(ObJsonString));
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            curr_ = static_cast<ObJsonString*>(new(buf)ObJsonString(value.ptr(), value.length()));
            is_null_result_ = false;
          }
        } else {
          void* buf = ctx->row_alloc_.alloc(sizeof(ObJsonInt));
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            LOG_WARN("buf allocate failed", K(ret));
          } else {
            curr_ = static_cast<ObJsonInt*>(new(buf)ObJsonInt(1));
            is_null_result_ = false;
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      cur_pos_ = 0;
      is_evaled_ = true;
    }
  }

  if (OB_FAIL(ret)) {
    if (ctx->is_cover_error_) {
      int tmp_ret = JtFuncHelpler::set_error_val(ctx, *this, ret);
      if (tmp_ret != OB_SUCCESS) {
        LOG_WARN("failed to set error val.", K(tmp_ret));
      } else if (OB_ISNULL(in_) && is_evaled_) {
        ret = OB_ITER_END;
      }
    }
  } else if (col_type == COL_TYPE_EXISTS || col_type == COL_TYPE_QUERY || col_type == COL_TYPE_VALUE) {
    if (is_null_result_ || (curr_ && curr_->json_type() == ObJsonNodeType::J_NULL && !curr_->is_real_json_null(curr_))) {
      if (!need_pro_emtpy) {
        col_expr->locate_datum_for_write(*ctx->eval_ctx_).set_null();
      } else if (OB_FAIL(set_val_on_empty(ctx, need_cast_res))) {
        LOG_WARN("fail to process on empty", K(ret));
      } else if (OB_ISNULL(iter_)) {
        col_expr->locate_datum_for_write(*ctx->eval_ctx_).set_null();
      } else if (OB_FAIL(JtFuncHelpler::cast_to_res(ctx, iter_, *this, false))) {
        LOG_WARN("failed set to res type", K(ret));
      }
    } else if (need_cast_res
               && OB_FAIL(JtFuncHelpler::cast_to_res(ctx, curr_, *this))) {
      LOG_WARN("failed to do cast to res type", K(ret));
    }

    if (OB_SUCC(ret)) {
      col_expr->get_eval_info(*ctx->eval_ctx_).evaluated_ = true;
    }

    if (is_sub_evaled_) {
      ret = OB_ITER_END;
    }
  }

  return ret;
}

int JtColNode::init_js_path(JtScanCtx* ctx)
{
  INIT_SUCC(ret);
  if (!is_evaled_ && OB_ISNULL(js_path_)) {
    void* path_buf = ctx->op_exec_alloc_->alloc(sizeof(ObJsonPath));
    if (OB_ISNULL(path_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to allocate json path buffer", K(ret));
    } else {
      js_path_ = new (path_buf) ObJsonPath(col_info_.path_, ctx->op_exec_alloc_);
      if (OB_FAIL(js_path_->parse_path())) {
        ret = OB_ERR_JSON_PATH_EXPRESSION_SYNTAX_ERROR;
        LOG_USER_ERROR(OB_ERR_JSON_PATH_EXPRESSION_SYNTAX_ERROR, col_info_.path_.length(), col_info_.path_.ptr());
      }
    }
  }

  return ret;
}

ObIJsonBase* container_at(ObIJsonBase* in, int32_t pos)
{
  INIT_SUCC(ret);
  ObIJsonBase* res = nullptr;

  if (in->json_type() == ObJsonNodeType::J_ARRAY) {
    if (OB_FAIL(in->get_array_element(pos, res))) {
      LOG_WARN("fail to get array element", K(ret), K(pos));
    }
  } else if (in->json_type() == ObJsonNodeType::J_OBJECT) {
    if (OB_FAIL(in->get_object_value(pos, res))) {
      LOG_WARN("fail to get object element", K(ret), K(pos));
    }
  }

  return res;
}

int JtScanNode::add_reg_column_node(JtColNode* node, bool add_idx)
{
  INIT_SUCC(ret);
  if (add_idx && OB_FAIL(child_idx_.push_back(node->node_idx()))) {
    LOG_WARN("fail to store node id", K(ret), K(child_idx_.count()));
  } else if (OB_FAIL(reg_col_defs_.push_back(node))) {
    LOG_WARN("fail to store node ptr", K(ret), K(reg_col_defs_.count()));
  }
  return ret;
}

int JtScanNode::get_next_row(ObIJsonBase* in, JtScanCtx* ctx, bool& is_null_value)
{
  INIT_SUCC(ret);
  if (OB_FAIL(init_js_path(ctx))) {
    RESET_COVER_CODE(ctx);
    LOG_WARN("fail to init js path", K(ret));
  } else if (!is_evaled_ || in_ != in) {
    ObJsonBaseVector hit;
    is_sub_evaled_ = false;
    is_nested_evaled_ = false;
    in_ = in;
    if (!OB_ISNULL(in_) && OB_ISNULL(in_->get_allocator())) {
      in_->set_allocator(&ctx->row_alloc_);
    }
    if (OB_ISNULL(in)) {
      total_ = 1;
      is_null_result_ = is_null_value = true;
      curr_ = iter_ = nullptr;
    } else if (OB_FAIL(in_->seek(*js_path_, js_path_->path_node_cnt(), true, false, hit))) {
      LOG_WARN("json seek failed", K(col_info_.path_), K(ret));
      SET_COVER_ERROR(ctx, ret);
    } else if (hit.size() == 0) {
      total_ = 1;
      is_null_value = is_null_result_ = true;
      curr_ = iter_ = nullptr;
      if (col_info_.parent_id_ == common::OB_INVALID_ID) { ret = OB_ITER_END; }
    } else if (hit.size() == 1) {
      iter_ = curr_ = hit[0];
      is_null_value = is_null_result_ = false;
      total_ = 1;
    } else {
      is_null_value = false;
      void* js_arr_buf = ctx->row_alloc_.alloc(sizeof(ObJsonArray));
      ObJsonArray* js_arr_ptr = nullptr;
      if (OB_ISNULL(js_arr_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to allocate json array buf", K(ret));
      } else if (OB_ISNULL(js_arr_ptr = new (js_arr_buf) ObJsonArray(&ctx->row_alloc_))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to new json array node", K(ret));
      } else {
        ObJsonNode *j_node = NULL;
        ObIJsonBase *jb_node = NULL;
        for (int32_t i = 0; OB_SUCC(ret) && i < hit.size(); i++) {
          if (ObJsonBaseFactory::transform(&ctx->row_alloc_, hit[i], ObJsonInType::JSON_TREE, jb_node)) { // to tree
            LOG_WARN("fail to transform to tree", K(ret), K(i), K(*(hit[i])));
          } else {
            j_node = static_cast<ObJsonNode *>(jb_node);
            if (OB_FAIL(js_arr_ptr->array_append(j_node->clone(&ctx->row_alloc_)))) {
              LOG_WARN("failed to array append", K(ret), K(i), K(*j_node));
            }
          }
        }

        if (OB_SUCC(ret)) {
          curr_ = js_arr_ptr;
          total_ = hit.size();
          if (OB_FAIL(js_arr_ptr->get_array_element(0, iter_))) {
            LOG_WARN("failed to get array selement 0.", K(ret));
          }
        }
      }
    }

    if (OB_SUCC(ret)) {
      cur_pos_ = 0;
      is_evaled_= true;
    }
  }

  if (OB_SUCC(ret)) {
    uint32_t reg_count = reg_col_defs_.count();
    bool tmp_is_null = false;
    for (uint32_t i = 0; OB_SUCC(ret) && i < reg_count && !is_sub_evaled_; ++i) {
      JtColNode* cur_node = reg_col_defs_.at(i);
      if (cur_node->type() == COL_TYPE_ORDINALITY) {
        ctx->ord_val_ = cur_pos_ + 1;
      }
      if (OB_FAIL(cur_node->get_next_row(iter_, ctx, tmp_is_null))) {
        LOG_WARN("fail to get regular column value", K(ret));
      }
    }

    bool is_curr_row_valid = !is_sub_evaled_;
    bool is_sub_result_null = false;
    if (OB_SUCC(ret)) {
      bool is_cur_end = false;
      JtColNode* nest_node = nest_col_def_;
      if (nest_node) {
        if (OB_FAIL(nest_node->get_next_row(iter_, ctx, is_sub_result_null))) {
          if (OB_FAIL(ret) && ret != OB_ITER_END) {
            LOG_WARN("fail to get column value", K(ret));
          } else if (ret == OB_ITER_END) {
            is_cur_end = true;
            is_sub_evaled_ = true;
            if (!is_nested_evaled_) {
              is_nested_evaled_ = true;
              ret = OB_SUCCESS;
            } else {
              is_curr_row_valid = false;
            }
          }
        } else if (OB_SUCC(ret)) {
          is_nested_evaled_ = true;
        }
      } else {
        is_nested_evaled_ = true;
        is_cur_end = true;
      }

      if (is_cur_end || is_sub_result_null || iter_ == nullptr) {
        if (cur_pos_ + 1 < total_ && !is_curr_row_valid) {
          cur_pos_++;
          if (OB_ISNULL(iter_ = container_at(curr_, cur_pos_))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("fail to get container element.", K(ret), K(cur_pos_));
          } else if (is_sub_evaled_) {
            is_sub_evaled_ = false;
            is_cur_end = false;
            if (OB_FAIL(get_next_row(in_, ctx, is_sub_result_null))) {
              if (ret != OB_ITER_END) {
                LOG_WARN("fail to get next row.", K(ret), K(cur_pos_));
              }
            }
          }
        } else if (is_cur_end) {
          if (!is_curr_row_valid) {
            ret = OB_ITER_END;
          }
          is_sub_evaled_ = true;
        }
      }

      if (is_sub_result_null && (cur_pos_ + 1 > total_)) {
        is_null_value = is_sub_result_null;
      }
    }
  }

  return ret;
}

int JtJoinNode::get_next_row(ObIJsonBase* in, JtScanCtx* ctx, bool& is_null_value)
{
  INIT_SUCC(ret);

  JtColNode* left_node = left();
  JtColNode* right_node = right();

  bool is_left_null = false;
  if (OB_NOT_NULL(left_node)) {
    ret = left_node->get_next_row(in, ctx, is_null_value);
    if (OB_FAIL(ret) && ret != OB_ITER_END) {
      LOG_WARN("fail to get next row", K(ret));
    }
    is_left_null = is_null_value;
  } else {
    ret = OB_ITER_END;
  }

  if (OB_SUCC(ret)) {
    if (is_null_value && OB_NOT_NULL(right_node)) {
      ret = right_node->get_next_row(in, ctx, is_null_value);
      if (OB_FAIL(ret) &&  ret != OB_ITER_END) {
        LOG_WARN("fail to get next row", K(ret));
      }
    }
  } else if (OB_NOT_NULL(right_node) && (ret == OB_ITER_END)) {
    ret = right_node->get_next_row(in, ctx, is_null_value);
    if (OB_FAIL(ret) &&  ret != OB_ITER_END) {
      LOG_WARN("fail to get next row", K(ret));
    } else if (OB_SUCC(ret) && is_null_value) {
      if (in_ == in) {
        ret = OB_ITER_END;
      }
    }
  }

  in_ = in;
  return ret;
}


ObJtColInfo::ObJtColInfo()
  : col_type_(0),
    truncate_(0),
    format_json_(0),
    wrapper_(0),
    allow_scalar_(0),
    output_column_idx_(-1),
    empty_expr_id_(-1),
    error_expr_id_(-1),
    col_name_(),
    path_(),
    on_empty_(3),
    on_error_(3),
    on_mismatch_(3),
    on_mismatch_type_(3),
    data_type_(),
    parent_id_(-1),
    id_(-1) {}

ObJtColInfo::ObJtColInfo(const ObJtColInfo& info)
  : col_type_(info.col_type_),
    truncate_(info.truncate_),
    format_json_(info.format_json_),
    wrapper_(info.wrapper_),
    allow_scalar_(info.allow_scalar_),
    output_column_idx_(info.output_column_idx_),
    empty_expr_id_(info.empty_expr_id_),
    error_expr_id_(info.error_expr_id_),
    col_name_(info.col_name_),
    path_(info.path_),
    on_empty_(info.on_empty_),
    on_error_(info.on_error_),
    on_mismatch_(info.on_mismatch_),
    on_mismatch_type_(info.on_mismatch_type_),
    data_type_(info.data_type_),
    parent_id_(info.parent_id_),
    id_(info.id_) {}


int ObJtColInfo::deep_copy(const ObJtColInfo& src, ObIAllocator* allocator)
{
  int ret = OB_SUCCESS;
  if (src.col_name_.length() > 0) {
    void *name_buf = allocator->alloc(src.col_name_.length());
    if (OB_ISNULL(name_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(name_buf, src.col_name_.ptr(), src.col_name_.length());
      col_name_.assign(static_cast<char*>(name_buf), src.col_name_.length());
    }
  }

  if (OB_SUCC(ret) && src.path_.length() > 0) {
    void *path_buf = allocator->alloc(src.path_.length());
    if (OB_ISNULL(path_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(path_buf, src.path_.ptr(), src.path_.length());
      path_.assign(static_cast<char*>(path_buf), src.path_.length());
    }
  }

  if (OB_SUCC(ret)) {
    col_type_ = src.col_type_;
    truncate_ = src.truncate_;
    format_json_ = src.format_json_;
    wrapper_ = src.wrapper_;
    allow_scalar_ = src.allow_scalar_;
    output_column_idx_ = src.output_column_idx_;
    empty_expr_id_ = src.empty_expr_id_;
    error_expr_id_ = src.error_expr_id_;
    on_empty_ = src.on_empty_;
    on_error_ = src.on_error_;
    on_mismatch_ = src.on_mismatch_;
    on_mismatch_type_ = src.on_mismatch_type_;
    data_type_ = src.data_type_;
    parent_id_ = src.parent_id_;
    id_ = src.id_;
  }
  return ret;
}

int ObJtColInfo::serialize(char *buf, int64_t buf_len, int64_t &pos) const
{
  INIT_SUCC(ret);
  OB_UNIS_ENCODE(col_type_);
  OB_UNIS_ENCODE(truncate_);
  OB_UNIS_ENCODE(format_json_);
  OB_UNIS_ENCODE(wrapper_);
  OB_UNIS_ENCODE(allow_scalar_);
  OB_UNIS_ENCODE(output_column_idx_);
  OB_UNIS_ENCODE(empty_expr_id_);
  OB_UNIS_ENCODE(error_expr_id_);
  OB_UNIS_ENCODE(col_name_);
  OB_UNIS_ENCODE(path_);
  OB_UNIS_ENCODE(on_empty_);
  OB_UNIS_ENCODE(on_error_);
  OB_UNIS_ENCODE(on_mismatch_);
  OB_UNIS_ENCODE(on_mismatch_type_);
  OB_UNIS_ENCODE(data_type_);
  OB_UNIS_ENCODE(parent_id_);
  OB_UNIS_ENCODE(id_);

  return ret;
}

int ObJtColInfo::deserialize(const char *buf, const int64_t data_len, int64_t &pos)
{
  INIT_SUCC(ret);
  OB_UNIS_DECODE(col_type_);
  OB_UNIS_DECODE(truncate_);
  OB_UNIS_DECODE(format_json_);
  OB_UNIS_DECODE(wrapper_);
  OB_UNIS_DECODE(allow_scalar_);
  OB_UNIS_DECODE(output_column_idx_);
  OB_UNIS_DECODE(empty_expr_id_);
  OB_UNIS_DECODE(error_expr_id_);
  OB_UNIS_DECODE(col_name_);
  OB_UNIS_DECODE(path_);
  OB_UNIS_DECODE(on_empty_);
  OB_UNIS_DECODE(on_error_);
  OB_UNIS_DECODE(on_mismatch_);
  OB_UNIS_DECODE(on_mismatch_type_);
  OB_UNIS_DECODE(data_type_);
  OB_UNIS_DECODE(parent_id_);
  OB_UNIS_DECODE(id_);
  return ret;
}

int64_t ObJtColInfo::get_serialize_size() const
{
  int64_t len = 0;
  OB_UNIS_ADD_LEN(col_type_);
  OB_UNIS_ADD_LEN(truncate_);
  OB_UNIS_ADD_LEN(format_json_);
  OB_UNIS_ADD_LEN(wrapper_);
  OB_UNIS_ADD_LEN(allow_scalar_);
  OB_UNIS_ADD_LEN(output_column_idx_);
  OB_UNIS_ADD_LEN(empty_expr_id_);
  OB_UNIS_ADD_LEN(error_expr_id_);
  OB_UNIS_ADD_LEN(col_name_);
  OB_UNIS_ADD_LEN(path_);
  OB_UNIS_ADD_LEN(on_empty_);
  OB_UNIS_ADD_LEN(on_error_);
  OB_UNIS_ADD_LEN(on_mismatch_);
  OB_UNIS_ADD_LEN(on_mismatch_type_);
  OB_UNIS_ADD_LEN(data_type_);
  OB_UNIS_ADD_LEN(parent_id_);
  OB_UNIS_ADD_LEN(id_);

  return len;
}

int ObJtColInfo::from_JtColBaseInfo(const ObJtColBaseInfo& info)
{
  INIT_SUCC(ret);
  col_type_ = info.col_type_;
  truncate_ = info.truncate_;
  format_json_ = info.format_json_;
  wrapper_ = info.wrapper_;
  allow_scalar_ = info.allow_scalar_;
  output_column_idx_ = info.output_column_idx_;
  empty_expr_id_ = info.empty_expr_id_;
  error_expr_id_ = info.error_expr_id_;
  col_name_ = info.col_name_;
  path_ = info.path_;
  on_empty_ = info.on_empty_;
  on_error_ = info.on_error_;
  on_mismatch_ = info.on_mismatch_;
  on_mismatch_type_ = info.on_mismatch_type_;
  data_type_ = info.data_type_;
  parent_id_ = info.parent_id_;
  id_ = info.id_;

  return ret;
}

static int construct_jt_scan_node(ObIAllocator* allocator,
                                 const ObJtColInfo& col_info,
                                 JtScanNode*& jt_node)
{
  INIT_SUCC(ret);
  void* node_buf = static_cast<void*>(jt_node);
  if (OB_ISNULL(node_buf)) {
    node_buf = allocator->alloc(sizeof(JtScanNode));
    if (OB_ISNULL(node_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to alloc col node buffer", K(ret));
    }
    jt_node = static_cast<JtScanNode*>(new(node_buf)JtScanNode(col_info));
  } else {
    jt_node = static_cast<JtScanNode*>(new(node_buf)JtScanNode(col_info));
  }
  return ret;
}

static int construct_jt_reg_node(ObIAllocator* allocator,
                                 const ObJtColInfo& col_info,
                                 JtColNode*& jt_node)
{
  INIT_SUCC(ret);
  void* node_buf = allocator->alloc(sizeof(JtColNode));
  if (OB_ISNULL(node_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc col node buffer", K(ret));
  } else {
    jt_node = static_cast<JtColNode*>(new(node_buf)JtColNode(col_info));
  }
  return ret;
}

static int construct_jt_join_node(ObIAllocator* allocator,
                                  const ObJtColInfo& col_info,
                                  JtJoinNode*& jt_node)
{
  INIT_SUCC(ret);
  void* node_buf = allocator->alloc(sizeof(JtJoinNode));
  if (OB_ISNULL(node_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc col node buffer", K(ret));
  } else {
    jt_node = static_cast<JtJoinNode*>(new(node_buf)JtJoinNode(col_info));
  }
  return ret;
}

int ObJsonTableSpec::construct_tree(common::ObArray<JtColNode*> all_nodes, JtScanNode* parent)
{
  INIT_SUCC(ret);

  for (int64_t i = 0; i < parent->child_idx_.count(); ++i) {
    int64_t idx = parent->child_idx_.at(i);
    JtColNode* node = all_nodes.at(idx);
    if (node->node_type() == REG_TYPE) {
      if (OB_FAIL(parent->add_reg_column_node(node))) {
        LOG_WARN("fail to add reg column node", K(ret), K(i), K(idx));
      }
    } else {
      if (OB_FAIL(parent->add_nest_column_node(node))) {
        LOG_WARN("fail to add nest column node", K(ret), K(i), K(idx));
      } else {
        JtNodeType type = node->node_type();
        if (type == JOIN_TYPE && OB_FAIL(construct_tree(all_nodes, static_cast<JtJoinNode*>(node)))) {
          LOG_WARN("fail to construct join node", K(ret), K(i), K(idx));
        } else if (type == SCAN_TYPE && OB_FAIL(construct_tree(all_nodes, static_cast<JtScanNode*>(node)))) {
          LOG_WARN("fail to construct scan node", K(ret), K(i), K(idx));
        }
      }
    }
  }

  return ret;
}

int ObJsonTableSpec::construct_tree(common::ObArray<JtColNode*> all_nodes, JtJoinNode* parent)
{
  INIT_SUCC(ret);
  JtColNode* node = nullptr;
  JtNodeType type;
  int64_t left = parent->left_idx();
  if (left != OB_INVALID_ID) {
    node = all_nodes.at(left);
    type = node->node_type();
    parent->set_left(node);
    if (type == JOIN_TYPE && OB_FAIL(construct_tree(all_nodes, static_cast<JtJoinNode*>(node)))) {
      LOG_WARN("fail to construct join node", K(ret), K(left));
    } else if (type == SCAN_TYPE && OB_FAIL(construct_tree(all_nodes, static_cast<JtScanNode*>(node)))) {
      LOG_WARN("fail to construct scan node", K(ret), K(left));
    }
  }

  int64_t right = parent->right_idx();
  if (right != OB_INVALID_ID) {
    node = all_nodes.at(right);
    type = node->node_type();
    parent->set_right(node);
    if (type == JOIN_TYPE && OB_FAIL(construct_tree(all_nodes, static_cast<JtJoinNode*>(node)))) {
      LOG_WARN("fail to construct join node", K(ret), K(right));
    } else if (type == SCAN_TYPE && OB_FAIL(construct_tree(all_nodes, static_cast<JtScanNode*>(node)))) {
      LOG_WARN("fail to construct scan node", K(ret), K(right));
    }
  }
  return ret;
}

void JtColTreeNode::destroy()
{
  regular_cols_.reset();
  for (size_t i = 0; i < nested_cols_.count(); ++i) {
    JtColTreeNode* tmp_node = nested_cols_.at(i);
    tmp_node->destroy();
  }
  nested_cols_.reset();
}

OB_DEF_SERIALIZE(ObJsonTableSpec)
{
  INIT_SUCC(ret);
  BASE_SER((ObJsonTableSpec, ObOpSpec));
  OB_UNIS_ENCODE(value_expr_);
  OB_UNIS_ENCODE(column_exprs_);
  OB_UNIS_ENCODE(emp_default_exprs_);
  OB_UNIS_ENCODE(err_default_exprs_);
  OB_UNIS_ENCODE(has_correlated_expr_);
  int32_t column_count = cols_def_.count();
  OB_UNIS_ENCODE(column_count);
  for (size_t i = 0; OB_SUCC(ret) && i < cols_def_.count(); ++i) {
    const ObJtColInfo& info = *cols_def_.at(i);
    OB_UNIS_ENCODE(info);
  }

  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObJsonTableSpec)
{
  int64_t len = 0;
  BASE_ADD_LEN((ObJsonTableSpec, ObOpSpec));
  OB_UNIS_ADD_LEN(value_expr_);
  OB_UNIS_ADD_LEN(column_exprs_);
  OB_UNIS_ADD_LEN(emp_default_exprs_);
  OB_UNIS_ADD_LEN(err_default_exprs_);
  OB_UNIS_ADD_LEN(has_correlated_expr_);

  int32_t column_count = cols_def_.count();
  OB_UNIS_ADD_LEN(column_count);
  for (size_t i = 0; i < cols_def_.count(); ++i) {
    const ObJtColInfo& info = *cols_def_.at(i);
    OB_UNIS_ADD_LEN(info);
  }

  return len;
}

OB_DEF_DESERIALIZE(ObJsonTableSpec)
{
  INIT_SUCC(ret);
  BASE_DESER((ObJsonTableSpec, ObOpSpec));
  OB_UNIS_DECODE(value_expr_);
  OB_UNIS_DECODE(column_exprs_);
  OB_UNIS_DECODE(emp_default_exprs_);
  OB_UNIS_DECODE(err_default_exprs_);
  OB_UNIS_DECODE(has_correlated_expr_);

  int32_t column_count = 0;
  OB_UNIS_DECODE(column_count);

  if (OB_SUCC(ret) && OB_FAIL(cols_def_.init(column_count))) {
    LOG_WARN("fail to init cols def array.", K(ret), K(column_count));
  }

  for (size_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
    void* col_info_buf = alloc_->alloc(sizeof(ObJtColInfo));
    if (OB_ISNULL(col_info_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to allocate col node buffer.", K(ret));
    } else {
      ObJtColInfo* col_info = static_cast<ObJtColInfo*>(new (col_info_buf) ObJtColInfo());
      ObJtColInfo& tmp_col_info = *col_info;
      OB_UNIS_DECODE(tmp_col_info);
      *col_info = tmp_col_info;
      if (OB_FAIL(cols_def_.push_back(col_info))) {
        LOG_WARN("fail to store col info.", K(ret), K(cols_def_.count()));
      }
    }
  }

  return ret;
}

int ObJsonTableOp::generate_table_exec_tree(ObIAllocator* allocator,
                                            const JtColTreeNode& orig_col,
                                            JtScanNode*& scan_col,
                                            int64_t& node_idx)
{
  INIT_SUCC(ret);

  int reg_count = orig_col.regular_cols_.count();
  int nest_count = orig_col.nested_cols_.count();

  if (OB_FAIL(construct_jt_scan_node(allocator, orig_col.col_base_info_, scan_col))) {
    LOG_WARN("fail to construct scan col node", K(ret));
  } else {
    scan_col->set_idx(node_idx++);
    ObIArray<int64_t>& child_nodes = scan_col->child_node_ref();
    if (OB_FAIL(child_nodes.reserve(reg_count + (nest_count > 0 ? 1 : 0)))) {
      LOG_WARN("fail to reserve space for idx array", K(ret), K(reg_count));
    }
  }

  for (int i = 0; OB_SUCC(ret) && i < reg_count; ++i) {
    JtColNode* reg_node = nullptr;
    if (OB_FAIL(construct_jt_reg_node(allocator, orig_col.regular_cols_.at(i)->col_base_info_, reg_node))) {
      LOG_WARN("fail to construct reg col node", K(ret), K(reg_count), K(i));
    } else {
      reg_node->set_idx(node_idx++);
      if (OB_FAIL(scan_col->add_reg_column_node(reg_node))) {
        LOG_WARN("fail to store col node", K(ret), K(reg_count), K(i));
      }
    }
  }

  if (OB_SUCC(ret) && nest_count > 0) {
    common::ObArray<JtJoinNode*> ji_nodes;
    for (size_t i = 0; OB_SUCC(ret) && i < nest_count; ++i) {
      JtJoinNode* ji_node = nullptr;
      if (OB_FAIL(construct_jt_join_node(allocator, orig_col.nested_cols_.at(i)->col_base_info_, ji_node))) {
        LOG_WARN("fail to construct join col node", K(ret));
      } else if (OB_FAIL(ji_nodes.push_back(ji_node))) {
        LOG_WARN("fail to store ji nodes in tmp array", K(ret), K(nest_count), K(i));
      } else {
        ji_node->set_idx(node_idx++);
      }
    }

    if (OB_SUCC(ret)) {
      int j = 0;
      JtJoinNode* last_node = nullptr;
      scan_col->add_nest_column_node(ji_nodes.at(j));
      ji_nodes.at(j)->set_join_type(RIGHT_TYPE);

      last_node = ji_nodes.at(j);
      ++j;

      while (j < nest_count) {
        JtJoinNode* cur_node = ji_nodes.at(j);

        last_node->set_left(cur_node);
        cur_node->set_join_type(LEFT_TYPE);
        last_node = cur_node;
        ++j;
      };

      for (int i = 0; OB_SUCC(ret) && i < nest_count; ++i) {
        JtScanNode* col_node = nullptr;
        if (OB_FAIL(generate_table_exec_tree(allocator, *orig_col.nested_cols_.at(i), col_node, node_idx))) {
          LOG_WARN("fail to generate sub col node", K(ret), K(i));
        } else {
          ji_nodes.at(i)->set_join_type(RIGHT_TYPE);
          ji_nodes.at(i)->set_right(col_node);
        }
      }
    }
  }

  return ret;
}

int ObJsonTableOp::generate_table_exec_tree()
{
  INIT_SUCC(ret);
  int64_t node_idx = 0;
  if (OB_FAIL(generate_column_trees(def_root_))) {
    LOG_WARN("fail to generate column tree", K(ret));
  } else if (OB_FAIL(generate_table_exec_tree(allocator_, *def_root_, jt_root_, node_idx))) {
    LOG_WARN("fail to generate sub col node", K(ret));
  }
  return ret;
}

int ObJsonTableSpec::dup_origin_column_defs(ObIArray<ObJtColBaseInfo*>& columns)
{
  INIT_SUCC(ret);

  for (size_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    void* col_info_buf = alloc_->alloc(sizeof(ObJtColInfo));
    if (OB_ISNULL(col_info_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to allocate col node buffer.", K(ret));
    } else {
      ObJtColInfo col_info;
      if (OB_FAIL(col_info.from_JtColBaseInfo(*columns.at(i)))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to transform to jtcolinfo", K(ret));
      } else {
        ObJtColInfo* col = static_cast<ObJtColInfo*>(new (col_info_buf) ObJtColInfo());
        if (OB_FAIL(col->deep_copy(col_info, alloc_))) {
          LOG_WARN("fail to deep copy col node", K(ret));
        } else if (OB_FAIL(cols_def_.push_back(col))) {
          LOG_WARN("fail to store col node", K(cols_def_.count()), K(ret));
        }
      }
    }
  }

  return ret;
}

int ObJsonTableOp::find_column(int32_t id, JtColTreeNode* root, JtColTreeNode*& col)
{
  INIT_SUCC(ret);
  common::ObArray<JtColTreeNode*> col_stack;
  if (OB_FAIL(col_stack.push_back(root))) {
    LOG_WARN("fail to store col node tmp", K(ret));
  }

  bool exists = false;

  while (OB_SUCC(ret) && !exists && col_stack.count() > 0) {
    JtColTreeNode* cur_col = col_stack.at(col_stack.count() - 1);
    if (cur_col->col_base_info_.id_ == id) {
      exists = true;
      col = cur_col;
    } else if (cur_col->col_base_info_.parent_id_ < 0
               || cur_col->col_base_info_.col_type_ == static_cast<int32_t>(NESTED_COL_TYPE)) {
      col_stack.remove(col_stack.count() - 1);
      for (size_t i = 0; !exists && i < cur_col->nested_cols_.count(); ++i) {
        JtColTreeNode* nest_col = cur_col->nested_cols_.at(i);
        if (nest_col->col_base_info_.id_ == id) {
          exists = true;
          col = nest_col;
        } else if (nest_col->col_base_info_.col_type_ == static_cast<int32_t>(NESTED_COL_TYPE)
                  && OB_FAIL(col_stack.push_back(nest_col))) {
          LOG_WARN("fail to store col node tmp", K(ret));
        }
      }
    }
  }

  if (OB_SUCC(ret) && !exists) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to find col node", K(ret));
  }
  return ret;
}

int ObJsonTableOp::generate_column_trees(JtColTreeNode*& root)
{
  INIT_SUCC(ret);

  const ObJsonTableSpec* spec_ptr = reinterpret_cast<const ObJsonTableSpec*>(&spec_);
  const ObIArray<ObJtColInfo*>& plain_def = spec_ptr->cols_def_;

  for (size_t i = 0; OB_SUCC(ret) && i < plain_def.count(); ++i) {
    const ObJtColInfo& info = *plain_def.at(i);
    JtColTreeNode* col_def = static_cast<JtColTreeNode*>(allocator_->alloc(sizeof(JtColTreeNode)));
    if (OB_ISNULL(col_def)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to allocate col node", K(ret));
    } else {
      col_def = new (col_def) JtColTreeNode(info);
    }

    if (OB_FAIL(ret)) {
    } else {
      JtColTreeNode* parent = nullptr;
      if (info.parent_id_ < 0) {
        root = col_def;
      } else if (OB_FAIL(find_column(info.parent_id_, root, parent))) {
        LOG_WARN("fail to find col node parent", K(ret), K(info.parent_id_));
      } else if (info.col_type_ == static_cast<int32_t>(NESTED_COL_TYPE)) {
        if (OB_FAIL(parent->nested_cols_.push_back(col_def))) {
          LOG_WARN("fail to store col node", K(ret), K(parent->nested_cols_.count()));
        }
      } else if (OB_FAIL(parent->regular_cols_.push_back(col_def))) {
        LOG_WARN("fail to store col node", K(ret), K(parent->nested_cols_.count()));
      }
    }
  }

  return ret;
}

int ObJsonTableOp::inner_open()
{
  INIT_SUCC(ret);
  if (OB_FAIL(init())) {
    LOG_WARN("failed to init.", K(ret));
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to open iter, value expr is null.", K(ret));
  } else if (OB_FAIL(jt_root_->open())) {
    LOG_WARN("failed to open jt column node.", K(ret));
  } else {
    is_evaled_ = false;
  }

  return ret;
}

int ObJsonTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
    LOG_WARN("failed to inner rescan", K(ret));
  } else if (OB_FAIL(inner_open())) {
    LOG_WARN("failed to inner open", K(ret));
  } else {
    jt_ctx_.row_alloc_.reuse();
  }
  return ret;
}

int ObJsonTableOp::switch_iterator()
{
  INIT_SUCC(ret);
  return OB_ITER_END;
}


int ObJsonTableOp::init()
{
  INIT_SUCC(ret);
  if (!is_inited_) {
    const ObJsonTableSpec* spec_ptr = reinterpret_cast<const ObJsonTableSpec*>(&spec_);
    if (OB_FAIL(generate_table_exec_tree())) {
      LOG_WARN("fail to init json table op, as generate exec tree occur error.", K(ret));
    } else {
      const sql::ObSQLSessionInfo *session = get_exec_ctx().get_my_session();
      uint64_t tenant_id = session ? common::OB_SERVER_TENANT_ID : session->get_effective_tenant_id();

      is_inited_ = true;
      jt_ctx_.spec_ptr_ = const_cast<ObJsonTableSpec*>(spec_ptr);
      jt_ctx_.eval_ctx_ = &eval_ctx_;
      jt_ctx_.exec_ctx_ = &get_exec_ctx();
      jt_ctx_.row_alloc_.set_tenant_id(tenant_id);
      jt_ctx_.op_exec_alloc_ = allocator_;
      jt_ctx_.is_evaled_ = false;
      jt_ctx_.is_charset_converted_ = false;
      jt_ctx_.res_obj_ = nullptr;

      js_null_ = static_cast<ObJsonNull*>(allocator_->alloc(sizeof(ObJsonNull)));
      js_arr_ = static_cast<ObJsonArray*>(allocator_->alloc(sizeof(ObJsonArray)));
      js_obj_ = static_cast<ObJsonObject*>(allocator_->alloc(sizeof(ObJsonObject)));

      if (OB_ISNULL(js_null_) || OB_ISNULL(js_arr_) || OB_ISNULL(js_obj_)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to allocate js node", KP(js_null_), KP(js_arr_), KP(js_obj_), K(ret));
      } else {
        js_null_ = new (js_null_) ObJsonNull();
        js_arr_ = new (js_arr_) ObJsonArray(allocator_);
        js_obj_ = new (js_obj_) ObJsonObject(allocator_);
      }
    }
  }

  jt_ctx_.is_cover_error_ = false;
  jt_ctx_.error_code_ = 0;
  jt_ctx_.is_need_end_ = 0;

  return ret;
}

int ObJsonTableOp::inner_close()
{
  INIT_SUCC(ret);
  if (OB_NOT_NULL(jt_root_)) {
    jt_root_->destroy();
  }

  if (OB_NOT_NULL(def_root_)) {
    def_root_->destroy();
  }
  return ret;
}

void ObJsonTableOp::reset_columns()
{
  for (size_t i = 0; i < col_count_; ++i) {
     ObExpr* col_expr = jt_ctx_.spec_ptr_->column_exprs_.at(i);
     col_expr->locate_datum_for_write(*jt_ctx_.eval_ctx_).reset();
     col_expr->locate_datum_for_write(*jt_ctx_.eval_ctx_).set_null();
     col_expr->get_eval_info(*jt_ctx_.eval_ctx_).evaluated_ = true;
  }
}

void ObJsonTableOp::destroy()
{
  ObOperator::destroy();
}

int ObJsonTableOp::inner_get_next_row()
{
  INIT_SUCC(ret);
  bool is_root_null = false;
  if (OB_FAIL(init())) {
    LOG_WARN("failed to init.", K(ret));
  } else if (is_evaled_) {
    clear_evaluated_flag();
    reset_columns();
    if (OB_FAIL(jt_root_->get_next_row(in_, &jt_ctx_, is_root_null))) {
      if (ret != OB_ITER_END) {
        LOG_WARN("failed to open get next row.", K(ret));
      }
    }
  } else {
    clear_evaluated_flag();
    common::ObObjMeta& doc_obj_datum = MY_SPEC.value_expr_->obj_meta_;
    ObDatumMeta& doc_datum = MY_SPEC.value_expr_->datum_meta_;
    ObObjType doc_type = doc_datum.type_;
    ObCollationType doc_cs_type = doc_datum.cs_type_;
    ObString j_str;
    bool is_null = false;

    if (doc_type == ObNullType) {
      ret = OB_ITER_END;
    } else if (doc_type == ObNCharType || !(doc_type == ObJsonType || ob_is_string_type(doc_type))) {
      ret = OB_ERR_INPUT_JSON_TABLE;
      LOG_WARN("fail to get json base", K(ret), K(doc_type));
    } else {
      reset_columns();
      if (OB_FAIL(ObJsonExprHelper::get_json_or_str_data(MY_SPEC.value_expr_,eval_ctx_,
                                                         jt_ctx_.row_alloc_, j_str, is_null))) {
        LOG_WARN("get real data failed", K(ret));
      } else if (is_null) {
        ret = OB_ITER_END;
      } else if ((ob_is_string_type(doc_type) || doc_type == ObLobType)
                  && (doc_cs_type != CS_TYPE_BINARY)
                  && (ObCharset::charset_type_by_coll(doc_cs_type) != CHARSET_UTF8MB4)) {
        // need convert to utf8 first, we are using GenericInsituStringStream<UTF8<> >
        char *buf = nullptr;
        const int64_t factor = 2;
        int64_t buf_len = j_str.length() * factor;
        uint32_t result_len = 0;

        if (OB_ISNULL(buf = static_cast<char*>(jt_ctx_.row_alloc_.alloc(buf_len)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("alloc memory failed", K(ret));
        } else if (OB_FAIL(ObCharset::charset_convert(doc_cs_type, j_str.ptr(),
                                                      j_str.length(), CS_TYPE_UTF8MB4_BIN, buf,
                                                      buf_len, result_len))) {
          LOG_WARN("charset convert failed", K(ret));
        } else {
          jt_ctx_.is_charset_converted_ = true;
          j_str.assign_ptr(buf, result_len);
        }
      }

      ObJsonInType j_in_type = ObJsonExprHelper::get_json_internal_type(doc_type);
      ObJsonInType expect_type = ObJsonInType::JSON_TREE;
      uint32_t parse_flag = ObJsonParser::JSN_RELAXED_FLAG;

      // json type input, or has is json check
      bool is_ensure_json = (doc_type == ObJsonType);

      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&jt_ctx_.row_alloc_, j_str, j_in_type, expect_type, in_, parse_flag))
                 || (in_->json_type() != ObJsonNodeType::J_ARRAY && in_->json_type() != ObJsonNodeType::J_OBJECT)) {
        if (OB_FAIL(ret) || (!is_ensure_json)) {
          in_= nullptr;
          ret = OB_ERR_JSON_SYNTAX_ERROR;
          SET_COVER_ERROR(&jt_ctx_, ret);
          if (jt_root_->col_info_.on_error_ != JSN_TABLE_ERROR) {
            jt_ctx_.is_need_end_ = 1;
            ret = OB_SUCCESS;
          }
        } else {
          ret = OB_SUCCESS;
        }
      }

      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(jt_root_->get_next_row(in_, &jt_ctx_, is_root_null))) {
        if (ret != OB_ITER_END) {
          LOG_WARN("fail to get next row", K(ret), KP(in_));
        }
      } else {
        is_evaled_ = true;
      }
    }
  }

  return ret;
}


} // end namespace sql
} // end namespace oceanbase