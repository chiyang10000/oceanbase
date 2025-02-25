/**
 * Copyright (c) 2022 OceanBase
 * OceanBase is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEANBASE_OBSERVER_OB_TABLE_MODIFY_EXECUTOR_H
#define OCEANBASE_OBSERVER_OB_TABLE_MODIFY_EXECUTOR_H
#include "ob_table_executor.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/engine/dml/ob_table_insert_op.h"

namespace oceanbase
{
namespace table
{
// todo@dazhi: 编译需要，后续修改 dml_rtctx 中的 modify op 引用为指针之后会移除
static sql::ObTableModifyOp& get_fake_modify_op()
{
  static common::ObArenaAllocator alloc;
  static sql::ObPhyOperatorType op_type;
  static sql::ObTableInsertSpec op_spec(alloc, op_type);
  static sql::ObExecContext exec_ctx(alloc);
  static sql::ObSQLSessionInfo session;
  exec_ctx.set_my_session(&session);
  static sql::ObTableInsertOpInput input(exec_ctx, op_spec); // ObDMLService::init_das_dml_rtdef 需要
  static sql::ObPhysicalPlan phy_plan; // ObDMLService::init_das_dml_rtdef 需要
  phy_plan.set_plan_type(OB_PHY_PLAN_LOCAL);
  op_spec.plan_ = &phy_plan;
  static sql::ObTableInsertOp ins_op(exec_ctx, op_spec, &input);
  return ins_op;
}

class ObTableApiModifySpec : public ObTableApiSpec
{
public:
  ObTableApiModifySpec(common::ObIAllocator &alloc, const ObTableExecutorType type)
      : ObTableApiSpec(alloc, type),
        expr_frame_info_(nullptr)
  {
  }
  virtual ~ObTableApiModifySpec()
  {
  }
  sql::ObExprFrameInfo *expr_frame_info_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObTableApiModifySpec);
};

class ObTableApiModifyExecutor : public ObTableApiExecutor
{
public:
  ObTableApiModifyExecutor(ObTableCtx &ctx)
      : ObTableApiExecutor(ctx),
        dml_rtctx_(eval_ctx_, exec_ctx_, get_fake_modify_op()),
        affected_rows_(0)
  {
  }
  virtual ~ObTableApiModifyExecutor()
  {
    destroy();
  }
public:
  virtual int open() override;
  virtual int close() override;
  virtual void destroy() override
  {
    dml_rtctx_.cleanup();
  }

  int init_das_ref();
  int submit_all_dml_task();
  int init_das_dml_rtdef(const sql::ObDASDMLBaseCtDef &das_ctdef,
                         sql::ObDASDMLBaseRtDef &das_rtdef,
                         const sql::ObDASTableLocMeta *loc_meta);
  int init_related_das_rtdef(const sql::DASDMLCtDefArray &das_ctdefs,
                             sql::DASDMLRtDefArray &das_rtdefs);
  int calc_tablet_loc(sql::ObDASTabletLoc *&tablet_loc);
  OB_INLINE int64_t get_affected_rows() const { return affected_rows_; }
  int get_affected_entity(ObITableEntity *&entity);
protected:
  int generate_ins_rtdef(const ObTableInsCtDef &ins_ctdef,
                         ObTableInsRtDef &ins_rtdef);
  int generate_del_rtdef(const ObTableDelCtDef &del_ctdef,
                         ObTableDelRtDef &del_rtdef);
  int generate_upd_rtdef(const ObTableUpdCtDef &upd_ctdef,
                         ObTableUpdRtDef &upd_rtdef);
  int insert_row_to_das(const ObTableInsCtDef &ins_ctdef,
                        ObTableInsRtDef &ins_rtdef);
  int delete_row_to_das(const ObTableDelCtDef &del_ctdef,
                        ObTableDelRtDef &del_rtdef);
protected:
  sql::ObDMLRtCtx dml_rtctx_;
  int64_t affected_rows_;
};

} // end namespace table
} // end namespace oceanbase

#endif /* OCEANBASE_OBSERVER_OB_TABLE_MODIFY_EXECUTOR_H */