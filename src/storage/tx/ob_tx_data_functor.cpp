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

#include "storage/tx/ob_tx_data_functor.h"
#include "lib/rowid/ob_urowid.h"
#include "storage/tx/ob_committer_define.h"
#include "storage/ob_i_store.h"
#include "storage/tx/ob_trans_define.h"
#include "storage/tx/ob_trans_service.h"
#include "storage/tx/ob_trans_part_ctx.h"
#include "storage/memtable/ob_memtable_context.h"
#include "observer/ob_server_struct.h"
#include "logservice/leader_coordinator/ob_failure_detector.h"

namespace oceanbase
{

using namespace share;

namespace storage
{

// NB: When modifying the concurrency control code from the file, you need
// understand how to manage the order between read and write of the txn state if
// we do not put these behaviors into critical section.
//
// Let's list the scarecrow thought:
// 1. txn1 change the txn's prepare version with 5
// 2. txn2 start the txn with snapshot version as 7
// 3. txn2 find the state of the txn1 is INIT, so ignore it according to the lock_for_read
// 4. txn1 change the txn's state to PREPARE
// So we find the order between the modification is very important
//
// First, we implement all the concurrency control logic by first locating the txn state:
//    if (ObTxData::ABORT == tx_state_) {
//      // logic 1
//    } else if (ObTxData::RUNNING == tx_state_) {
//      // logic 2
//    } else {
//      // logic 3
//    }
// So we must change the state last.
//
// Then, all variables necessary for concurrency control is prepare version,
// commit version and undo status(the concurrency control of undo status is
// in ob_lock_wait_mgr.cpp, so we skip it in the file).
// For version, obviously we should change it before state
//     Prepare Logic                              Commit Logic
//        |                                          |
//        | 1. modify prepare version in tx state    | 1. modify commit version in tx data
//        |                                          | 2. modify commit state in tx state
//        | 2. modify prepare state in tx state      | 3. modify commit state in tx data
//        |                                          |
//       ...                                        ...
//
// If we follow the logic above, we can always ensure the correctness between read and write
//

int CheckSqlSequenceCanReadFunctor::operator() (const ObTxData &tx_data, ObTxCCCtx *tx_cc_ctx) {
  UNUSED(tx_cc_ctx);
  int ret = OB_SUCCESS;
  const int32_t state = ATOMIC_LOAD(&tx_data.state_);

  // NB: The functor is only used during minor merge
  if (ObTxData::ABORT == state) {
    // Case 1: data is aborted, so we donot need it during merge
    can_read_ = false;
  } else if (tx_data.undo_status_list_.is_contain(sql_sequence_)) {
    // Case 2: data is rollbacked in undo status, so we donot need it during merge
    can_read_ = false;
  } else {
    // Case 3: data is committed or during execution, so we need it during merge
    can_read_ = true;
  }

  return ret;
}

int CheckRowLockedFunctor::operator() (const ObTxData &tx_data, ObTxCCCtx *tx_cc_ctx)
{
  UNUSED(tx_cc_ctx);
  int ret = OB_SUCCESS;
  const int32_t state = ATOMIC_LOAD(&tx_data.state_);
  const SCN commit_version = tx_data.commit_version_.atomic_load();

  switch (state) {
  case ObTxData::COMMIT: {
    // Case 1: data is committed, so the lock is locked by the data and we
    // also need return the commit version for tsc check
    lock_state_.is_locked_ = false;
    lock_state_.trans_version_ = commit_version;
    break;
  }
  case ObTxData::RUNNING:
  case ObTxData::ELR_COMMIT: {
    if (read_tx_id_ == data_tx_id_) {
      // Case 2: data is during execution and it is owned by the checker, so
      // whether the lock is locked by the data depends on whether undo status
      // conains the data and the tsc version is unnecessary for the running
      // txn.
      lock_state_.is_locked_ = !tx_data.undo_status_list_.is_contain(sql_sequence_);
      lock_state_.trans_version_.set_min();
    } else {
      // Case 3: data is during execution and it is not owned by the checker, so
      // whether the lock is locked by the data depends on whether undo status
      // conains the data and the tsc version is unnecessary for the running txn.
      lock_state_.is_locked_ = !tx_data.undo_status_list_.is_contain(sql_sequence_);
      lock_state_.trans_version_.set_min();
    }
    break;
  }
  case ObTxData::ABORT: {
    // Case 1: data is aborted, so the lock is not locked by the data and
    // the tsc version is unnecessary for the aborted txn
    lock_state_.is_locked_ = false;
    lock_state_.trans_version_.set_min();
    break;
  default:
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "wrong state", K(tx_data), KPC(tx_cc_ctx));
    break;
  }
  }
  if (lock_state_.is_locked_) {
    lock_state_.lock_trans_id_ = data_tx_id_;
    lock_state_.lock_data_sequence_ = sql_sequence_;
    lock_state_.is_delayed_cleanout_ = true;
  }

