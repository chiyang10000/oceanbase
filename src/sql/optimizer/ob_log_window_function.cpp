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

#define USING_LOG_PREFIX SQL_OPT
#include "ob_log_window_function.h"
#include "ob_opt_est_cost.h"
#include "sql/optimizer/ob_join_order.h"
#include "common/ob_smart_call.h"

#define PRINT_BOUND(bound_name, bound)                 \
  if (OB_SUCC(ret)) {                                 \
    if (OB_FAIL(BUF_PRINTF(#bound_name"("))) {        \
      LOG_WARN("BUF_PRINTF fails", K(ret));           \
    } else {                                          \
      bool print_dir = false;                         \
      if (BOUND_UNBOUNDED == bound.type_) {           \
        print_dir = true;                             \
        if (OB_FAIL(BUF_PRINTF("UNBOUNDED"))) {       \
          LOG_WARN("BUF_PRINTF fails", K(ret));       \
        }                                             \
      } else if (BOUND_CURRENT_ROW == bound.type_) {  \
        if (OB_FAIL(BUF_PRINTF("CURRENT ROW"))) {     \
          LOG_WARN("BUF_PRINTF fails", K(ret));       \
        }                                             \
      } else if (BOUND_INTERVAL == bound.type_) {     \
        print_dir = true;                             \
        if (OB_FAIL(bound.interval_expr_->get_name(buf, buf_len, pos, type))) { \
          LOG_WARN("print expr name failed", K(ret));  \
        } else if (!bound.is_nmb_literal_) {           \
          int64_t date_unit_type = DATE_UNIT_MAX;      \
          ObConstRawExpr *con_expr = static_cast<ObConstRawExpr*>(bound.date_unit_expr_);  \
          if (OB_ISNULL(con_expr)) {                   \
            ret = OB_ERR_UNEXPECTED;                   \
            LOG_WARN("con_expr should not be NULL", K(ret));  \
          } else {                                     \
            con_expr->get_value().get_int(date_unit_type);  \
            const static char *date_unit =             \
            ob_date_unit_type_str(static_cast<ObDateUnitType>(date_unit_type)); \
            if (OB_FAIL(BUF_PRINTF(" %s", date_unit))) { \
              LOG_WARN("BUF_PRINTF fails", K(ret));      \
            }                                          \
          }                                            \
        }                                              \
      }                                                \
      if (OB_SUCC(ret) && print_dir) {                 \
        if (bound.is_preceding_) {                     \
          if (OB_FAIL(BUF_PRINTF(" PRECEDING"))) {     \
            LOG_WARN("BUF_PRINTF fails", K(ret));      \
          }                                            \
        } else {                                       \
          if (OB_FAIL(BUF_PRINTF(" FOLLOWING"))) {     \
            LOG_WARN("BUF_PRINTF fails", K(ret));      \
          }                                            \
        }                                              \
      }                                                \
      if (OB_SUCC(ret)) {                              \
        if (OB_FAIL(BUF_PRINTF(")"))) {                \
          LOG_WARN("BUF_PRINTF fails", K(ret));        \
        }                                              \
      }                                                \
    }                                                  \
  }

using namespace oceanbase::sql;
using namespace oceanbase::common;
using namespace oceanbase::sql::log_op_def;

int ObLogWindowFunction::get_op_exprs(ObIArray<ObRawExpr*> &all_exprs)
{
  int ret = OB_SUCCESS;
  FOREACH_CNT_X(it, rd_sort_keys_, OB_SUCC(ret)) {
    if (OB_FAIL(all_exprs.push_back(it->expr_))) {
      LOG_WARN("array push back failed", K(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (NULL != wf_aggr_status_expr_) {
    if (OB_FAIL(all_exprs.push_back(wf_aggr_status_expr_))) {
      LOG_WARN("array push back failed", K(ret));
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(append(all_exprs, win_exprs_))) {
    LOG_WARN("failed to append exprs", K(ret));
  } else if (OB_FAIL(ObLogicalOperator::get_op_exprs(all_exprs))) {
    LOG_WARN("failed to get op exprs", K(ret));
  } else { /*do nothing*/ }
  return ret;
}

int ObLogWindowFunction::get_explain_name_internal(char *buf,
                                                   const int64_t buf_len,
                                                   int64_t &pos)
{
  int ret = BUF_PRINTF("%s", get_name());
  if (OB_FAIL(ret)) {
  } else if (WindowFunctionRoleType::CONSOLIDATOR == role_type_) {
    ret = BUF_PRINTF(" CONSOLIDATOR");
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("BUF_PRINTF fails", K(ret));
  }
  return ret;
}

int ObLogWindowFunction::get_plan_item_info(PlanText &plan_text,
                                            ObSqlPlanItem &plan_item)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObLogicalOperator::get_plan_item_info(plan_text, plan_item))) {
    LOG_WARN("failed to get plan item info", K(ret));
  } else {
    BEGIN_BUF_PRINT;
    if (OB_FAIL(get_explain_name_internal(buf, buf_len, pos))) {
      LOG_WARN("failed to get explain name", K(ret));
    }
    END_BUF_PRINT(plan_item.operation_, plan_item.operation_len_);
  }
  if (OB_SUCC(ret)) {
    BEGIN_BUF_PRINT;
    for (int64_t i = 0; i < win_exprs_.count() && OB_SUCC(ret); ++i) {
      ObWinFunRawExpr *win_expr = win_exprs_.at(i);
      if (i != 0 && OB_FAIL(BUF_PRINTF("\n      "))) {
        LOG_WARN("BUF_PRINTF fails", K(ret));
      } else {
        EXPLAIN_PRINT_EXPR(win_expr, type);
      }
      // partition by
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(BUF_PRINTF(", "))) {
        LOG_WARN("BUF_PRINTF fails", K(ret));
      } else {
        const ObIArray<ObRawExpr *> &partition_by = win_expr->get_partition_exprs();
        EXPLAIN_PRINT_EXPRS(partition_by, type);
      }
      // order by
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(BUF_PRINTF(", "))) {
        LOG_WARN("BUF_PRINTF fails", K(ret));
      } else {
        const ObIArray<OrderItem> &order_by = win_expr->get_order_items();
        EXPLAIN_PRINT_SORT_ITEMS(order_by, type);
      }
      // win_type
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(BUF_PRINTF(", "))) {
        LOG_WARN("BUF_PRINTF fails", K(ret));
      } else {
        const char *win_type_str = "MAX";
        if (WINDOW_ROWS == win_expr->get_window_type()) {
          win_type_str = "window_type(ROWS), ";
        } else if (WINDOW_RANGE == win_expr->get_window_type()) {
          win_type_str = "window_type(RANGE), ";
        }
        if (OB_FAIL(BUF_PRINTF("%s", win_type_str))) {
          LOG_WARN("BUF_PRINTF fails", K(ret));
        }
        PRINT_BOUND(upper, win_expr->get_upper());
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(BUF_PRINTF(", "))) {
          LOG_WARN("BUF_PRINTF fails", K(ret));
        }
        PRINT_BOUND(lower, win_expr->get_lower());
      }
    }
    END_BUF_PRINT(plan_item.special_predicates_,
                  plan_item.special_predicates_len_);
  }
  return ret;
}

