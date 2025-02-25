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

#define USING_LOG_PREFIX  SQL_ENG

#include "ob_expr_convert.h"

#include "lib/charset/ob_charset.h"
#include "sql/engine/expr/ob_expr_cast.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/expr/ob_expr_result_type_util.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;
namespace oceanbase
{
namespace sql
{

ObExprConvert::ObExprConvert(ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, T_FUN_SYS_CONVERT, N_CONVERT, 2, NOT_ROW_DIMENSION)
{
}

ObExprConvert::~ObExprConvert()
{
}

int ObExprConvert::calc_result_type2(ObExprResType &type,
                                     ObExprResType &type1,
                                     ObExprResType &type2,
                                     ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);

  int ret = OB_SUCCESS;
  type.set_type(ObVarcharType); // Only convert (xx using collation) will reach here now. It must be a varchar result.
  type.set_scale(type1.get_scale()); 
  type.set_precision(type1.get_precision());
  if (ob_is_string_type(type.get_type())) {
    type.set_length(type1.get_length());
  }
  const ObObj &dest_collation = type2.get_param();
  TYPE_CHECK(dest_collation, ObVarcharType);
  if (OB_SUCC(ret)) {
    ObString cs_name = dest_collation.get_string();
    ObCharsetType charset_type = CHARSET_INVALID;
    if (CHARSET_INVALID == (charset_type = ObCharset::charset_type(cs_name.trim()))) {
      ret = OB_ERR_UNKNOWN_CHARSET;
      LOG_WARN("unknown charset", K(ret), K(cs_name));
    } else {
      type.set_collation_level(CS_LEVEL_EXPLICIT);
      type.set_collation_type(ObCharset::get_default_collation(charset_type));
      //set calc type
      //only set type2 here.
      type2.set_calc_type(ObVarcharType);
      // cast表达式会对convert表达式的第一个子节点cast为type1,计算时convert的结果就是第一个
      // 子节点的结果
      type1.set_calc_meta(type.get_obj_meta());
      type1.set_calc_collation_type(type.get_collation_type());
      type1.set_calc_collation_level(type.get_collation_level());
      type_ctx.set_cast_mode(type_ctx.get_cast_mode() | CM_CHARSET_CONVERT_IGNORE_ERR);
      LOG_DEBUG("in calc result type", K(ret), K(type1), K(type2), K(type));
    }
  }

  return ret;
}

int calc_convert_expr(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res_datum)
{
  int ret = OB_SUCCESS;
  ObDatum *child_res = NULL;
  if (OB_FAIL(expr.args_[0]->eval(ctx, child_res))) {
    LOG_WARN("eval arg 0 failed", K(ret));
  } else {
    res_datum.set_datum(*child_res);
  }
  return ret;
}

int ObExprConvert::cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  rt_expr.eval_func_ = calc_convert_expr;
  return ret;
}

ObExprConvertOracle::ObExprConvertOracle(ObIAllocator &alloc)
    : ObStringExprOperator(alloc, T_FUN_SYS_CONVERT, N_CONVERT, TWO_OR_THREE)
{
}

ObExprConvertOracle::~ObExprConvertOracle()
{
}

int ObExprConvertOracle::calc_result_typeN(ObExprResType &type,
                                           ObExprResType *types_array,
                                           int64_t param_num,
                                           ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;
  if (!(param_num >= 2 && param_num <= 3)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("param num should be 2 or 3", K(ret));
  }

  //result meta deduce
  if (OB_SUCC(ret)) {
    ObLength length;
    auto str_params = make_const_carray(&types_array[0]);
    OZ (aggregate_string_type_and_charset_oracle(*type_ctx.get_session(),
                                                 str_params,
                                                 type,
                                                 PREFER_VAR_LEN_CHAR));
    OZ (deduce_string_param_calc_type_and_charset(*type_ctx.get_session(),
                                                  type,
                                                  str_params));
    OX (length = types_array[0].get_calc_length());
    OX (type.set_length(length * ObCharset::MAX_MB_LEN));
  }

  //param calc type deduce
  if (OB_SUCC(ret)) {
    types_array[1].set_calc_type_default_varchar();
    if (3 == param_num) {
      types_array[2].set_calc_type_default_varchar();
    }
  }


  return ret;
}