  return ret;
}


int GetTxStateWithSCNFunctor::operator()(const ObTxData &tx_data, ObTxCCCtx *tx_cc_ctx)
{
  UNUSED(tx_cc_ctx);
  int ret = OB_SUCCESS;
  const int32_t state = ATOMIC_LOAD(&tx_data.state_);
  const SCN commit_version = tx_data.commit_version_.atomic_load();
  const SCN end_scn = tx_data.end_scn_.atomic_load();

  // return the transaction state_ according to the merge log ts.
  // the detailed document is available as follows.
  // https://yuque.antfin-inc.com/docs/share/a3160d5e-6e1a-4980-a12e-4af653c6cf57?#
  if (ObTxData::RUNNING == state || ObTxData::ELR_COMMIT == state) {
    // Case 1: data is during execution, so we return the running state with
    // INT64_MAX as version
    state_ = ObTxData::RUNNING;
    trans_version_ = SCN::max_scn();
  } else if (scn_ < end_scn) {
    // Case 2: data is decided while the required state is before the merge log
    // ts, so we return the running state with INT64_MAX as txn version
    state_ = ObTxData::RUNNING;
    trans_version_.set_max();
  } else if (ObTxData::COMMIT == state) {
    // Case 3: data is committed and the required state is after the merge log
    // ts, so we return the commit state with commit version as txn version
    state_ = ObTxData::COMMIT;
    trans_version_ = commit_version;
  } else if (ObTxData::ABORT == state) {
    // Case 4: data is aborted and the required state is after the merge log
    // ts, so we return the abort state with 0 as txn version
    state_ = ObTxData::ABORT;
    trans_version_ = SCN::min_scn();
  } else {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(ERROR, "unexpected transaction state_", K(ret), K(tx_data));
  }

  return ret;
}


int LockForReadFunctor::inner_lock_for_read(const ObTxData &tx_data, ObTxCCCtx *tx_cc_ctx)
{
  int ret = OB_SUCCESS;
  const transaction::ObTxSnapshot &snapshot = lock_for_read_arg_.mvcc_acc_ctx_.snapshot_;
  const SCN &snapshot_version = snapshot.version_;
  const transaction::ObTransID snapshot_tx_id = snapshot.tx_id_;
  const int64_t snapshot_sql_sequence = snapshot.scn_;

  const transaction::ObTransID data_tx_id = lock_for_read_arg_.data_trans_id_;
  const int64_t data_sql_sequence = lock_for_read_arg_.data_sql_sequence_;
  const bool read_latest = lock_for_read_arg_.read_latest_;
  const transaction::ObTransID reader_tx_id = lock_for_read_arg_.mvcc_acc_ctx_.tx_id_;

  // NB: We need pay much attention to the order of the reads to the different
  // variables. Although we update the version before the state for the tnodes
  // and read the state before the version. It may appear that the compiled code
  // execution may rearrange its order and fail to obey its origin logic(You can
  // read the Dependency Definiation of the ARM architecture book to understand
  // it). So the synchronization primitive below is much important.
  const int32_t state = ATOMIC_LOAD(&tx_data.state_);
  const SCN commit_version = tx_data.commit_version_.atomic_load();

  can_read_ = false;
  trans_version_ = SCN::invalid_scn();
  is_determined_state_ = false;

  switch (state) {
    case ObTxData::COMMIT: {
      // Case 1: data is committed, so the state is decided and whether we can read
      // depends on whether undo status contains the data. Then we return the commit
      // version as data version.
      can_read_ = !tx_data.undo_status_list_.is_contain(data_sql_sequence);
      trans_version_ = commit_version;
      is_determined_state_ = true;
      break;
    }
    case ObTxData::RUNNING:
    case ObTxData::ELR_COMMIT: {
      // Case 2: data is during execution, so the state is not decided.
      if (read_latest && reader_tx_id == data_tx_id) {
        // Case 2.0: read the latest written of current txn
        can_read_ = !tx_data.undo_status_list_.is_contain(data_sql_sequence);
        trans_version_ = SCN::min_scn();
      } else if (snapshot_tx_id == data_tx_id) {
        // Case 2.1: data is owned by the read txn
        bool tmp_can_read = false;
        if (data_sql_sequence <= snapshot_sql_sequence) {
          // Case 2.1.1: data's sequence number is smaller than the read txn's
          // sequence number, so we can read it if it is not undone
          tmp_can_read = true;
        } else {
          // Case 2.1.3: data's sequence number is equal or bigger than the read
          // txn's sequence number and we need not read the latest data(to
          // prevent Halloween problem), so we cannot read it
          tmp_can_read = false;
        }
        // Tip 2.1.1: we should skip the data if it is undone
        can_read_ = tmp_can_read &&
          !tx_data.undo_status_list_.is_contain(data_sql_sequence);
        // Tip 2.1.2: trans version is unnecessary for the running txn
        trans_version_ = SCN::min_scn();
      } else {
        // Case 2.2: data is not owned by the read txn
        // NB: we need pay attention to the choice condition when issuing the
        // lock_for_read, we cannot only treat state in exec_info as judgement
        // whether txn is prepared, because the state in exec_info will not be
        // updated as prepared until log is applied and the application is
        // asynchronous. So we need use version instead of state as judgement and
        // mark it whenever we submit the commit/prepare log(using before_prepare)
        if (tx_cc_ctx->prepare_version_.is_max()) {
          // Case 2.2.1: data is not in 2pc state, so the prepare version and
          // commit version of the data must be bigger than the read txn's
          // snapshot version, so we cannot read it and trans version is
          // unnecessary for the running txn
          can_read_ = false;
          trans_version_ = SCN::min_scn();
        } else if (tx_cc_ctx->prepare_version_ > snapshot_version) {
          // Case 2.2.2: data is at least in prepare state and the prepare
          // version is bigger than the read txn's snapshot version, then the
          // data's commit version must be bigger than the read txn's snapshot
          // version, so we cannot read it and trans version is unnecessary for
          // the running txn
          can_read_ = false;
          trans_version_ = SCN::min_scn();
        } else {
          // Only dml statement can read elr data
          if (ObTxData::ELR_COMMIT == state
              && lock_for_read_arg_.mvcc_acc_ctx_.snapshot_.tx_id_.is_valid()) {
            can_read_ = !tx_data.undo_status_list_.is_contain(data_sql_sequence);
            trans_version_ = commit_version;
          } else {
            // Case 2.2.3: data is in prepare state and the prepare version is
            // smaller than the read txn's snapshot version, then the data's
            // commit version may or may not be bigger than the read txn's
            // snapshot version, so we are unsure whether we can read it and we
            // need wait for the commit version of the data
            ret = OB_ERR_SHARED_LOCK_CONFLICT;
            if (REACH_TIME_INTERVAL(1 * 1000 * 1000)) {
              TRANS_LOG(WARN, "lock_for_read need retry", K(ret),
                        K(tx_data), K(lock_for_read_arg_), K(tx_cc_ctx));
            }
          }
        }
      }
      // Tip 2.1: data is during execution, so the state is not decided.
      is_determined_state_ = false;
      break;
    }
    case ObTxData::ABORT: {
      // Case 3: data is aborted, so the state is decided, then we can not read
      // the data and the trans version is unnecessary for the aborted txn
      can_read_ = false;
      trans_version_ = SCN::min_scn();
      is_determined_state_ = true;
      break;
    }
    default:
      break;
  }

  return ret;
}

bool LockForReadFunctor::recheck()
{
  return recheck_op_();
}

int LockForReadFunctor::operator()(const ObTxData &tx_data, ObTxCCCtx *tx_cc_ctx)
{
  int ret = OB_ERR_SHARED_LOCK_CONFLICT;
  const int64_t MAX_SLEEP_US = 1000;
  auto &acc_ctx = lock_for_read_arg_.mvcc_acc_ctx_;
  auto lock_expire_ts = acc_ctx.eval_lock_expire_ts();
  // check lock_for_read blocked or not every 1ms * 100 = 100ms
  int64_t retry_cnt = 0;
  const int64_t MAX_RETRY_CNT = 100;

  const int32_t state = ATOMIC_LOAD(&tx_data.state_);

  if (OB_ISNULL(tx_cc_ctx) && (ObTxData::RUNNING == state)) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "lock for read functor need prepare version.", KR(ret));
  } else {
    for (int32_t i = 0; OB_ERR_SHARED_LOCK_CONFLICT == ret; i++) {
      retry_cnt++;
      if (OB_FAIL(inner_lock_for_read(tx_data, tx_cc_ctx))) {
        if (OB_UNLIKELY(observer::SS_STOPPING == GCTX.status_) ||
            OB_UNLIKELY(observer::SS_STOPPED == GCTX.status_)) {
          // rewrite ret
          ret = OB_SERVER_IS_STOPPING;
          TRANS_LOG(WARN, "observer is stopped", K(ret));
        } else if (ObTimeUtility::current_time() + MIN(i, MAX_SLEEP_US) >= lock_expire_ts) {
          ret = OB_ERR_SHARED_LOCK_CONFLICT;
          break;
        } else if (!MTL_IS_PRIMARY_TENANT() && OB_SUCC(check_for_standby(tx_data.tx_id_))) {
          TRANS_LOG(INFO, "read by standby tenant success", K(tx_data), KPC(tx_cc_ctx), KPC(this));
          break;
        } else if (i < 10) {
          PAUSE();
        } else {
          ob_usleep((i < MAX_SLEEP_US ? i : MAX_SLEEP_US));
        }
        if (retry_cnt == MAX_RETRY_CNT) {
          retry_cnt = 0;
          logservice::coordinator::ObFailureDetector *detector = MTL(logservice::coordinator::ObFailureDetector *);
          if (NULL != detector && detector->is_clog_disk_has_fatal_error()) {
            ret = OB_IO_ERROR;
            TRANS_LOG(ERROR, "unexpected io error", K(ret), K(tx_data), KPC(tx_cc_ctx), KPC(this));
          }
        }
      }
    }
  }

  if (OB_SUCC(ret) && OB_FAIL(cleanout_op_(tx_data, tx_cc_ctx))) {
    TRANS_LOG(WARN, "cleanout failed", K(ret), K(cleanout_op_), KPC(this),
              K(tx_data), KPC(tx_cc_ctx));
  }

  TRANS_LOG(DEBUG, "lock for read", K(ret), K(tx_data), KPC(tx_cc_ctx), KPC(this));

  return ret;
}