int ObLogWindowFunction::est_width()
{
  int ret = OB_SUCCESS;
  double width = 0.0;
  ObLogPlan *plan = NULL;
  ObSEArray<ObRawExpr*, 16> output_exprs;
  if (OB_ISNULL(plan = get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid input", K(ret));
  } else if (OB_FAIL(get_winfunc_output_exprs(output_exprs))) {
    LOG_WARN("failed to compute winfunc output exprs", K(ret));
  } else if (OB_FAIL(ObOptEstCost::estimate_width_for_exprs(get_plan()->get_basic_table_metas(),
                                                            get_plan()->get_selectivity_ctx(),
                                                            output_exprs,
                                                            width))) {
    LOG_WARN("failed to estimate width for output winfunc exprs", K(ret));
  } else {
    set_width(width);
    LOG_TRACE("est_width for winfunc", K(output_exprs), K(width));
  }
  return ret;
}

int ObLogWindowFunction::get_winfunc_output_exprs(ObIArray<ObRawExpr *> &output_exprs)
{
  int ret = OB_SUCCESS;
  ObLogPlan *plan = NULL;
  ObSEArray<ObRawExpr*, 16> candi_exprs;
  ObSEArray<ObRawExpr*, 16> extracted_col_aggr_winfunc_exprs;
  if (OB_ISNULL(plan = get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid input", K(ret));
  } else if (OB_FAIL(append_array_no_dup(candi_exprs, plan->get_select_item_exprs_for_width_est()))) {
    LOG_WARN("failed to add into output exprs", K(ret));
  } else if (OB_FAIL(append_array_no_dup(candi_exprs, plan->get_orderby_exprs_for_width_est()))) {
    LOG_WARN("failed to add into output exprs", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::extract_col_aggr_winfunc_exprs(candi_exprs,
                                                                    extracted_col_aggr_winfunc_exprs))) {
  } else if (OB_FAIL(append_array_no_dup(output_exprs, extracted_col_aggr_winfunc_exprs))) {
    LOG_WARN("failed to add into output exprs", K(ret));
  } else {/*do nothing*/}
  return ret;
}

uint64_t ObLogWindowFunction::hash(uint64_t seed) const
{
  seed = do_hash(role_type_, seed);
  seed = ObLogicalOperator::hash(seed);
  return seed;
}

int ObLogWindowFunction::est_cost()
{
  int ret = OB_SUCCESS;
  int64_t parallel = 0;
  ObLogicalOperator *first_child = NULL;
  if (OB_ISNULL(get_plan()) ||
      OB_ISNULL(first_child = get_child(ObLogicalOperator::first_child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("first child is null", K(ret), K(first_child));
  } else if (OB_UNLIKELY((parallel = get_parallel()) < 1)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected parallel degree", K(parallel), K(ret)); 
  } else if (OB_FAIL(ObOptEstCost::cost_window_function(first_child->get_card() / parallel,
                                                        first_child->get_width(),
                                                        win_exprs_.count(),
                                                        op_cost_,
                                                        get_plan()->get_optimizer_context().get_cost_model_type()))) {
    LOG_WARN("calculate cost of window function failed", K(ret));
  } else {
    set_card(first_child->get_card());
    set_cost(first_child->get_cost() + op_cost_);
  }
  return ret;
}

int ObLogWindowFunction::re_est_cost(EstimateCostInfo &param, double &card, double &cost)
{
  int ret = OB_SUCCESS;
  int64_t parallel = 0;
  ObLogicalOperator *child = NULL;
  if (OB_ISNULL(get_plan()) ||
      OB_ISNULL(child = get_child(ObLogicalOperator::first_child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(child), K(ret));
  } else if (OB_UNLIKELY((parallel = get_parallel()) < 1)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected parallel degree", K(parallel), K(ret));
  } else {
    double child_card = child->get_card();
    double child_cost = child->get_cost();
    double op_cost = 0.0;
    ObOptimizerContext &opt_ctx = get_plan()->get_optimizer_context();
    if (is_block_op()) {
      param.need_row_count_ = -1; //reset need row count
    }
    if (OB_FAIL(SMART_CALL(child->re_est_cost(param, child_card, child_cost)))) {
      LOG_WARN("failed to re est exchange cost", K(ret));
    } else if (OB_FAIL(ObOptEstCost::cost_window_function(child_card / parallel,
                                                          child->get_width(),
                                                          win_exprs_.count(),
                                                          op_cost,
                                                          opt_ctx.get_cost_model_type()))) {
      LOG_WARN("calculate cost of window function failed", K(ret));
    } else {
      cost = child_cost + op_cost;
      card = child_card;
      if (param.override_) {
        set_op_cost(op_cost);
        set_cost(cost);
        set_card(card);
      }
    }
  }
  return ret;
}

int ObLogWindowFunction::allocate_granule_pre(AllocGIContext &ctx)
{
  return pw_allocate_granule_pre(ctx);
}

int ObLogWindowFunction::allocate_granule_post(AllocGIContext &ctx)
{
  return pw_allocate_granule_post(ctx);
}

int ObLogWindowFunction::get_win_partition_intersect_exprs(ObIArray<ObWinFunRawExpr *> &win_exprs,
                                                           ObIArray<ObRawExpr *> &win_part_exprs)
{
  int ret = OB_SUCCESS;
  if (win_exprs.count() > 0) {
    if (OB_ISNULL(win_exprs.at(0))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else if (OB_FAIL(append(win_part_exprs, win_exprs.at(0)->get_partition_exprs()))) {
      LOG_WARN("failed to append exprs", K(ret));
    } else {
      for (int64_t i = 1; OB_SUCC(ret) && i < win_exprs.count(); ++i) {
        ObWinFunRawExpr *win_expr = win_exprs.at(i);
        if (OB_ISNULL(win_expr)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null", K(ret));
        } else if (OB_FAIL(ObOptimizerUtil::intersect_exprs(win_part_exprs,
                                                            win_expr->get_partition_exprs(),
                                                            win_part_exprs))) {
          LOG_WARN("failed to intersect exprs", K(ret));
        } else { /*do nothing*/ }
      }
    }
  }
  return ret;
}

int ObLogWindowFunction::compute_op_ordering()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObLogicalOperator::compute_op_ordering())) {
    LOG_WARN("failed to compute op ordering", K(ret));
  } else if (!single_part_parallel_) {
    is_local_order_ = (range_dist_parallel_ || is_fully_paratition_wise())
                       && !get_op_ordering().empty();
  }
  return ret;
}

int ObLogWindowFunction::compute_sharding_info()
{
  int ret = OB_SUCCESS;
  if (is_single_part_parallel()) {
    if (OB_ISNULL(get_plan())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret));
    } else {
      strong_sharding_ = get_plan()->get_optimizer_context().get_distributed_sharding();
    }
  } else if (OB_FAIL(ObLogicalOperator::compute_sharding_info())) {
    LOG_WARN("failed to compute sharding info", K(ret));
  }
  return ret;
}

bool ObLogWindowFunction::is_block_op() const
{
  bool is_block_op = true;
  // 对于window function算子, 在没有partition by以及完整窗口情况下,
  // 所有数据作为一个窗口, 认为是block算子
  // 在其他情况下, 认为是非block算子
  ObWinFunRawExpr *win_expr = NULL;
  for (int64_t i = 0; i < win_exprs_.count(); ++i) {
    if (OB_ISNULL(win_expr = win_exprs_.at(i))) {
      LOG_ERROR("win expr is null");
    } else if (win_expr->get_partition_exprs().count() > 0 ||
        win_expr->get_upper().type_ != BoundType::BOUND_UNBOUNDED ||
        win_expr->get_lower().type_ != BoundType::BOUND_UNBOUNDED ) {
      is_block_op = false;
      break;
    }
  }
  return is_block_op;
}

int ObLogWindowFunction::print_outline_data(PlanText &plan_text)
{
  int ret = OB_SUCCESS;
  const ObDMLStmt *stmt = NULL;
  ObString qb_name;
  if (OB_ISNULL(get_plan()) || OB_ISNULL(stmt = get_plan()->get_stmt())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected NULL", K(ret), K(get_plan()), K(stmt));
  } else {
    if (!dist_hint_.empty()) {
      ObWindowDistHint win_dist;
      if (OB_FAIL(stmt->get_qb_name(qb_name))) {
        LOG_WARN("get qb name failed", K(ret));
      } else if (OB_FAIL(win_dist.get_algos().assign(dist_hint_))) {
        LOG_WARN("array assign failed", K(ret));
      } else {
        win_dist.set_qb_name(qb_name);
        if (OB_FAIL(win_dist.print_hint(plan_text))) {
          LOG_WARN("print hint failed", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObLogWindowFunction::print_used_hint(PlanText &plan_text)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected NULL", K(ret), K(get_plan()));
  } else {
    auto win_dist = get_plan()->get_log_plan_hint().get_window_dist();
    if (NULL != win_dist && !win_dist->get_algos().empty()) {
      if (is_array_equal(dist_hint_, win_dist->get_algos())) {
        if (OB_FAIL(win_dist->print_hint(plan_text))) {
          LOG_WARN("print hint failed", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObLogWindowFunction::inner_replace_op_exprs(
    const common::ObIArray<std::pair<ObRawExpr *, ObRawExpr*>> &to_replace_exprs)
{
  int ret = OB_SUCCESS;
  FOREACH_X(key, rd_sort_keys_, OB_SUCC(ret)) {
    if (OB_FAIL(replace_expr_action(to_replace_exprs, key->expr_))) {
      LOG_WARN("replace expr failed", K(ret));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < win_exprs_.count(); ++i) {
    ObRawExpr *win_expr = win_exprs_.at(i);
    ObWinFunRawExpr *new_expr = NULL;
    if (OB_ISNULL(win_expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("win expr is null", K(ret));
    } else if (OB_FAIL(replace_expr_action(to_replace_exprs, win_expr))) {
      LOG_WARN("replace expr failed", K(ret));
    } else if (win_expr == win_exprs_.at(i)) {
      // do nothing
    } else if (OB_ISNULL(new_expr = static_cast<ObWinFunRawExpr *>(win_expr))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("new win expr is null", K(ret));
    } else {
      win_exprs_.at(i) = new_expr;
    }
  }
  return ret;
}
