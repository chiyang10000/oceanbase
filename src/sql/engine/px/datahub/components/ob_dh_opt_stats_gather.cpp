// Copyright 1999-2022 Alibaba Inc. All Rights Reserved.
// Author:
//   liuqifan.lqf@oceanbase.com
//

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/px/datahub/components/ob_dh_opt_stats_gather.h"
#include "sql/engine/px/datahub/ob_dh_msg_ctx.h"
#include "sql/engine/px/ob_dfo.h"
#include "sql/engine/px/ob_px_util.h"
#include "sql/engine/px/datahub/ob_dh_msg.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/opt_statistics/ob_optimizer_stats_gathering_op.h"

using namespace oceanbase::sql;
using namespace oceanbase::common;

OB_DEF_SERIALIZE(ObOptStatsGatherPieceMsg)
{
  int ret = OB_SUCCESS;
  ret = ObDatahubPieceMsg::serialize(buf, buf_len, pos);
  if (OB_SUCC(ret)) {
    OB_UNIS_ENCODE(table_stats_.count());
    for (int64_t i = 0; i < table_stats_.count(); i++) {
      OB_UNIS_ENCODE(*table_stats_.at(i));
    }
    OB_UNIS_ENCODE(column_stats_.count());
    for (int64_t i = 0; i < column_stats_.count(); i++) {
      OB_UNIS_ENCODE(column_stats_.at(i)->size());
      OB_UNIS_ENCODE(*column_stats_.at(i));
    }
    OB_UNIS_ENCODE(target_osg_id_);
  }
  return ret;
}

OB_DEF_DESERIALIZE(ObOptStatsGatherPieceMsg)
{
  int ret = OB_SUCCESS;
  ret = ObDatahubPieceMsg::deserialize(buf, data_len, pos);
  if (OB_SUCC(ret)) {
    int64_t size = 0;
    OB_UNIS_DECODE(size)
    for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
      void *tmp_buf = arena_.alloc(sizeof(ObOptTableStat));
      if (OB_ISNULL(tmp_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("allocate memory failed", K(ret));
      } else {
        ObOptTableStat *tmp_stat = new (tmp_buf) ObOptTableStat;
        if (OB_FAIL(tmp_stat->deserialize(buf, data_len, pos))) {
          LOG_WARN("deserialize datum store failed", K(ret), K(i));
        } else if (OB_FAIL(table_stats_.push_back(tmp_stat))) {
          LOG_WARN("push back datum store failed", K(ret), K(i));
        }
      }
    }
    size = 0;
    OB_UNIS_DECODE(size)
    for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
      int col_stat_size = 0;
      OB_UNIS_DECODE(col_stat_size);
      void *tmp_buf = arena_.alloc(col_stat_size);
      if (OB_ISNULL(tmp_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("allocate memory failed", K(ret));
      } else {
        ObOptColumnStat *tmp_stat = new (tmp_buf) ObOptColumnStat(arena_);
        if (OB_FAIL(tmp_stat->deserialize(buf, data_len, pos))) {
          LOG_WARN("deserialize datum store failed", K(ret), K(i));
        } else if (OB_FAIL(column_stats_.push_back(tmp_stat))) {
          LOG_WARN("push back datum store failed", K(ret), K(i));
        }
      }
    }
    OB_UNIS_DECODE(target_osg_id_);
  }

  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObOptStatsGatherPieceMsg)
{
  int64_t len = 0;
  len += ObDatahubPieceMsg::get_serialize_size();
  OB_UNIS_ADD_LEN(table_stats_.count());
  for (int64_t i = 0; i < table_stats_.count(); i++) {
    OB_UNIS_ADD_LEN(*table_stats_.at(i));
  }
  OB_UNIS_ADD_LEN(column_stats_.count());
  for (int64_t i = 0; i < column_stats_.count(); i++) {
    OB_UNIS_ADD_LEN(column_stats_.at(i)->size());
    OB_UNIS_ADD_LEN(*column_stats_.at(i));
  }
  OB_UNIS_ADD_LEN(target_osg_id_);
  return len;
}

OB_SERIALIZE_MEMBER((ObOptStatsGatherWholeMsg, ObDatahubWholeMsg), ready_state_);

int ObOptStatsGatherPieceMsgListener::on_message(
  ObOptStatsGatherPieceMsgCtx &piece_ctx,
  common::ObIArray<ObPxSqcMeta *> &sqcs,
  const ObOptStatsGatherPieceMsg &pkt)
{
  int ret = OB_SUCCESS;
  if (pkt.op_id_ != piece_ctx.op_id_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected piece msg", K(ret), K(pkt), K(piece_ctx));
  } else {
    ObOptimizerStatsGatheringOp *osg_op = NULL;
    piece_ctx.received_++;
    // get the merge osg, pass piece msg to osg.
    ObOperatorKit *kit = piece_ctx.osg_info_.op_kit_;
    if (OB_ISNULL(kit) || OB_ISNULL(kit->op_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get null op kit", K(ret), K(kit));
    } else if (OB_UNLIKELY(PHY_OPTIMIZER_STATS_GATHERING != kit->op_->get_spec().type_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected op", K(ret), K(kit->op_->get_spec().type_));
    } else {
      osg_op = static_cast<ObOptimizerStatsGatheringOp *>(kit->op_);
      if (OB_FAIL(osg_op->on_piece_msg(pkt))) {
        LOG_WARN("fail to call on piece msg", K(ret));
      }
    }
  }
  return ret;
}

int ObOptStatsGatherPieceMsgCtx::alloc_piece_msg_ctx(const ObOptStatsGatherPieceMsg &pkt,
                                                    ObPxCoordInfo &,
                                                    ObExecContext &ctx,
                                                    int64_t task_cnt,
                                                    ObPieceMsgCtx *&msg_ctx)
{
  int ret = OB_SUCCESS;
  void *buf = ctx.get_allocator().alloc(sizeof(ObOptStatsGatherPieceMsgCtx));
  if (OB_ISNULL(ctx.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("physical plan ctx is null", K(ret));
  } else if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    msg_ctx = new (buf) ObOptStatsGatherPieceMsgCtx(pkt.op_id_, task_cnt,
          ctx.get_physical_plan_ctx()->get_timeout_timestamp());
    if (OB_ISNULL(ctx.get_operator_kit(pkt.target_osg_id_))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get null operator kit", K(ret), K(pkt));
    } else {
      static_cast<ObOptStatsGatherPieceMsgCtx*>(msg_ctx)->osg_info_.op_kit_ = ctx.get_operator_kit(pkt.target_osg_id_);
    }
  }

  return ret;
}