int LockForReadFunctor::check_for_standby(const transaction::ObTransID &tx_id)
{
  int ret = OB_SUCCESS;
  transaction::ObPartTransCtx *ctx = NULL;
  if (OB_SUCC(MTL(transaction::ObTransService *)->get_tx_ctx(ls_id_, tx_id, ctx))) {
    ret = ctx->check_for_standby(lock_for_read_arg_.mvcc_acc_ctx_.snapshot_.version_,
                                 can_read_, trans_version_, is_determined_state_);
    MTL(transaction::ObTransService *)->revert_tx_ctx(ctx);
  }
  return ret;
}

int CleanoutTxStateFunctor::operator()(const ObTxData &tx_data, ObTxCCCtx *tx_cc_ctx)
{
  return operation_(tx_data, tx_cc_ctx);
}

bool ObReCheckTxNodeForLockForReadOperation::operator()()
{
  bool ret = false;

  if (tnode_.is_aborted()) {
    can_read_ = false;
    trans_version_ = SCN::min_scn();
    is_determined_state_ = true;
    ret = true;
  }

  return ret;
}

bool ObReCheckNothingOperation::operator()()
{
  bool ret = false;
  return ret;
}

int ObCleanoutTxNodeOperation::operator()(const ObTxData &tx_data, ObTxCCCtx *tx_cc_ctx)
{
  int ret = OB_SUCCESS;
  const int32_t state = ATOMIC_LOAD(&tx_data.state_);
  const SCN commit_version = tx_data.commit_version_.atomic_load();
  const SCN end_scn = tx_data.end_scn_.atomic_load();

  if (ObTxData::RUNNING == state
      && !tx_data.undo_status_list_.is_contain(tnode_.seq_no_)
      // NB: we need pay attention to the choice condition when issuing the
      // lock_for_read, we cannot only treat state in exec_info as judgement
      // whether txn is prepared, because the state in exec_info will not be
      // updated as prepared until log is applied and the application is
      // asynchronous. So we need use version instead of state as judgement and
      // mark it whenever we submit the commit/prepare log(using before_prepare)
      && tx_cc_ctx->prepare_version_.is_max()) {
    // Case 1: data is during execution, so we donot need write back
    // This is the case for most of the lock for read scenerio, so we need to
    // mainly optimize it through not latching the row
  } else if (!(tnode_.is_committed() || tnode_.is_aborted())
             && tnode_.is_delayed_cleanout()) {
    if (need_row_latch_) {
      value_.latch_.lock();
    }
    if (!(tnode_.is_committed() || tnode_.is_aborted())
        && tnode_.is_delayed_cleanout()) {
      if (tx_data.undo_status_list_.is_contain(tnode_.seq_no_)) {
        // Case 2: data is rollbacked during execution, so we write back the abort state
        if (OB_FAIL(value_.unlink_trans_node(tnode_))) {
          TRANS_LOG(WARN, "mvcc trans ctx trans commit error", K(ret), K(value_), K(tnode_));
        } else {
          (void)tnode_.trans_abort(tx_data.end_scn_);
        }
      } else if (ObTxData::RUNNING == state || ObTxData::ELR_COMMIT == state) {
        if (!tx_cc_ctx->prepare_version_.is_max()) {
          // Case 3: data is prepared, we also donot write back the prepare state
        }
      } else if (ObTxData::COMMIT == state) {
        // Case 4: data is committed, so we should write back the commit state
        if (OB_FAIL(value_.trans_commit(commit_version, tnode_))) {
          TRANS_LOG(WARN, "mvcc trans ctx trans commit error", K(ret), K(value_), K(tnode_));
        } else if (FALSE_IT(tnode_.trans_commit(commit_version, end_scn))) {
        } else if (blocksstable::ObDmlFlag::DF_LOCK == tnode_.get_dml_flag()
                   && OB_FAIL(value_.unlink_trans_node(tnode_))) {
          TRANS_LOG(WARN, "unlink lock node failed", K(ret), K(value_), K(tnode_));
        }
      } else if (ObTxData::ABORT == state) {
        // Case 6: data is aborted, so we write back the abort state

        if (OB_FAIL(value_.unlink_trans_node(tnode_))) {
          TRANS_LOG(WARN, "mvcc trans ctx trans commit error", K(ret), K(value_), K(tnode_));
        } else {
          (void)tnode_.trans_abort(end_scn);
        }
      } else {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "unexpected transaction state_", K(ret), K(tx_data));
      }
    }

    if (need_row_latch_) {
      value_.latch_.unlock();
    }
  }

  TRANS_LOG(DEBUG, "cleanout tx state", K(ret), K(tx_data), KPC(tx_cc_ctx), KPC(this));

  return ret;
}

int ObCleanoutNothingOperation::operator()(const ObTxData &tx_data, ObTxCCCtx *tx_cc_ctx)
{
  UNUSED(tx_data);
  UNUSED(tx_cc_ctx);

  return OB_SUCCESS;
}

} // namespace storage
} // namespace oceanbase