int ObExprConvertOracle::calc_convert_oracle_expr(const ObExpr &expr,
                                                  ObEvalCtx &ctx,
                                                  ObDatum &res_datum)
{
  int ret = OB_SUCCESS;
  ObDatum *src_param = NULL;
  ObCollationType src_cs_type = CS_TYPE_INVALID;
  ObCollationType dst_cs_type = CS_TYPE_INVALID;
  ObValueChecker<int> charset_checker(CHARSET_INVALID + 1, CHARSET_MAX - 1,
                                      OB_ERR_UNSUPPORTED_CHARACTER_SET);

  //param1
  if (OB_FAIL(expr.args_[0]->eval(ctx, src_param))) {
    LOG_WARN("eval arg failed", K(ret));
  }

  //param2
  if (OB_SUCC(ret)) {
    ObString dst_character_set;
    ObDatum *dst_cs_type_param = NULL;

    if (OB_FAIL(expr.args_[1]->eval(ctx, dst_cs_type_param))) {
      LOG_WARN("eval arg failed", K(ret));
    } else {
      dst_character_set = dst_cs_type_param->get_string();
      dst_cs_type = ObCharset::get_default_collation_by_mode(
            ObCharset::charset_type_by_name_oracle(dst_character_set), lib::is_oracle_mode());
      if (OB_FAIL(charset_checker.validate(ObCharset::charset_type_by_coll(dst_cs_type)))) {
        LOG_WARN("invalid charset value", K(ret), K(dst_character_set));
      }
    }
  }

  //param3
  if (OB_SUCC(ret)) {
    if (3 == expr.arg_cnt_) {
      ObString src_character_set;
      ObDatum *src_cs_type_param = NULL;

      if (OB_FAIL(expr.args_[2]->eval(ctx, src_cs_type_param))) {
        LOG_WARN("eval arg failed", K(ret));
      } else {
        src_character_set = src_cs_type_param->get_string();
        src_cs_type = ObCharset::get_default_collation_by_mode(
              ObCharset::charset_type_by_name_oracle(src_character_set), lib::is_oracle_mode());
        if (OB_FAIL(charset_checker.validate(ObCharset::charset_type_by_coll(src_cs_type)))) {
          LOG_WARN("invalid charset value", K(ret), K(src_character_set));
        }
      }
    } else {
      src_cs_type = expr.args_[0]->datum_meta_.cs_type_;
    }
  }

  //convert result
  if (OB_SUCC(ret)) {
    if (src_param->is_null()) {
      res_datum.set_null();
    } else {
      ObString src = src_param->get_string();
      ObString dst;
      char *res_buf = NULL;
      const int64_t res_buf_len = src.length() * ObCharset::MAX_MB_LEN;
      if (OB_ISNULL(res_buf = expr.get_str_res_mem(ctx, res_buf_len))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("allocate memory failed", K(ret), K(res_buf_len));
      } else if (!ob_is_text_tc(expr.args_[0]->datum_meta_.type_)) {
        ObDataBuffer data_buf(res_buf, res_buf_len);
        if (OB_FAIL(ObCharset::charset_convert(data_buf, src, src_cs_type, dst_cs_type, dst,
                                               ObCharset::REPLACE_UNKNOWN_CHARACTER))) {
          LOG_WARN("fail to convert input string", K(src), K(src_cs_type), K(dst_cs_type),
                   KPHEX(src.ptr(), src.length()), K(res_buf_len));
        } else {
          if (dst.empty()) {
            res_datum.set_null();
          } else {
            res_datum.set_string(dst);
          }
        }
      } else { // text tc
        ObTextStringIter src_iter(expr.args_[0]->datum_meta_.type_,
                                  src_cs_type,
                                  src_param->get_string(),
                                  expr.args_[0]->obj_meta_.has_lob_header());
        ObEvalCtx::TempAllocGuard alloc_guard(ctx);
        ObIAllocator &calc_alloc = alloc_guard.get_allocator();
        ObTextStringDatumResult output_result(expr.datum_meta_.type_, &expr, &ctx, &res_datum);
        int64_t src_byte_len = 0;
        int64_t buf_size = 0;
        if (ob_is_string_tc(expr.datum_meta_.type_)
            && (src.length() == 0
               || ObCharset::charset_type_by_coll(src_cs_type) == ObCharset::charset_type_by_coll(dst_cs_type)
               || ObCharset::charset_type_by_coll(dst_cs_type) == CHARSET_BINARY)) {
          dst = src; // no need convert
        } else if (OB_FAIL(src_iter.init(0, NULL, &calc_alloc))) {
          LOG_WARN("init src_iter failed ", K(ret), K(src_iter));
        } else if (OB_FAIL(src_iter.get_byte_len(src_byte_len))) {
          LOG_WARN("get input byte len failed");
        } else if (OB_FAIL(output_result.init(src_byte_len * ObCharset::MAX_MB_LEN))) {
          LOG_WARN("init stringtext result failed");
        } else if (src_byte_len == 0) {
          output_result.set_result();
        } else if (OB_FAIL(output_result.get_reserved_buffer(res_buf, buf_size))) {
          LOG_WARN("stringtext result reserve buffer failed");
        } else {
          ObTextStringIterState state;
          ObString src_block_data;
          while (OB_SUCC(ret)
                && buf_size > 0
                && (state = src_iter.get_next_block(src_block_data)) == TEXTSTRING_ITER_NEXT) {
            ObDataBuffer data_buf(res_buf, buf_size);
            if (OB_FAIL(ObCharset::charset_convert(data_buf, src_block_data, src_cs_type, dst_cs_type, dst,
                                                  (ObCharset::REPLACE_UNKNOWN_CHARACTER
                                                    | ObCharset::COPY_STRING_ON_SAME_CHARSET)))) {
              LOG_WARN("fail to convert input string", K(src_block_data), K(src_cs_type), K(dst_cs_type),
                      KPHEX(src_block_data.ptr(), src_block_data.length()), K(buf_size));
            } else if (OB_FAIL(output_result.lseek(dst.length(), 0))) {
              LOG_WARN("result lseek failed", K(ret));
            } else {
              res_buf += dst.length();
              buf_size -= dst.length();
            }
          }
          if (OB_SUCC(ret)) {
            output_result.get_result_buffer(dst);
          }
        }
        if (OB_FAIL(ret)) {
        } else if (dst.empty()) {
          // initcap is only for oracle mode. set res be null when string length is 0.
          res_datum.set_null();
        } else {
          res_datum.set_string(dst);
        }
      }
    }
  }

  return ret;
}

int ObExprConvertOracle::cg_expr(ObExprCGCtx &op_cg_ctx,
                                 const ObRawExpr &raw_expr,
                                 ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  UNUSED(op_cg_ctx);
  UNUSED(raw_expr);
  rt_expr.eval_func_ = calc_convert_oracle_expr;
  return ret;
}

} //namespace sql
} //namespace oceanbase
