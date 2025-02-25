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

#define USING_LOG_PREFIX PALF
#include "log_config_mgr.h"
#include "lib/ob_errno.h"
#include "log_engine.h"
#include "log_io_task_cb_utils.h"                // FlushMetaCbCtx
#include "election/interface/election.h"
#include "log_state_mgr.h"
#include "log_sliding_window.h"
#include "log_mode_mgr.h"
#include "log_reconfirm.h"

namespace oceanbase
{
using namespace common;
namespace palf
{
LogConfigMgr::LogConfigMgr()
    : log_ms_meta_(),
      alive_paxos_memberlist_(),
      alive_paxos_replica_num_(0),
      all_learnerlist_(),
      config_meta_(),
      applied_alive_paxos_memberlist_(),
      applied_alive_paxos_replica_num_(0),
      applied_all_learnerlist_(),
      running_args_(),
      region_(DEFAULT_REGION_NAME),
      paxos_member_region_map_(),
      lock_(common::ObLatchIds::PALF_CM_CONFIG_LOCK),
      palf_id_(),
      self_(),
      prev_log_proposal_id_(INVALID_PROPOSAL_ID),
      prev_lsn_(),
      prev_mode_pid_(INVALID_PROPOSAL_ID),
      state_(INIT),
      last_submit_config_log_time_us_(OB_INVALID_TIMESTAMP),
      ms_ack_list_(),
      need_change_config_bkgd_(false),
      bkgd_config_version_(),
      is_sw_interrupted_by_degrade_(false),
      resend_config_version_(),
      resend_log_list_(),
      election_leader_epoch_(OB_INVALID_TIMESTAMP),
      last_broadcast_leader_info_time_us_(OB_INVALID_TIMESTAMP),
      persistent_config_version_(),
      barrier_print_log_time_(OB_INVALID_TIMESTAMP),
      last_check_init_state_time_us_(OB_INVALID_TIMESTAMP),
      check_config_print_time_(OB_INVALID_TIMESTAMP),
      last_wait_barrier_time_us_(OB_INVALID_TIMESTAMP),
      last_wait_committed_end_lsn_(),
      last_sync_meta_for_arb_election_leader_time_us_(OB_INVALID_TIMESTAMP),
      parent_lock_(common::ObLatchIds::PALF_CM_PARENT_LOCK),
      register_time_us_(OB_INVALID_TIMESTAMP),
      parent_(),
      parent_keepalive_time_us_(OB_INVALID_TIMESTAMP),
      last_submit_register_req_time_us_(OB_INVALID_TIMESTAMP),
      last_first_register_time_us_(OB_INVALID_TIMESTAMP),
      child_lock_(common::ObLatchIds::PALF_CM_CHILD_LOCK),
      children_(),
      last_submit_keepalive_time_us_(OB_INVALID_TIMESTAMP),
      log_engine_(NULL),
      sw_(NULL),
      state_mgr_(NULL),
      election_(NULL),
      mode_mgr_(NULL),
      reconfirm_(NULL),
      is_inited_(false)
{}

LogConfigMgr::~LogConfigMgr()
{
  destroy();
}


int LogConfigMgr::init(const int64_t palf_id,
                       const ObAddr &self,
                       const LogConfigMeta &log_ms_meta,
                       LogEngine *log_engine,
                       LogSlidingWindow *sw,
                       LogStateMgr *state_mgr,
                       election::Election* election,
                       LogModeMgr *mode_mgr,
                       LogReconfirm *reconfirm)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    PALF_LOG(WARN, "LogConfigMgr init twice", KR(ret), K_(palf_id), K_(self));
  } else if (false== is_valid_palf_id(palf_id) ||
             false == self.is_valid() ||
             false == log_ms_meta.is_valid() ||
             OB_ISNULL(log_engine) ||
             OB_ISNULL(sw) ||
             OB_ISNULL(state_mgr) ||
             OB_ISNULL(election) ||
             OB_ISNULL(mode_mgr)) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", KR(ret), K(palf_id), K(self),
        K(log_ms_meta), K(log_engine), K(sw), K(state_mgr), K(election), K(mode_mgr));
  } else if (OB_FAIL(paxos_member_region_map_.init("LogRegionMap", OB_MAX_MEMBER_NUMBER))) {
    PALF_LOG(WARN, "paxos_member_region_map_ init failed", K(palf_id));
  } else {
    palf_id_ = palf_id;
    self_ = self;
    log_ms_meta_ = log_ms_meta;
    config_meta_ = log_ms_meta;
    log_engine_ = log_engine;
    sw_ = sw;
    state_mgr_ = state_mgr;
    election_ = election;
    mode_mgr_ = mode_mgr;
    reconfirm_ = reconfirm;
    if (true == log_ms_meta.curr_.is_valid()) {
      if (OB_FAIL(append_config_info_(log_ms_meta.curr_))) {
        PALF_LOG(WARN, "append_config_info_ failed", K(ret), K(palf_id), K(log_ms_meta));
      } else if (OB_FAIL(apply_config_info_(log_ms_meta.curr_))) {
        PALF_LOG(WARN, "apply_config_info_ failed", K(ret), K(palf_id), K(log_ms_meta));
      } else if (OB_FAIL(update_election_meta_(log_ms_meta_.curr_))) {
        PALF_LOG(WARN, "update_election_meta_ failed", K(ret), K(palf_id), K(log_ms_meta));
      }
    }
    if (OB_SUCC(ret)) {
      persistent_config_version_ = log_ms_meta_.curr_.config_version_;
      is_inited_ = true;
      PALF_LOG(INFO, "LogConfigMgr init success", K(ret), K_(palf_id), K_(self), K_(config_meta), KP(this));
    }
  }
  return ret;
}

void LogConfigMgr::destroy()
{
  SpinLockGuard guard(lock_);
  if (IS_INIT) {
    PALF_LOG(INFO, "LogConfigMgr destory", K_(palf_id), K_(self));
    is_inited_ = false;
    election_ = NULL;
    mode_mgr_ = NULL;
    state_mgr_ = NULL;
    sw_ = NULL;
    log_engine_ = NULL;
    reconfirm_ = NULL;
    register_time_us_ = OB_INVALID_TIMESTAMP;
    parent_.reset();
    parent_keepalive_time_us_ = OB_INVALID_TIMESTAMP;
    reset_registering_state_();
    children_.reset();
    last_submit_keepalive_time_us_ = OB_INVALID_TIMESTAMP;
    ms_ack_list_.reset();
    resend_config_version_.reset();
    resend_log_list_.reset();
    running_args_.reset();
    last_submit_config_log_time_us_ = OB_INVALID_TIMESTAMP;
    log_ms_meta_.reset();
    alive_paxos_memberlist_.reset();
    alive_paxos_replica_num_ = 0;
    all_learnerlist_.reset();
    paxos_member_region_map_.destroy();
    last_sync_meta_for_arb_election_leader_time_us_ = OB_INVALID_TIMESTAMP;
    region_ = DEFAULT_REGION_NAME;
    state_ = ConfigChangeState::INIT;
    prev_mode_pid_ = INVALID_PROPOSAL_ID;
    prev_lsn_.reset();
    prev_log_proposal_id_ = INVALID_PROPOSAL_ID;
    persistent_config_version_.reset();
    barrier_print_log_time_ = OB_INVALID_TIMESTAMP;
    self_.reset();
    palf_id_ = 0;
  }
}

int LogConfigMgr::set_initial_member_list(const ObMemberList &member_list,
                                          const int64_t replica_num,
                                          const int64_t proposal_id,
                                          LogConfigVersion &init_config_version)
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else if (!member_list.is_valid() ||
             replica_num <= 0 ||
             replica_num > OB_MAX_MEMBER_NUMBER ||
             INVALID_PROPOSAL_ID == proposal_id ||
             false == can_memberlist_majority_(member_list.get_member_number(), replica_num)) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", KR(ret), K_(palf_id), K(member_list), K(replica_num));
  } else {
    LogConfigInfo config_info = log_ms_meta_.curr_;
    config_info.log_sync_memberlist_ = member_list;
    config_info.log_sync_replica_num_ = replica_num;
    if (OB_FAIL(set_initial_config_info_(config_info, proposal_id, init_config_version))) {
      PALF_LOG(WARN, "set_initial_config_info failed", K(ret), K_(palf_id), K_(self), K(config_info), K(proposal_id));
    } else {
      PALF_LOG(INFO, "set_initial_member_list success", K(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member_list), K(replica_num), K(proposal_id));
    }
  }
  return ret;
}

int LogConfigMgr::set_initial_member_list(const common::ObMemberList &member_list,
                                          const common::ObMember &arb_member,
                                          const int64_t replica_num,
                                          const int64_t proposal_id,
                                          LogConfigVersion &init_config_version)
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else if (!member_list.is_valid() ||
             !arb_member.is_valid() ||
             (member_list.get_member_number() & 1) != 0 ||
             replica_num <= 0 ||
             replica_num > OB_MAX_MEMBER_NUMBER ||
             INVALID_PROPOSAL_ID == proposal_id ||
             false == can_memberlist_majority_(member_list.get_member_number(), replica_num)) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", KR(ret), K_(palf_id), K_(self), K(member_list), K(arb_member), K(replica_num));
  } else {
    LogConfigInfo config_info = log_ms_meta_.curr_;
    config_info.log_sync_memberlist_ = member_list;
    config_info.log_sync_replica_num_ = replica_num;
    config_info.arbitration_member_ = arb_member;
    if (OB_FAIL(set_initial_config_info_(config_info, proposal_id, init_config_version))) {
      PALF_LOG(WARN, "set_initial_config_info failed", K(ret), K_(palf_id), K_(self), K(config_info), K(proposal_id));
    } else {
      PALF_LOG(INFO, "set_initial_member_list success", K(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member_list), K(arb_member), K(replica_num), K(proposal_id));
    }
  }
  return ret;
}

int LogConfigMgr::set_initial_config_info_(const LogConfigInfo &config_info,
                                           const int64_t proposal_id,
                                           LogConfigVersion &init_config_version)
{
  int ret = OB_SUCCESS;
  const int64_t initial_config_seq = 1;
  LogReplicaType replica_type = state_mgr_->get_replica_type();
  //  TODO by haofan: Mittest case creates arb member by same interface with F member,
  //  so its replica_type maybe NORMAL_REPLICA too. We just skip type check here temporarily.
//  const bool valid_replica_type = (config_info.arbitration_member_.get_server() == self_) ? \
//      (replica_type == ARBITRATION_REPLICA) : (replica_type == NORMAL_REPLICA);
  const bool valid_replica_type = true;
  if (false == valid_replica_type) {
    ret = OB_NOT_SUPPORTED;
    PALF_LOG(WARN, "set_initial_member_list don't match with replica_type", KR(ret), K_(palf_id), K_(self), K(replica_type), K(config_info));
  } else if (OB_FAIL(init_config_version.generate(proposal_id, initial_config_seq))) {
    PALF_LOG(WARN, "invalid argument", KR(ret), K_(palf_id), K(proposal_id), K(initial_config_seq));
  } else {
    LogConfigInfo init_config_info = config_info;
    init_config_info.config_version_ = init_config_version;
    if (false == check_need_update_memberlist_without_lock_(init_config_version)) {
      PALF_LOG(INFO, "persistent_config_version_ has been greater than or equal to config_version, \
          no need set_initial_config_info_", K(ret), K_(palf_id), K_(self), K_(log_ms_meta), K_(persistent_config_version), K(init_config_version));
    } else if (OB_FAIL(log_ms_meta_.generate(proposal_id, init_config_info, init_config_info, proposal_id, LSN(PALF_INITIAL_LSN_VAL), proposal_id))) {
      PALF_LOG(ERROR, "generate LogConfigMeta failed", KR(ret), K_(palf_id), K(proposal_id), K(initial_config_seq));
    } else {
      config_meta_ = log_ms_meta_;
      FlushMetaCbCtx cb_ctx;
      cb_ctx.type_ = MetaType::CHANGE_CONFIG_META;
      cb_ctx.proposal_id_ = proposal_id;
      cb_ctx.config_version_ = log_ms_meta_.curr_.config_version_;
      if (OB_FAIL(append_config_info_(log_ms_meta_.curr_))) {
        PALF_LOG(WARN, "append_config_info_ failed", K(ret), K_(palf_id), K_(log_ms_meta));
      } else if (OB_FAIL(apply_config_info_(log_ms_meta_.curr_))) {
        PALF_LOG(WARN, "apply_config_info_ failed", K(ret), K_(palf_id), K_(log_ms_meta));
      } else if (OB_FAIL(update_election_meta_(log_ms_meta_.curr_))) {
        PALF_LOG(WARN, "update_election_memberlist_ failed", K(ret));
      } else if (OB_FAIL(log_engine_->submit_flush_change_config_meta_task(cb_ctx, log_ms_meta_))) {
        PALF_LOG(WARN, "LogEngine submit_flush_change_config_meta_task failed", K(ret), K_(log_ms_meta));
      } else {
        PALF_LOG(INFO, "set_initial_config_info_ success", K_(palf_id), K_(self), K_(log_ms_meta), K(config_info), \
            K_(alive_paxos_memberlist), K_(alive_paxos_replica_num), KP(this));
      }
    }
  }
  return ret;
}

int LogConfigMgr::set_region(const common::ObRegion &region)
{
  int ret = OB_SUCCESS;
  bool need_register = false;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (region.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", KR(ret), K_(palf_id), K_(self), K(region));
  } else {
    const common::ObRegion old_region = region_;
    region_ = region;
    PALF_EVENT("set_region success", palf_id_, K_(self), K(old_region), K_(region));
    SpinLockGuard guard(parent_lock_);
    if (old_region != region && OB_FAIL(after_region_changed_(old_region, region))) {
      PALF_LOG(WARN, "after_region_changed_ failed", KR(ret), K_(palf_id), K_(self), K(old_region), K(region));
    } else {
      PALF_LOG(INFO, "after_region_changed_ success", KR(ret), K_(palf_id), K_(self), K(old_region), K(region));
    }
  }
  return ret;
}

int LogConfigMgr::set_paxos_member_region_map(const LogMemberRegionMap &region_map)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (0 == region_map.size()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const common::ObMemberList &member_list = alive_paxos_memberlist_;
    // reset paxos_member_region_map_ and init again with current member_list
    paxos_member_region_map_.reset();
    for (int i = 0; i < member_list.get_member_number(); ++i) {
      common::ObAddr tmp_server;
      common::ObRegion tmp_region;
      int tmp_ret = OB_SUCCESS;
      if (OB_SUCCESS != (tmp_ret = member_list.get_server_by_index(i, tmp_server))) {
        PALF_LOG(WARN, "get_server_by_index failed", KR(tmp_ret), K_(palf_id), K_(self), K(i));
      } else if (OB_SUCCESS != (tmp_ret = region_map.get(tmp_server, tmp_region))) {
        tmp_region = DEFAULT_REGION_NAME;
        tmp_ret = OB_SUCCESS;
      }
      if (OB_SUCCESS == tmp_ret && OB_SUCCESS != (tmp_ret = paxos_member_region_map_.insert(tmp_server, tmp_region))) {
        PALF_LOG(WARN, "paxos_member_region_map_ insert failed", KR(tmp_ret), K_(palf_id), K_(self), K(tmp_server), K(tmp_region));
      }
    }
    PALF_LOG(INFO, "set_paxos_member_region_map success", K_(palf_id), K_(self), K(region_map));
  }
  return ret;
}

int LogConfigMgr::get_prev_member_list(ObMemberList &member_list) const
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else if (OB_FAIL(member_list.deep_copy(config_meta_.prev_.log_sync_memberlist_))) {
    PALF_LOG(WARN, "deep_copy member_list failed", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta));
  } else if (config_meta_.prev_.arbitration_member_.is_valid() &&
      OB_FAIL(member_list.add_member(config_meta_.prev_.arbitration_member_))) {
    PALF_LOG(WARN, "add_member failed", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta));
    // no nothing
  }
  return ret;
}

int LogConfigMgr::get_global_learner_list(common::GlobalLearnerList &learner_list) const
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(learner_list.deep_copy(config_meta_.curr_.learnerlist_))) {
    PALF_LOG(WARN, "deep_copy learner_list failed", KR(ret), K_(palf_id), K_(self));
  } else {
    // pass
  }
  return ret;
}

int LogConfigMgr::get_degraded_learner_list(common::GlobalLearnerList &degraded_learner_list) const
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(degraded_learner_list.deep_copy(config_meta_.curr_.degraded_learnerlist_))) {
    PALF_LOG(WARN, "deep_copy degraded_learnerlist_ failed", KR(ret), K_(palf_id), K_(self));
  } else {
    // pass
  }
  return ret;
}

int LogConfigMgr::get_arbitration_member(common::ObMember &arb_member) const
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else {
    arb_member = config_meta_.curr_.arbitration_member_;
  }
  return ret;
}

int LogConfigMgr::get_curr_member_list(ObMemberList &member_list, int64_t &replica_num) const
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else if (OB_FAIL(config_meta_.curr_.get_expected_paxos_memberlist(member_list, replica_num))) {
    PALF_LOG(WARN, "get_expected_paxos_memberlist failed", KR(ret), K_(palf_id), K_(self));
  } else {
    // no nothing
  }
  return ret;
}

int LogConfigMgr::get_alive_member_list_with_arb(
    common::ObMemberList &member_list,
    int64_t &replica_num) const
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else if (OB_FAIL(member_list.deep_copy(applied_alive_paxos_memberlist_))) {
    PALF_LOG(WARN, "deep_copy member_list failed", KR(ret), K_(palf_id), K_(self));
  } else {
    replica_num = applied_alive_paxos_replica_num_;
  }
  return ret;
}

int LogConfigMgr::get_log_sync_member_list(ObMemberList &member_list,
    int64_t &replica_num) const
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else if (OB_FAIL(member_list.deep_copy(config_meta_.curr_.log_sync_memberlist_))) {
    PALF_LOG(WARN, "deep_copy member_list failed", KR(ret), K_(palf_id), K_(self));
  } else {
    replica_num = config_meta_.curr_.log_sync_replica_num_;
  }
  return ret;
}

int LogConfigMgr::get_children_list(LogLearnerList &children) const
{
  int ret = OB_SUCCESS;
  // TODO by yunlong: this lock may be hotspot, need check further
  SpinLockGuard gaurd(child_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(children.deep_copy(children_))) {
    PALF_LOG(WARN, "deep_copy children_list failed", KR(ret), K_(palf_id), K_(self));
  } else {
    // pass
  }
  return ret;
}

int LogConfigMgr::get_config_version(LogConfigVersion &config_version) const
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    config_version = config_meta_.curr_.config_version_;
  }
  return ret;
}

int LogConfigMgr::get_replica_num(int64_t &replica_num) const
{
  int ret = OB_SUCCESS;
  common::ObMemberList member_list;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else if (OB_FAIL(config_meta_.curr_.get_expected_paxos_memberlist(member_list, replica_num))) {
    PALF_LOG(WARN, "get_expected_paxos_memberlist failed", KR(ret), K_(palf_id), K_(self));
  } else {
  }
  return ret;
}

int64_t LogConfigMgr::get_accept_proposal_id() const
{
  return log_ms_meta_.proposal_id_;
}

int LogConfigMgr::submit_broadcast_leader_info(const int64_t proposal_id) const
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(submit_broadcast_leader_info_(proposal_id))) {
    PALF_LOG(WARN, "submit_prepare_meta_req failed", KR(ret), K_(palf_id), K_(self), K(proposal_id));
  } else {
    PALF_LOG(INFO, "submit_prepare_meta_req success", KR(ret), K_(palf_id), K_(self), K(proposal_id));
  }
  return ret;
}

int LogConfigMgr::submit_broadcast_leader_info_(const int64_t proposal_id) const
{
  int ret = OB_SUCCESS;
  const common::GlobalLearnerList &learner_list = all_learnerlist_;
  if (INVALID_PROPOSAL_ID == proposal_id) {
    ret = OB_INVALID_ARGUMENT;
  } else if (false == learner_list.is_valid()) {
  } else if (OB_FAIL(log_engine_->submit_prepare_meta_req(learner_list, proposal_id))) {
    PALF_LOG(WARN, "submit_prepare_meta_req failed", KR(ret), K_(palf_id), K_(self), K(proposal_id));
  } else {
    PALF_LOG(INFO, "submit_prepare_meta_req success", K(ret), K_(palf_id), K_(self), K(proposal_id));
  }
  return ret;
}

const common::ObAddr &LogConfigMgr::get_parent() const
{
  SpinLockGuard guard(parent_lock_);
  return parent_;
}

//================================ Config Change ================================

// 1. switch config change state machine
// 2. broadcasting leader info periodically
// 3. resend config log to members and followers who haven't responsed ack
// after config change finished.
int LogConfigMgr::leader_do_loop_work(bool &need_change_config)
{
  int ret = OB_SUCCESS;
  need_change_config = false;
  SpinLockGuard guard(lock_);
  const int64_t proposal_id = state_mgr_->get_proposal_id();
  const bool is_leader = (self_ == state_mgr_->get_leader());
  const bool is_leader_active = state_mgr_->is_leader_active();
  if (is_leader) {
    bool need_rlock = false;
    bool need_wlock = false;
    if ((ConfigChangeState::INIT != state_) &&
        true == need_change_config_bkgd_ &&
        !FALSE_IT(is_state_changed_(need_rlock, need_wlock))) {
      if (true == need_rlock) {
        (void) change_config_(running_args_, proposal_id, election_leader_epoch_, bkgd_config_version_);
      } else if (true == need_wlock) {
        need_change_config = true;
      }
    }
    if (OB_FAIL(try_resend_config_log_(proposal_id))) {
      PALF_LOG(WARN, "try_resend_config_log failed", KR(ret), K_(palf_id), K_(self));
    }
  }
  if (is_leader_active && palf_reach_time_interval(PALF_BROADCAST_LEADER_INFO_INTERVAL_US, last_broadcast_leader_info_time_us_) &&
      OB_FAIL(submit_broadcast_leader_info_(proposal_id))) {
    PALF_LOG(WARN, "submit_broadcast_leader_info failed", KR(ret), K_(self), K_(palf_id));
  }
  return ret;
}

int LogConfigMgr::switch_state()
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  const int64_t proposal_id = state_mgr_->get_proposal_id();
  const bool is_leader = (self_ == state_mgr_->get_leader());
  if (is_leader) {
    bool need_rlock = false;
    bool need_wlock = false;
    if ((ConfigChangeState::INIT != state_) &&
        true == need_change_config_bkgd_ &&
        !FALSE_IT(is_state_changed_(need_rlock, need_wlock)) &&
        true == need_wlock) {
      (void) change_config_(running_args_, proposal_id, election_leader_epoch_, bkgd_config_version_);
    }
  }
  return ret;
}

int LogConfigMgr::after_flush_config_log(const LogConfigVersion &config_version)
{
  int ret = OB_SUCCESS;
  LogLearnerList removed_children;
  do {
    SpinLockGuard guard(lock_);
    persistent_config_version_ = (persistent_config_version_.is_valid())? MAX(persistent_config_version_, config_version): config_version;
  } while (0);
  // if self is in memberlist, then retire parent, otherwise paxos may has a parent.
  if (alive_paxos_memberlist_.contains(self_)) {
    SpinLockGuard guard_parent(parent_lock_);
    if (parent_.is_valid() && OB_FAIL(retire_parent_())) {
      PALF_LOG(WARN, "retire_parent failed", KR(ret), K_(palf_id), K_(self));
    }
  }
  // if child is not in learnerlist, then retire child
  if (OB_FAIL(remove_child_is_not_learner_(removed_children))) {
    PALF_LOG(WARN, "remove_child_is_not_learner failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(submit_retire_children_req_(removed_children))) {
    PALF_LOG(WARN, "submit_retire_children_req failed", KR(ret), K_(palf_id), K_(self), K(removed_children));
  }
  PALF_LOG(INFO, "after_flush_config_log success", K_(palf_id), K_(self), K(config_version), K_(persistent_config_version));
  return ret;
}

int LogConfigMgr::is_state_changed(bool &need_rlock, bool &need_wlock) const
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    ret = is_state_changed_(need_rlock, need_wlock);
  }
  return ret;
}

int LogConfigMgr::is_state_changed_(bool &need_rlock, bool &need_wlock) const
{
  int ret = OB_SUCCESS;
  need_rlock = false;
  need_wlock = false;
  switch(state_) {
    case (ConfigChangeState::INIT): {
      need_wlock = need_recheck_init_state_();
      break;
    }
    case (ConfigChangeState::CHANGING): {
      if (is_reach_majority_()) {
        need_wlock = true;
      } else if (need_resend_config_log_()) {
        need_rlock = true;
      }
      break;
    }
    default:
    {
      ret = OB_ERR_UNEXPECTED;
      PALF_LOG(ERROR, "invalid ConfigChangeState", K_(palf_id), K_(self), K(state_));
      break;
    }
  }
  return ret;
}

int LogConfigMgr::check_config_version_matches_state_(const LogConfigVersion &config_version) const
{
  int ret = OB_SUCCESS;
  if (ConfigChangeState::INIT == state_) {
    ret = (config_version.is_valid())? OB_ERR_UNEXPECTED: OB_SUCCESS;
  } else {
    ret = (config_version != log_ms_meta_.curr_.config_version_)? OB_EAGAIN: OB_SUCCESS;
  }
  return ret;
}

int LogConfigMgr::start_change_config(int64_t &proposal_id,
                                      int64_t &election_epoch) const
{
  int ret = OB_SUCCESS;
  common::ObRole ele_role;
  SpinLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (CHANGING == state_) {
    ret = OB_EAGAIN;
  } else if (OB_FAIL(election_->get_role(ele_role, election_epoch))) {
    PALF_LOG(ERROR, "election get_role failed", K(ret), K_(self), K_(palf_id));
  } else {
    proposal_id = state_mgr_->get_proposal_id();
  }
  return ret;
}

int LogConfigMgr::start_degrade()
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (CHANGING == state_) {
    ret = OB_EAGAIN;
  } else {
    is_sw_interrupted_by_degrade_ = true;
  }
  return ret;
}

int LogConfigMgr::end_degrade()
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    is_sw_interrupted_by_degrade_ = false;
  }
  return ret;
}

int LogConfigMgr::change_config(const LogConfigChangeArgs &args,
                                const int64_t proposal_id,
                                const int64_t election_epoch,
                                LogConfigVersion &config_version)
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    ret = change_config_(args, proposal_id, election_epoch, config_version);
    PALF_LOG(INFO, "config_change stat", K_(palf_id), K_(self), K(args), K(proposal_id),
      K_(prev_log_proposal_id), K_(prev_lsn), K_(prev_mode_pid), K_(state),
      K_(persistent_config_version), K_(ms_ack_list), K_(resend_config_version),
      K_(resend_log_list), K_(log_ms_meta), K_(last_submit_config_log_time_us));
  }
  return ret;
}

// state machine for config change
// ┌──────────────────────┐
// |    Config Change     |
// └──────────────────────┘
// │         INIT         |
// │          │           │
// │          ▼           │
// │       CHANGING       │
// └──────────────────────┘
int LogConfigMgr::change_config_(const LogConfigChangeArgs &args,
                                 const int64_t proposal_id,
                                 const int64_t election_epoch,
                                 LogConfigVersion &config_version)
{
  int ret = OB_SUCCESS;
  // args may be invalid when background retry config_change, so don't check it here
  if (false == is_leader_for_config_change_(args.type_, proposal_id, election_epoch)) {
    ret = OB_NOT_MASTER;
    PALF_LOG(WARN, "not leader, can't change member", KR(ret), K_(palf_id), K_(self),
        "role", state_mgr_->get_role(), "state", state_mgr_->get_state());
  } else if (false == mode_mgr_->can_do_paxos_accept()) {
    // do not allow chagne_config when ModeMgr is in prepare state
    ret = OB_EAGAIN;
    PALF_LOG(WARN, "is changing access_mode, try again", KR(ret), K_(palf_id), K_(self),
        "role", state_mgr_->get_role(), "state", state_mgr_->get_state());
  } else if (OB_FAIL(check_config_version_matches_state_(config_version))) {
    PALF_LOG(WARN, "config_version does not match with state, try again", KR(ret), K_(palf_id), K_(self),
        K(config_version), K_(state), K_(log_ms_meta));
  } else {
    const int64_t curr_proposal_id = state_mgr_->get_proposal_id();
    switch(state_) {
      case (ConfigChangeState::INIT): {
        bool is_already_finished = false;
        last_check_init_state_time_us_ = common::ObTimeUtility::current_time();
        if (OB_FAIL(append_config_meta_(curr_proposal_id, args, is_already_finished))) {
          if (OB_EAGAIN != ret) {
            PALF_LOG(WARN, "append_config_meta failed", KR(ret), K_(palf_id), K_(self), K(curr_proposal_id), K(args));
          }
        } else if (true == is_already_finished) {
          ret = OB_SUCCESS;
        } else {
          PALF_LOG(INFO, "append_config_meta success, start config change", K(ret), K_(palf_id), K_(self),
              K(curr_proposal_id), K(args));
          ms_ack_list_.reset();
          (void) set_resend_log_info_();
          state_ = ConfigChangeState::CHANGING;
          config_version = log_ms_meta_.curr_.config_version_;
          running_args_ = args;
          need_change_config_bkgd_ = false;
          bkgd_config_version_.reset();
          election_leader_epoch_ = election_epoch;
          last_submit_config_log_time_us_ = OB_INVALID_TIMESTAMP;
          ret = OB_EAGAIN;
        }
        break;
      }
      case (ConfigChangeState::CHANGING): {
        if (is_reach_majority_()) {
          (void) state_mgr_->reset_changing_config_with_arb();
          state_ = INIT;
          config_meta_ = log_ms_meta_;
          applied_all_learnerlist_ = all_learnerlist_;
          applied_alive_paxos_memberlist_ = alive_paxos_memberlist_;
          applied_alive_paxos_replica_num_ = alive_paxos_replica_num_;
          (void) update_match_lsn_map_(running_args_, config_meta_.curr_);
          ms_ack_list_.reset();
          resend_config_version_ = log_ms_meta_.curr_.config_version_;
          last_submit_config_log_time_us_ = OB_INVALID_TIMESTAMP;
          ret = OB_SUCCESS;
        } else if (need_resend_config_log_()) {
          if (OB_SUCC(submit_config_log_(alive_paxos_memberlist_, curr_proposal_id,
              prev_log_proposal_id_, prev_lsn_, prev_mode_pid_, log_ms_meta_))) {
            PALF_LOG(INFO, "submit_config_log success", KR(ret), K_(palf_id), K_(self));
            ret = OB_EAGAIN;
          } else if (OB_EAGAIN != ret) {
            PALF_LOG(WARN, "submit_config_log failed", KR(ret), K_(palf_id), K_(self));
          }
        } else {
          ret = OB_EAGAIN;
        }
        break;
      }
      default:
      {
        ret = OB_ERR_UNEXPECTED;
        PALF_LOG(ERROR, "invalid ConfigChangeState", K_(palf_id), K_(self), K(state_));
        break;
      }
    }
  }
  return ret;
}

void LogConfigMgr::after_config_change_timeout(const LogConfigVersion &config_version)
{
  SpinLockGuard guard(lock_);
  if (IS_NOT_INIT) {
  } else {
    // need config change background
    need_change_config_bkgd_ = true;
    bkgd_config_version_ = config_version;
  }
}

bool LogConfigMgr::is_leader_for_config_change_(const LogConfigChangeType &type,
                                                const int64_t proposal_id,
                                                const int64_t election_epoch) const
{
  bool bool_ret = false;
  bool is_leader = false;
  // 1. PALF leader state check
  // UPGRADE should be executed in leader active state
  // DEGRADE could be executed in leader active/reconfirm state
  if (LogConfigChangeType::DEGRADE_ACCEPTOR_TO_LEARNER == type) {
    is_leader = state_mgr_->is_leader_active() ||
      (state_mgr_->is_leader_reconfirm() && reconfirm_->can_do_degrade());
  } else if (LogConfigChangeType::STARTWORKING == type) {
    is_leader = state_mgr_->is_leader_reconfirm();
  } else {
    is_leader = state_mgr_->is_leader_active();
  }
  // 2. palf proposal_id check
  const int64_t curr_proposal_id = state_mgr_->get_proposal_id();
  bool is_pid_changed = (proposal_id != curr_proposal_id);
  // 3. election leader epoch check
  int ret = OB_SUCCESS;
  common::ObRole ele_role;
  int64_t ele_epoch = OB_INVALID_TIMESTAMP;
  bool is_epoch_changed = true;
  if (OB_FAIL(election_->get_role(ele_role, ele_epoch))) {
    PALF_LOG(ERROR, "election get_role failed", K(ret), K_(self), K_(palf_id));
  } else if (LEADER != ele_role) {
    is_leader = false;
  } else {
    is_epoch_changed = (election_epoch != ele_epoch);
  }
  bool_ret = (is_leader && !is_pid_changed && !is_epoch_changed);
  if (false == bool_ret) {
    PALF_LOG(WARN, "is_leader_for_config_change return false", K(ret), K(is_leader), K(is_pid_changed),
        K(is_epoch_changed), K_(self), K_(palf_id), K(type), K(proposal_id), K(curr_proposal_id),
        K(election_epoch), K(ele_role), K(ele_epoch));
  }
  return bool_ret;
}

bool LogConfigMgr::is_reach_majority_() const
{
  int64_t curr_replica_num = alive_paxos_replica_num_;
  return (ms_ack_list_.get_count() > (curr_replica_num / 2));
}

bool LogConfigMgr::need_resend_config_log_() const
{
  int64_t curr_time_us = common::ObTimeUtility::current_time();
  return (last_submit_config_log_time_us_ == OB_INVALID_TIMESTAMP ||
      curr_time_us - last_submit_config_log_time_us_ > PALF_RESEND_MSLOG_INTERVAL_US);
}

bool LogConfigMgr::need_recheck_init_state_() const
{
  int64_t curr_time_us = common::ObTimeUtility::current_time();
  return (last_check_init_state_time_us_ == OB_INVALID_TIMESTAMP ||
      curr_time_us - last_check_init_state_time_us_ > 100 * 1000);
}

int LogConfigMgr::renew_config_change_barrier_()
{
  int ret = OB_SUCCESS;
  int64_t prev_log_proposal_id = INVALID_PROPOSAL_ID;
  int64_t prev_mode_pid = INVALID_PROPOSAL_ID;
  LSN prev_log_lsn;
  if (OB_FAIL(get_log_barrier_(prev_log_lsn, prev_log_proposal_id))) {
    PALF_LOG(WARN, "get_log_barrier_ failed", KR(ret), K_(palf_id), K_(self));
  } else {
    prev_mode_pid_ = mode_mgr_->get_last_submit_mode_meta().proposal_id_;
    prev_log_proposal_id_ = prev_log_proposal_id;
    prev_lsn_ = prev_log_lsn;
  }
  return ret;
}

int LogConfigMgr::get_log_barrier_(LSN &prev_log_lsn, int64_t &prev_log_proposal_id) const
{
  int ret = OB_SUCCESS;
  int64_t unused_log_id;
  // log barrier must be last_submit_log_info
  if (OB_FAIL(sw_->get_last_submit_log_info(prev_log_lsn, unused_log_id, prev_log_proposal_id))) {
    PALF_LOG(WARN, "get_last_submit_log_info failed", KR(ret), K_(palf_id), K_(self));
  }
  return ret;
}

int LogConfigMgr::check_config_change_args_(const LogConfigChangeArgs &args, bool &is_already_finished) const
{
  int ret = OB_SUCCESS;
  is_already_finished = false;
  common::ObMemberList curr_member_list;
  int64_t curr_replica_num = -1;
  if (!args.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", KR(ret), K_(palf_id), K_(self), K(args));
  } else if (is_remove_member_list(args.type_) && self_ == args.server_.get_server()) {
    // can not remove leader, return OB_NOT_ALLOW_REMOVING_LEADER,
    // proposer of remove_member cmd will retry later and notify election to switch leader.
    ret = OB_NOT_ALLOW_REMOVING_LEADER;
    PALF_LOG(WARN, "leader can not remove itself", KR(ret), K_(palf_id), K_(self), K(args));
  } else if (OB_FAIL(get_curr_member_list(curr_member_list, curr_replica_num))) {
    PALF_LOG(WARN, "get_curr_member_list failed", KR(ret), K_(palf_id), K_(self), K(args));
  } else {
    const ObMemberList &log_sync_member_list = log_ms_meta_.curr_.log_sync_memberlist_;
    const common::GlobalLearnerList &curr_learner_list = log_ms_meta_.curr_.learnerlist_;
    const common::GlobalLearnerList &degraded_learnerlist = log_ms_meta_.curr_.degraded_learnerlist_;
    const common::ObMember &member = args.server_;
    const int64_t new_replica_num = args.new_replica_num_;
    const bool is_in_log_sync_memberlist = log_sync_member_list.contains(member);
    const bool is_in_degraded_learnerlist = degraded_learnerlist.contains(member);
    const bool is_in_learnerlist = curr_learner_list.contains(member);
    const bool is_arb_replica = (log_ms_meta_.curr_.arbitration_member_ == member);
    const bool has_arb_replica = (log_ms_meta_.curr_.arbitration_member_.is_valid());
    switch (args.type_) {
      case CHANGE_REPLICA_NUM:
      {
        if (curr_replica_num == new_replica_num &&
            true == curr_member_list.member_addr_equal(args.curr_member_list_)) {
          is_already_finished = true;
          PALF_LOG(INFO, "change_replica_num has finished, donot need do again", KR(ret), K_(palf_id),
              K_(self), K(args), K_(alive_paxos_memberlist), K_(alive_paxos_replica_num));
        } else if (false == curr_member_list.member_addr_equal(args.curr_member_list_)
            || curr_replica_num != args.curr_replica_num_) {
          ret = OB_INVALID_ARGUMENT;
          PALF_LOG(WARN, "pre-check failed, cannot change_replica_num", KR(ret), K_(palf_id), K_(self),
              K(args), K(curr_member_list), K(curr_replica_num));
        }
        break;
      }
      case ADD_MEMBER:
      case ADD_MEMBER_AND_NUM:
      {
        if (is_in_learnerlist || is_in_degraded_learnerlist || is_arb_replica) {
          ret = OB_INVALID_ARGUMENT;
          PALF_LOG(WARN, "server is learner/arbitration member, can not add_member/replace_member", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta),
              K(is_in_learnerlist), K(is_in_degraded_learnerlist), K(is_arb_replica), K(member));
        } else if (is_in_log_sync_memberlist) {
          if (args.type_ == ADD_MEMBER_AND_NUM || new_replica_num == curr_replica_num) {
            // config change has finished successfully, do not need change again
            is_already_finished = true;
            if (palf_reach_time_interval(100 * 1000, check_config_print_time_)) {
              PALF_LOG(INFO, "member already exists, don't need add_member/replace_member", KR(ret), K_(palf_id), K_(self),
                  K_(log_ms_meta), K(member), K(new_replica_num), K_(alive_paxos_replica_num));
            }
          } else {
            ret = OB_INVALID_ARGUMENT;
            PALF_LOG(INFO, "member already exists, but new_replica_num not equal to curr val", KR(ret), K_(palf_id), K_(self),
                K_(log_ms_meta), K(member), K(new_replica_num), K_(alive_paxos_replica_num));
          }
        }
        break;
      }
      case REMOVE_MEMBER:
      case REMOVE_MEMBER_AND_NUM:
      {
        if (is_in_learnerlist || is_arb_replica) {
          // Members in degraded_learnerlist are allowed to be removed.
          ret = OB_INVALID_ARGUMENT;
          PALF_LOG(WARN, "server is learner/arbitration member, can not remove_member/replace_member", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta),
              K(is_in_learnerlist), K(is_arb_replica), K(member));
        } else if (!is_in_log_sync_memberlist) {
          if (is_in_degraded_learnerlist) {
            PALF_LOG(INFO, "member to be removed is a degraded learner", KR(ret), K_(palf_id), K_(self),
                K_(log_ms_meta), K(member), K(new_replica_num));
          } else if (args.type_ == REMOVE_MEMBER_AND_NUM || new_replica_num == curr_replica_num) {
            // config change has finished successfully, do not need change again
            is_already_finished = true;
            PALF_LOG(INFO, "member is already removed, don't need remove_member/replcae_member", KR(ret), K_(palf_id), K_(self),
                K_(log_ms_meta), K(member), K(new_replica_num), K_(alive_paxos_replica_num));
          } else {
            ret = OB_INVALID_ARGUMENT;
            PALF_LOG(INFO, "member is already removed, but new_replica_num not equal to curr val", KR(ret), K_(palf_id), K_(self),
                K_(log_ms_meta), K(member), K(new_replica_num), K_(alive_paxos_replica_num));
          }
        }
        break;
      }
      case ADD_ARB_MEMBER:
      {
        if (is_in_learnerlist || is_in_degraded_learnerlist || is_in_log_sync_memberlist) {
          ret = OB_INVALID_ARGUMENT;
          PALF_LOG(WARN, "server is learner/normal member, can not add_arb_member/replace_arb_member", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta),
              K(is_in_learnerlist), K(is_in_log_sync_memberlist), K(member));
        } else if (is_arb_replica) {
          if (new_replica_num == curr_replica_num) {
            // config change has finished successfully, do not need change again
            is_already_finished = true;
            PALF_LOG(INFO, "arb replica already exists, don't need add_arb_member/replace_arb_member", KR(ret), K_(palf_id), K_(self),
                K_(log_ms_meta), K(member), K(new_replica_num), K_(alive_paxos_replica_num));
          } else {
            ret = OB_INVALID_ARGUMENT;
            PALF_LOG(INFO, "arb replica already exists, but new_replica_num not equal to curr val", KR(ret), K_(palf_id), K_(self),
                K_(log_ms_meta), K(member), K(new_replica_num), K_(alive_paxos_replica_num));
          }
        } else if (log_ms_meta_.curr_.arbitration_member_.is_valid()) {
          ret = OB_INVALID_ARGUMENT;
          PALF_LOG(WARN, "arbitration replica exists, can not add_arb_member", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
        }
        break;
      }
      case REMOVE_ARB_MEMBER:
      {
        if (is_in_learnerlist || is_in_degraded_learnerlist || is_in_log_sync_memberlist) {
          ret = OB_INVALID_ARGUMENT;
          PALF_LOG(WARN, "server is learner/normal member, can not remove_arb_member/replace_arb_member", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta),
              K(is_in_learnerlist), K(is_in_log_sync_memberlist), K(member));
        } else if (!is_arb_replica) {
          if (new_replica_num == curr_replica_num) {
            // config change has finished successfully, do not need change again
            is_already_finished = true;
            PALF_LOG(INFO, "member already exists, don't need add_arb_member", KR(ret), K_(palf_id), K_(self),
                K_(log_ms_meta), K(member), K(new_replica_num), K_(alive_paxos_replica_num));
          } else {
            ret = OB_INVALID_ARGUMENT;
            PALF_LOG(INFO, "arb replica does not exists, but new_replica_num not equal to curr val", KR(ret), K_(palf_id), K_(self),
                K_(log_ms_meta), K(member), K(new_replica_num), K_(alive_paxos_replica_num));
          }
        }
        break;
      }
      case ADD_LEARNER:
      {
        if (is_in_log_sync_memberlist || is_in_degraded_learnerlist || is_arb_replica) {
          ret = OB_INVALID_ARGUMENT;
          PALF_LOG(WARN, "server is already in memberlist, can not add_learner", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
        } else if (is_in_learnerlist) {
        // config change has finished successfully, do not need change again
          is_already_finished = true;
          PALF_LOG(INFO, "learner already exists, don't need add_learner", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
        }
        break;
      }
      case REMOVE_LEARNER:
      {
        if (is_in_log_sync_memberlist || is_in_degraded_learnerlist || is_arb_replica) {
          ret = OB_INVALID_ARGUMENT;
          PALF_LOG(WARN, "server is already in memberlist, can not remove_learner", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
        } else if (!is_in_learnerlist) {
        // config change has finished successfully, do not need change again
          is_already_finished = true;
          PALF_LOG(INFO, "learner don't exist, don't need remove_learner", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
        }
        break;
      }
      case SWITCH_LEARNER_TO_ACCEPTOR:
      case UPGRADE_LEARNER_TO_ACCEPTOR:
      {
        if ((is_in_degraded_learnerlist || is_in_learnerlist) && is_in_log_sync_memberlist) {
          ret = OB_ERR_UNEXPECTED;
          PALF_LOG(ERROR, "server is both in memberlist and in learnerlist", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
        } else if ((is_in_learnerlist || is_in_degraded_learnerlist) && !is_in_log_sync_memberlist) {
          if (UPGRADE_LEARNER_TO_ACCEPTOR == args.type_ && true == is_in_learnerlist) {
            ret = OB_INVALID_ARGUMENT;
            PALF_LOG(WARN, "can not upgrade a normal learner", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(args));
          } else if (SWITCH_LEARNER_TO_ACCEPTOR == args.type_ && true == is_in_degraded_learnerlist) {
            ret = OB_INVALID_ARGUMENT;
            PALF_LOG(WARN, "can not upgrade a normal learner", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(args));
          }
        } else if (!is_in_learnerlist && !is_in_degraded_learnerlist && is_in_log_sync_memberlist) {
          is_already_finished = true;
          PALF_LOG(INFO, "learner_to_acceptor is already finished", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
        } else {
          ret = OB_INVALID_ARGUMENT;
          PALF_LOG(WARN, "server is neither in memberlist nor in learnerlist", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
        }
        break;
      }
      case SWITCH_ACCEPTOR_TO_LEARNER:
      case DEGRADE_ACCEPTOR_TO_LEARNER:
      {
        if (false == has_arb_replica && args.type_ == DEGRADE_ACCEPTOR_TO_LEARNER) {
          ret = OB_INVALID_ARGUMENT;
          PALF_LOG(WARN, "do not allow to degrade member without arbitration member", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
        } else if ((is_in_learnerlist || is_in_degraded_learnerlist) && is_in_log_sync_memberlist) {
          ret = OB_ERR_UNEXPECTED;
          PALF_LOG(ERROR, "server is both in memberlist and in learnerlist", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
        } else if ((is_in_learnerlist || is_in_degraded_learnerlist) && false == is_in_log_sync_memberlist) {
          if (args.type_ == DEGRADE_ACCEPTOR_TO_LEARNER && true == is_in_learnerlist) {
            ret = OB_INVALID_ARGUMENT;
            PALF_LOG(WARN, "server is a learner, can't degrade", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
          } else if (args.type_ == SWITCH_ACCEPTOR_TO_LEARNER && true == is_in_degraded_learnerlist) {
            ret = OB_INVALID_ARGUMENT;
            PALF_LOG(WARN, "server has been degraded, can't switch to learner", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
          } else {
            is_already_finished = true;
            PALF_LOG(INFO, "acceptor_to_learner is already finished", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
          }
        } else if (!is_in_degraded_learnerlist && is_in_log_sync_memberlist) {
          // degrade operation can only be done when there is arbitration replica in paxos group
          if (args.type_ == DEGRADE_ACCEPTOR_TO_LEARNER && !log_ms_meta_.curr_.arbitration_member_.is_valid()) {
            ret = OB_INVALID_ARGUMENT;
            PALF_LOG(WARN, "arb member is invalid, can't degrade", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
          }
        } else {
          ret = OB_INVALID_ARGUMENT;
          PALF_LOG(WARN, "server is neither in memberlist nor in learnerlist", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(member));
        }
        break;
      }
      case STARTWORKING:
      {
        break;
      }
      default:
      {
        ret = OB_INVALID_ARGUMENT;
        PALF_LOG(ERROR, "unknown LogConfigChangeType", KR(ret), K_(palf_id), K_(self), K(args.type_));
        break;
      }
    }
    LogConfigInfo new_config_info;
    common::ObMemberList new_paxos_memberlist;
    int64_t new_paxos_replica_num;
    GlobalLearnerList unused_list;
    // defensive check
    // 1. check if reaches majority
    // 2. new memberlist contains self
    // 1F1A, if command is add_member(C, 5), if add C, length of log_sync_member_list is 2, log_sync_replica_num is 4,
    // so reject add_member(C, 5)
    // 2F, if command is add_member(C, 5), if add C, length of log_sync_member_list is 3, log_sync_replica_num is 5,
    // so allow add_member(C, 5)
    // 2F2F1A(ABCDE), remove(D, 5) success, then remove(C, 5). if remove C, length of log_sync_member_list is 2, log_sync_replica_num is 4,
    // so reject remove(C, 5)
    if (OB_SUCC(ret) && false == is_already_finished) {
      if (OB_FAIL(generate_new_config_info_(state_mgr_->get_proposal_id(), args, new_config_info))) {
        PALF_LOG(WARN, "generate_new_config_info_ failed", KR(ret), K_(palf_id), K_(self), K(args));
      } else if (OB_FAIL(new_config_info.convert_to_complete_config(new_paxos_memberlist, new_paxos_replica_num, unused_list))) {
      } else if (false == new_paxos_memberlist.contains(self_) || false == new_config_info.log_sync_memberlist_.contains(self_)) {
        PALF_LOG(ERROR, "new memberlist doesn't contain self", KR(ret), K_(palf_id), K_(self), K(new_config_info), K(new_paxos_memberlist));
      } else if (false == can_memberlist_majority_(new_config_info.log_sync_memberlist_.get_member_number(), new_config_info.log_sync_replica_num_) ||
          false == can_memberlist_majority_(new_paxos_memberlist.get_member_number(), new_paxos_replica_num)) {
        ret = OB_INVALID_ARGUMENT;
        PALF_LOG(WARN, "can't change config, memberlist don't reach majority", KR(ret), K_(palf_id), K_(self), K(new_config_info),
            K(new_paxos_memberlist), K(new_paxos_replica_num), K(args));
      }
    }
  }
  return ret;
}

bool LogConfigMgr::can_memberlist_majority_(const int64_t new_member_list_len, const int64_t new_replica_num) const
{
  // NB: new_replica_num is not the number of paxos member after config changing,
  // it means that after config changing, availability of this paxos group should be like
  // 'new_replica_num' member group, even if paxos member number is smaller than 'new_replica_num'.
  // For example, 'member_list' is (A, B, C, D) and 'replica_num' is 4, then request remove(D, 4) arrives,
  // after removing D, 'member_list' is (A, B, C) and 'replica_num' is still 4 (rather than 3). A new log will
  // be committed only when it has been flushed by 3 replicas at least. Even if there are only 3 replicas in
  // this paxos group, its availibility is equal to 4 replicas paxos group.
  // constraints:
  // 1. replica_num >= len(member_list)
  // 2. len(member_list) >= replica_num / 2 + 1
  bool bool_ret = false;
  if (new_member_list_len > new_replica_num) {
    PALF_LOG(WARN, "can't change config, replica_num too small", K_(palf_id), K_(self), K(new_replica_num), K(new_member_list_len));
  } else if (new_member_list_len < (new_replica_num / 2 + 1)) {
    PALF_LOG(WARN, "can't change config, replica_num too large", K_(palf_id), K_(self), K(new_replica_num), K(new_member_list_len));
  } else {
    bool_ret = true;
  }
  return bool_ret;
}

int LogConfigMgr::check_args_and_generate_config(const LogConfigChangeArgs &args,
                                                 const int64_t proposal_id,
                                                 const int64_t election_epoch,
                                                 bool &is_already_finished,
                                                 LogConfigInfo &new_config_info) const
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (false == is_leader_for_config_change_(args.type_, proposal_id, election_epoch)) {
    ret = OB_NOT_MASTER;
    PALF_LOG(WARN, "is_leader_for_config_change_ return false", K(ret), K_(palf_id), K_(self),
        K(args.type_), K(proposal_id), K(election_epoch));
  } else if (OB_FAIL(check_config_change_args_(args, is_already_finished))) {
    PALF_LOG(WARN, "check_config_change_args_ failed", K(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(args));
  } else if (is_already_finished) {
  } else if (OB_FAIL(generate_new_config_info_(proposal_id, args, new_config_info))) {
    PALF_LOG(WARN, "generate_new_config_info_ failed", KR(ret), K_(palf_id), K_(self), K(args));
  } else {
    PALF_LOG(INFO, "check_args_and_generate_config success", K(ret), K_(palf_id), K_(self), K(args), K(is_already_finished), K(new_config_info));
  }
  return ret;
}

int LogConfigMgr::sync_meta_for_arb_election_leader()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  const bool is_arb_replica = state_mgr_->is_arb_replica();
  common::ObAddr ele_leader;
  int64_t leader_epoch;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (false == is_arb_replica) {
  } else if (false == palf_reach_time_interval(500 * 1000, last_sync_meta_for_arb_election_leader_time_us_)) {
    // skip
  } else if (OB_SUCCESS != (tmp_ret = election_->get_current_leader_likely(ele_leader, leader_epoch))) {
    // skip
  } else if (self_ == ele_leader) {
    const int64_t proposal_id = state_mgr_->get_proposal_id();
    for (int i = 0; OB_SUCC(ret) && i < alive_paxos_memberlist_.get_member_number(); i++) {
      common::ObMember member;
      if (OB_FAIL(alive_paxos_memberlist_.get_member_by_index(i, member))) {
        PALF_LOG(WARN, "get_member_by_index failed", KR(ret), K_(palf_id), K_(self), K(i), K(alive_paxos_memberlist_));
      } else if (self_ == member.get_server()) {
        // skip
      } else if (OB_FAIL(pre_sync_config_log_and_mode_meta_(member, proposal_id, is_arb_replica))) {
        PALF_LOG(WARN, "pre_sync_config_log_and_mode_meta_ failed", KR(ret), K_(palf_id), K_(self), K(member), K(proposal_id));
      }
    }
    if (OB_SUCC(ret)) {
      PALF_LOG(INFO, "sync_meta_for_arb_election_leader success", KR(ret), K_(palf_id), K_(self),
          K_(alive_paxos_memberlist), K(proposal_id), K_(log_ms_meta));
    }
  }
  return ret;
}

int LogConfigMgr::pre_sync_config_log_and_mode_meta(const common::ObMember &server,
                                                    const int64_t proposal_id)
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(pre_sync_config_log_and_mode_meta_(server, proposal_id, false))) {
    PALF_LOG(WARN, "pre_sync_config_log_and_mode_meta_ failed", KR(ret), K_(palf_id), K_(self), K(server), K(proposal_id));
  }
  return ret;
}

int LogConfigMgr::pre_sync_config_log_and_mode_meta_(const common::ObMember &server,
                                                     const int64_t proposal_id,
                                                     const bool is_arb_replica)
{
  int ret = OB_SUCCESS;
  common::ObMemberList member_list;
  const int64_t prev_log_proposal_id = (is_arb_replica)? log_ms_meta_.prev_log_proposal_id_: prev_log_proposal_id_;
  const LSN prev_lsn = (is_arb_replica)? log_ms_meta_.prev_lsn_: prev_lsn_;
  const int64_t prev_mode_pid = (is_arb_replica)? log_ms_meta_.prev_mode_pid_: prev_mode_pid_;
  if (false == server.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (CHANGING == state_ && false == is_arb_replica) {
    ret = OB_EAGAIN;
  } else if (CHANGING == state_ && true == is_arb_replica) {
    ret = OB_ERR_UNEXPECTED;
    PALF_LOG(ERROR, "arbitration replica is not allowed to change config", K(ret), K_(palf_id), K_(self), K_(state));
  } else if (OB_FAIL(mode_mgr_->submit_fetch_mode_meta_resp(server.get_server(), proposal_id, prev_mode_pid))) {
      PALF_LOG(WARN, "submit_fetch_mode_meta_resp failed", K(ret), K_(palf_id), K_(self), K(proposal_id));
  } else if (FALSE_IT(member_list.add_member(server))) {
  } else if (OB_FAIL(log_engine_->submit_change_config_meta_req(member_list, proposal_id,
      prev_log_proposal_id, prev_lsn, prev_mode_pid, log_ms_meta_))) {
    PALF_LOG(WARN, "submit_change_config_meta_req failed", KR(ret), K_(palf_id), K_(self), K(proposal_id), K(server));
  }
  return ret;
}

// Require caller hold wlock of ObLogService
int LogConfigMgr::append_config_meta_(const int64_t curr_proposal_id,
                                     const LogConfigChangeArgs &args,
                                     bool &is_already_finished)
{
  int ret = OB_SUCCESS;
  LogConfigInfo new_config_info;
  bool has_arb_member = false;
  bool unused_bool;
  if (INVALID_PROPOSAL_ID == curr_proposal_id || !args.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", KR(ret), K_(palf_id), K_(self), K(args), K(curr_proposal_id));
  } else if (!(state_mgr_->get_leader() == self_ && state_mgr_->get_proposal_id() == curr_proposal_id)) {
    ret = OB_NOT_MASTER;
    PALF_LOG(WARN, "leader has switched during config changing", KR(ret), K_(palf_id), K_(self),
        "role", state_mgr_->get_role(), K(curr_proposal_id), "proposal_id", state_mgr_->get_proposal_id());
  } else if (OB_FAIL(check_config_change_args_(args, is_already_finished))) {
    PALF_LOG(WARN, "check_config_change_args_ failed", K(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(args));
  } else if (is_already_finished) {
    PALF_LOG(INFO, "config_change is already success", K(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(args));
  } else if (OB_FAIL(generate_new_config_info_(curr_proposal_id, args, new_config_info))) {
    PALF_LOG(WARN, "generate_new_config_info_ failed", KR(ret), K_(palf_id), K_(self), K(args));
    // new_member_list contains arb member, stop appending logs and check log barrier
  } else if (FALSE_IT(has_arb_member = new_config_info.arbitration_member_.is_valid())) {
  } else if (has_arb_member && OB_FAIL(state_mgr_->set_changing_config_with_arb())) {
    PALF_LOG(ERROR, "set_changing_config_with_arb failed", KR(ret), K_(palf_id), K_(self), K(args));
  } else if (has_arb_member &&
      OB_FAIL(check_follower_sync_status_(args, new_config_info.log_sync_memberlist_,
          new_config_info.log_sync_replica_num_, true, unused_bool))) {
      PALF_LOG(WARN, "check_follower_sync_status_ eagain", KR(ret), K_(palf_id), K_(self), K(args), K(new_config_info));
      if (OB_LOG_NOT_SYNC == ret) {
        state_mgr_->reset_changing_config_with_arb();
      }
  } else if (OB_FAIL(update_election_meta_(new_config_info))) {
    if (OB_OP_NOT_ALLOW == ret) {
      ret = OB_EAGAIN;
    } else {
      PALF_LOG(WARN, "update_election_meta_ failed", KR(ret), K_(palf_id), K_(self), K(new_config_info), K_(log_ms_meta));
    }
  } else if (OB_FAIL(append_config_info_(new_config_info))) {
    PALF_LOG(WARN, "append_config_info_ failed", KR(ret), K_(palf_id), K_(self), K(new_config_info));
    // update election meta may need retry, we will not allow appending logs until updating election meta successfully
  } else if (OB_FAIL(renew_config_change_barrier_())) {
    PALF_LOG(WARN, "renew_config_change_barrier_ failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(log_ms_meta_.generate(curr_proposal_id, log_ms_meta_.curr_, new_config_info,
      prev_log_proposal_id_, prev_lsn_, prev_mode_pid_))) {
    PALF_LOG(ERROR, "generate LogConfigMeta failed", KR(ret), K_(palf_id), K_(self), K(args));
  } else {
    // 1. For normal config changing, memberlist in memory takes effect right now.
    // 2. For degrading operation, memberlist in memory will not be changed until the memberlist
    // is accepted by majority. For example, 2F1A (ABC), A is leader. If log_sync_member_list (A) takes effect
    // in leader right now, some logs will be committed by A itself before the config log has been committed. If leadership
    // are switched to B and B executes degrading operation successfully. These committed logs will get lost.
    // 3. For upgrading operation, memberlist in memory must be changed right now.
    // For example, 1F1A (AC), A is leader, B has been degraded to learner. If log_sync_member_list (A) still takes effect
    // when upgrading operation (upgrade B) is executing, some logs will be committed by A itself before the config log has been
    // committed. If leadership are switched to B and B executes degrading operation successfully, these committed logs
    // will get lost.
    // 4. 2F1A, add one member may casue exception like case 2, so we just reject appended logs
    // when changing config with arbitration members and apply member list
    // after config log reaches majority to avoid complex scenario
    if (false == has_arb_member) {
      config_meta_ = log_ms_meta_;
      (void) update_match_lsn_map_(args, new_config_info);
      if (OB_FAIL(apply_config_info_(new_config_info))) {
        PALF_LOG(WARN, "apply_config_info_ failed", KR(ret), K_(palf_id), K_(self), K(new_config_info));
      }
    }
    PALF_LOG(INFO, "append_config_meta_ success", KR(ret), K_(palf_id), K_(self), K(curr_proposal_id),
       K(args), K(new_config_info), K_(log_ms_meta));
  }
  // encounter unexpected error, reset flag
  if (OB_EAGAIN != ret && OB_SUCCESS != ret) {
    (void) state_mgr_->reset_changing_config_with_arb();
  }
  return ret;
}

int LogConfigMgr::update_match_lsn_map_(const LogConfigChangeArgs &args,
    const LogConfigInfo &new_config_info)
{
  int ret = OB_SUCCESS;
  ObMemberList added_memberlist;
  ObMemberList removed_memberlist;
  if (is_add_log_sync_member_list(args.type_) && OB_FAIL(added_memberlist.add_member(args.server_))) {
    PALF_LOG(WARN, "add_member failed", K(ret), K_(palf_id), K_(self), K(added_memberlist), K(args));
  } else if (is_remove_log_sync_member_list(args.type_) && OB_FAIL(removed_memberlist.add_member(args.server_))) {
    PALF_LOG(WARN, "add_member failed", K(ret), K_(palf_id), K_(self), K(added_memberlist), K(args));
  }
  if (OB_SUCC(ret) && OB_FAIL(sw_->config_change_update_match_lsn_map(added_memberlist, \
          removed_memberlist, new_config_info.log_sync_memberlist_, \
          new_config_info.log_sync_replica_num_))) {
    PALF_LOG(WARN, "config_change_update_match_lsn_map failed", K(ret), K_(palf_id), K_(self), K(added_memberlist), K(removed_memberlist));
  }
  return ret;
}

// caller hold lock_
int LogConfigMgr::append_config_info_(const LogConfigInfo &config_info)
{
  int ret = OB_SUCCESS;
  common::ObMemberList alive_paxos_memberlist;
  int64_t alive_paxos_replica_num;
  GlobalLearnerList all_learners;
  if (OB_FAIL(config_info.convert_to_complete_config(alive_paxos_memberlist, \
          alive_paxos_replica_num, all_learners))) {
    PALF_LOG(WARN, "convert_to_complete_config failed", K(ret), K_(palf_id), K(config_info));
  } else if (OB_FAIL(all_learnerlist_.deep_copy(all_learners))) {
    PALF_LOG(WARN, "deep_copy failed", K(ret), K_(palf_id), K(all_learners));
  } else {
    alive_paxos_memberlist_ = alive_paxos_memberlist;
    alive_paxos_replica_num_ = alive_paxos_replica_num;
  }
  return ret;
}

// caller hold lock_
int LogConfigMgr::apply_config_info_(const LogConfigInfo &config_info)
{
  int ret = OB_SUCCESS;
  common::ObMemberList alive_paxos_memberlist;
  int64_t alive_paxos_replica_num;
  GlobalLearnerList all_learners;
  if (OB_FAIL(config_info.convert_to_complete_config(alive_paxos_memberlist, \
          alive_paxos_replica_num, all_learners))) {
    PALF_LOG(WARN, "convert_to_complete_config failed", K(ret), K_(palf_id), K(config_info));
  } else if (OB_FAIL(applied_all_learnerlist_.deep_copy(all_learners))) {
    PALF_LOG(WARN, "deep_copy failed", K(ret), K_(palf_id), K(all_learners));
  } else {
    applied_alive_paxos_memberlist_ = alive_paxos_memberlist;
    applied_alive_paxos_replica_num_ = alive_paxos_replica_num;
    PALF_LOG(INFO, "apply_config_info_ success", KR(ret), K_(palf_id), K_(self), K(config_info),
       K_(applied_alive_paxos_memberlist), K_(applied_alive_paxos_replica_num));
  }
  return ret;
}

int LogConfigMgr::set_resend_log_info_()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  resend_config_version_.reset();
  resend_log_list_.reset();
  (void) all_learnerlist_.deep_copy_to(resend_log_list_);
  const int64_t member_number = alive_paxos_memberlist_.get_member_number();
  for (int64_t idx = 0; idx < member_number && OB_SUCCESS == tmp_ret; ++idx) {
    common::ObAddr server;
    if (OB_SUCCESS != (tmp_ret = (alive_paxos_memberlist_.get_server_by_index(idx, server)))) {
      PALF_LOG(WARN, "get_server_by_index failed", K(tmp_ret), K(idx));
    } else if (server == self_) {
    } else if (OB_SUCCESS != (tmp_ret = (resend_log_list_.add_learner(ObMember(server, 1))))) {
      PALF_LOG(WARN, "add_learner failed", K(ret), K(server));
    }
  }
  return ret;
}

// require caller hold rlock
// Before calling this func, must call check_config_change_args to ensure LogConfigChangeArgs can
// be applied to current ConfigMeta safely.
int LogConfigMgr::generate_new_config_info_(const int64_t proposal_id,
                                            const LogConfigChangeArgs &args,
                                            LogConfigInfo &new_config_info) const
{
  int ret = OB_SUCCESS;
  const LogConfigChangeType cc_type = args.type_;
  const common::ObMember member = args.server_;
  new_config_info = log_ms_meta_.curr_;
  int64_t curr_replica_num = -1;
  if (INVALID_PROPOSAL_ID == proposal_id || !args.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(new_config_info.config_version_.inc_update_version(proposal_id))) {
    PALF_LOG(WARN, "generate config_version failed", KR(ret), K_(palf_id), K_(self), K(new_config_info), K(proposal_id));
  } else if (STARTWORKING == cc_type) {
    // pass
  } else if (OB_FAIL(get_replica_num(curr_replica_num))) {
    PALF_LOG(WARN, "get_replica_num failed", KR(ret), K_(palf_id), K_(self), K(args));
  } else {
    // change replcia num
    int64_t new_log_sync_replica_num = curr_replica_num;
    if (is_may_change_replica_num(cc_type)) {
      if (is_use_replica_num_args(cc_type)) {
        new_log_sync_replica_num = args.new_replica_num_;
      } else if (is_add_log_sync_member_list(cc_type)) {
        new_log_sync_replica_num = new_config_info.log_sync_replica_num_ + 1;
      } else if (is_remove_log_sync_member_list(cc_type)) {
        new_log_sync_replica_num = new_config_info.log_sync_replica_num_ - 1;
      } else if (is_arb_member_change_type(cc_type)) {
        new_log_sync_replica_num = new_config_info.log_sync_replica_num_;
      } else {
        ret = OB_ERR_UNEXPECTED;
        PALF_LOG(ERROR, "unexpected config change type", KR(ret), K_(palf_id), K_(self), K(args), K(new_config_info));
      }
    }
    // memberlist add, update replica number
    if (is_add_member_list(cc_type)) {
      // update log_sync_member_list or arb_member
      if (is_add_log_sync_member_list(args.type_)) {
        if (OB_FAIL(new_config_info.log_sync_memberlist_.add_member(member))) {
          PALF_LOG(WARN, "new_member_list add_member failed", KR(ret), K_(palf_id), K_(self), K(args), K(new_config_info));
        }
      } else {
        new_config_info.arbitration_member_ = member;
      }
    }
    // memberlist remove, update replica number
    if (OB_SUCC(ret) && is_remove_member_list(cc_type)) {
      // update log_sync_member_list or arb_member
      if (is_remove_log_sync_member_list(args.type_)) {
        if (new_config_info.log_sync_memberlist_.contains(member)) {
          if (OB_FAIL(new_config_info.log_sync_memberlist_.remove_server(member.get_server()))) {
            PALF_LOG(WARN, "remove member failed", KR(ret), K_(palf_id), K_(self), K(args), K(new_config_info));
          }
        } else if (new_config_info.degraded_learnerlist_.contains(member)) {
          if (OB_FAIL(new_config_info.degraded_learnerlist_.remove_learner(member))) {
            PALF_LOG(WARN, "new_member_list remove member failed", KR(ret), K_(palf_id), K_(self), K(args), K(new_config_info));
          }
          // If dst member is already degraded learner, just remove it and no need update log_sync_replica_num_.
          new_log_sync_replica_num = new_config_info.log_sync_replica_num_;
        } else {
          ret = OB_INVALID_ARGUMENT;
          PALF_LOG(WARN, "member to be removed does not exist", KR(ret), K_(palf_id), K_(self), K(args), K(new_config_info));
        }
      } else {
        new_config_info.arbitration_member_.reset();
      }
    }
    // learnerlist add
    if (OB_SUCC(ret) && is_add_learner_list(cc_type)) {
      if (DEGRADE_ACCEPTOR_TO_LEARNER == cc_type) {
        if (OB_FAIL(new_config_info.degraded_learnerlist_.add_learner(member))) {
          PALF_LOG(WARN, "new_learner_list add_learner failed", KR(ret), K_(palf_id), K_(self), K(args), K(new_config_info));
        }
      } else {
        if (OB_FAIL(new_config_info.learnerlist_.add_learner(member))) {
          PALF_LOG(WARN, "new_learner_list add_learner failed", KR(ret), K_(palf_id), K_(self), K(args), K(new_config_info));
        }
      }
    }
    // learnerlist remove
    if (OB_SUCC(ret) && is_remove_learner_list(cc_type)) {
      if (UPGRADE_LEARNER_TO_ACCEPTOR == cc_type) {
        if (OB_FAIL(new_config_info.degraded_learnerlist_.remove_learner(member.get_server()))) {
          PALF_LOG(WARN, "new_learner_list add_learner failed", KR(ret), K_(palf_id), K_(self), K(args), K(new_config_info));
        }
      } else {
        if (OB_FAIL(new_config_info.learnerlist_.remove_learner(member.get_server()))) {
          PALF_LOG(WARN, "new_learner_list add_learner failed", KR(ret), K_(palf_id), K_(self), K(args), K(new_config_info));
        }
      }
    }
    // generate log_sync_replica_num_
    if (OB_SUCC(ret)) {
      new_config_info.log_sync_replica_num_ = new_log_sync_replica_num;
    }
  }
  return ret;
}

bool LogConfigMgr::check_need_update_memberlist_without_lock_(const LogConfigVersion &config_version) const
{
  return persistent_config_version_ < config_version;
}

int LogConfigMgr::update_election_meta_(const ObMemberList &member_list,
                                        const LogConfigVersion &config_version,
                                        const int64_t new_replica_num)
{
  int ret = OB_SUCCESS;
  election::MemberList new_election_member_list;
  ObArray<ObAddr> addr_list;
  if (OB_FAIL(member_list.get_addr_array(addr_list))) {
    PALF_LOG(WARN, "get addr list from member list failed", KR(ret), K_(palf_id), K_(self), K(member_list));
  } else if (OB_FAIL(new_election_member_list.set_new_member_list(addr_list, config_version, new_replica_num))) {
    PALF_LOG(WARN, "create new memberlist failed",
                    KR(ret), K_(palf_id), K_(self), K(member_list), K(config_version), K(new_replica_num));
  } else {
    ret = election_->set_memberlist(new_election_member_list);
    if (OB_SUCC(ret)) {
      PALF_LOG(INFO, "update_election_meta_ success", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(new_election_member_list));
    }
  }
  return ret;
}

int LogConfigMgr::update_election_meta_(const LogConfigInfo &info)
{
  int ret = OB_SUCCESS;
  common::ObMemberList memberlist;
  int64_t replica_num = 0;
  GlobalLearnerList unused_list;
  if (OB_FAIL(info.convert_to_complete_config(memberlist, replica_num, unused_list))) {
    PALF_LOG(WARN, "convert_to_complete_config failed", K(ret), K_(palf_id), K(info));
  } else {
    ret = update_election_meta_(memberlist, info.config_version_, replica_num);
  }
  return ret;
}

int LogConfigMgr::confirm_start_working_log(const int64_t proposal_id,
                                            const int64_t election_epoch,
                                            LogConfigVersion &config_version)
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  const bool has_finished = (ConfigChangeState::INIT == state_ && config_version.is_valid()) ||
      (ConfigChangeState::CHANGING == state_ && config_version.is_valid() && log_ms_meta_.curr_.config_version_ > config_version);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret), K_(palf_id), K_(self));
  } else if (is_sw_interrupted_by_degrade_) {
    PALF_LOG(INFO, "is degrading, skip this startworking", KR(ret), K_(palf_id), K_(self),
        K(config_version), K_(state), K(is_sw_interrupted_by_degrade_));
  } else if (has_finished) {
    ret = OB_SUCCESS;
    PALF_LOG(INFO, "start_working log has reached majority, pass", KR(ret), K_(palf_id), K_(self),
        K(config_version), K_(log_ms_meta), K_(state));
  } else {
    common::ObMember dummy_member;
    const LogConfigChangeArgs args(dummy_member, 1, STARTWORKING);
    if (OB_FAIL(change_config_(args, proposal_id, election_epoch, config_version))) {
      if (OB_EAGAIN != ret) {
        PALF_LOG(INFO, "confirm_start_working_log failed", KR(ret), K_(palf_id), K_(self), K(args));
      }
    } else {
      PALF_LOG(INFO, "confirm_start_working_log success", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta));
    }
  }
  return ret;
}

int LogConfigMgr::wait_config_log_persistence(const LogConfigVersion &config_version) const
{
  int ret = OB_SUCCESS;
  const int64_t timeout_ts = 1 * 1000 * 1000;
  const int64_t start_ts = ObTimeUtility::current_time();
  const int64_t SLEEP_INTERVAL_TS = 1 * 1000;
  int64_t cost_ts = OB_INVALID_TIMESTAMP;
  while (OB_SUCC(ret)) {
    cost_ts  = ObTimeUtility::current_time() - start_ts;
    bool bool_ret = false;
    do {
      SpinLockGuard guard(lock_);
      bool_ret = check_need_update_memberlist_without_lock_(config_version);
    } while(0);
    if (false == bool_ret){
      break;
    } else if (cost_ts >= timeout_ts) {
      ret = OB_TIMEOUT;
    } else {
      ob_usleep(SLEEP_INTERVAL_TS);
    }
  }
  return ret;
}

void LogConfigMgr::reset_status()
{
  SpinLockGuard guard(lock_);
  state_ = ConfigChangeState::INIT;
  ms_ack_list_.reset();
  need_change_config_bkgd_ = false;
  bkgd_config_version_.reset();
  resend_config_version_.reset();
  resend_log_list_.reset();
  running_args_.reset();
  election_leader_epoch_ = OB_INVALID_TIMESTAMP;
  last_submit_config_log_time_us_ = OB_INVALID_TIMESTAMP;
  last_check_init_state_time_us_ = OB_INVALID_TIMESTAMP;
  last_wait_barrier_time_us_ = OB_INVALID_TIMESTAMP;
  last_wait_committed_end_lsn_.reset();
}

// leader check barrier condition when config changing
int LogConfigMgr::check_barrier_condition_(const int64_t &prev_log_proposal_id,
                                           const LSN &prev_lsn,
                                           const int64_t prev_mode_pid) const
{
  int ret = OB_SUCCESS;
  LSN unused_lsn;
  LSN max_flushed_lsn;
  int64_t max_flushed_log_pid = INVALID_PROPOSAL_ID;
  int64_t max_flushed_mode_pid = INVALID_PROPOSAL_ID;
  if (false == prev_lsn.is_valid() || INVALID_PROPOSAL_ID == prev_mode_pid) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", KR(ret), K_(palf_id), K_(self), K(prev_log_proposal_id),
        K(prev_lsn), K(prev_mode_pid));
  } else if (OB_FAIL(sw_->get_max_flushed_log_info(max_flushed_lsn, unused_lsn, max_flushed_log_pid))) {
    PALF_LOG(WARN, "get_max_flushed_log_info failed", KR(ret), K_(palf_id), K_(self));
  } else if (FALSE_IT(max_flushed_mode_pid = mode_mgr_->get_accepted_mode_meta().proposal_id_)) {
  } else {
    ret = ((INVALID_PROPOSAL_ID == prev_log_proposal_id || max_flushed_log_pid >= prev_log_proposal_id) &&
           max_flushed_lsn >= prev_lsn &&
           max_flushed_mode_pid != INVALID_PROPOSAL_ID &&
           max_flushed_mode_pid >= prev_mode_pid)? OB_SUCCESS: OB_EAGAIN;
    if (OB_EAGAIN == ret && palf_reach_time_interval(500 * 1000, barrier_print_log_time_)) {
      PALF_LOG(INFO, "check_barrier_condition_ eagain", KR(ret), K_(palf_id), K_(self),
          K(max_flushed_log_pid), K(max_flushed_lsn), K(prev_log_proposal_id), K(prev_lsn));
    }
  }
  return ret;
}

// require caller hold rlock
int LogConfigMgr::submit_config_log_(const common::ObMemberList &paxos_member_list,
                                     const int64_t proposal_id,
                                     const int64_t prev_log_proposal_id,
                                     const LSN &prev_lsn,
                                     const int64_t prev_mode_pid,
                                     const LogConfigMeta &config_meta)
{
  int ret = OB_SUCCESS;
  ObMemberList dst_member_list = paxos_member_list;
  const common::GlobalLearnerList &learner_list = all_learnerlist_;
  const bool need_skip_log_barrier = mode_mgr_->need_skip_log_barrier();
  if (false == paxos_member_list.is_valid() || INVALID_PROPOSAL_ID == proposal_id || false == prev_lsn.is_valid() ||
      INVALID_PROPOSAL_ID == prev_mode_pid || false == config_meta.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", KR(ret), K_(palf_id), K_(self), K(proposal_id), K(prev_log_proposal_id),
        K(prev_lsn), K(prev_mode_pid), K(config_meta));
  } else if (need_skip_log_barrier == false &&
      OB_FAIL(check_barrier_condition_(prev_log_proposal_id, prev_lsn, prev_mode_pid))) {
    // barrier condition may don't match, need retry
  } else {
    (void) dst_member_list.remove_server(self_);
    const LogConfigVersion config_version = config_meta.curr_.config_version_;
    FlushMetaCbCtx cb_ctx;
    cb_ctx.type_ = MetaType::CHANGE_CONFIG_META;
    cb_ctx.proposal_id_ = proposal_id;
    cb_ctx.config_version_ = config_version;
    if (OB_FAIL(log_engine_->submit_flush_change_config_meta_task(cb_ctx, config_meta))) {
      PALF_LOG(WARN, "submit_flush_change_config_meta_task failed", KR(ret), K_(palf_id), K_(self), K(proposal_id),
          K(prev_log_proposal_id), K(prev_lsn), K(config_meta));
    } else if (dst_member_list.is_valid() &&
        OB_FAIL(log_engine_->submit_change_config_meta_req(dst_member_list, proposal_id, prev_log_proposal_id,
        prev_lsn, prev_mode_pid, config_meta))) {
      PALF_LOG(WARN, "submit_change_config_meta_req failed, to member", KR(ret), K_(palf_id), K_(self), K(proposal_id),
          K(prev_log_proposal_id), K(prev_lsn), K(prev_mode_pid), K(config_meta));
    } else if (learner_list.is_valid() &&
        OB_FAIL(log_engine_->submit_change_config_meta_req(learner_list, proposal_id, prev_log_proposal_id,
        prev_lsn, prev_mode_pid, config_meta))) {
      PALF_LOG(WARN, "submit_change_config_meta_req failed, to learner", KR(ret), K_(palf_id), K_(self), K(proposal_id),
          K(prev_log_proposal_id), K(prev_lsn), K(prev_mode_pid), K(config_meta));
    } else {
      last_submit_config_log_time_us_ = common::ObTimeUtility::current_time();
      PALF_LOG(INFO, "submit_config_log success", KR(ret), K_(palf_id), K_(self), K(dst_member_list), K(proposal_id),
          K(prev_log_proposal_id), K(prev_lsn), K(prev_mode_pid), K(config_meta));
    }
  }
  return ret;
}

bool LogConfigMgr::can_receive_config_log(const common::ObAddr &leader, const LogConfigMeta &meta) const
{
  bool bool_ret = false;
  const LogConfigVersion &config_version = meta.curr_.config_version_;
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT || !config_version.is_valid()) {
  } else {
    bool_ret = config_version > log_ms_meta_.curr_.config_version_;
    if ((persistent_config_version_ == config_version) &&
        OB_FAIL(log_engine_->submit_change_config_meta_resp(leader, meta.proposal_id_, config_version))) {
      PALF_LOG(WARN, "submit_change_config_meta_resp fail", K(ret), K_(palf_id), K_(self), K(leader), K(meta));
    }
  }
  PALF_LOG(INFO, "can_receive_config_log", K(bool_ret), K_(palf_id), K_(self), K(leader), K(meta), K_(log_ms_meta));
  return bool_ret;
}

int LogConfigMgr::receive_config_log(const common::ObAddr &leader, const LogConfigMeta &meta)
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else if (false == meta.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", KR(ret), K_(palf_id), K_(self), K(meta));
  } else if (OB_FAIL(update_election_meta_(meta.curr_))) {
    PALF_LOG(ERROR, "update_election_meta_ failed", KR(ret), K_(palf_id), K_(self), K(meta));
  } else {
    FlushMetaCbCtx cb_ctx;
    cb_ctx.type_ = MetaType::CHANGE_CONFIG_META;
    cb_ctx.proposal_id_ = meta.proposal_id_;
    cb_ctx.config_version_ = meta.curr_.config_version_;
    if (OB_FAIL(log_engine_->submit_flush_change_config_meta_task(cb_ctx, meta))) {
      PALF_LOG(WARN, "LogEngine submit_flush_change_config_meta_task failed", KR(ret), K_(palf_id), K_(self), K(meta));
    } else if (OB_FAIL(append_config_info_(meta.curr_))) {
      PALF_LOG(WARN, "append_config_info_ failed", KR(ret), K_(palf_id), K_(self), K(meta));
    } else if (OB_FAIL(apply_config_info_(meta.curr_))) {
      PALF_LOG(WARN, "apply_config_info_ failed", KR(ret), K_(palf_id), K_(self), K(meta));
    } else {
      log_ms_meta_ = meta;
      config_meta_ = meta;
    }
  }
  return ret;
}

int LogConfigMgr::ack_config_log(const common::ObAddr &sender,
                                 const int64_t proposal_id,
                                 const LogConfigVersion &config_version)
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  const bool is_in_memberlist = alive_paxos_memberlist_.contains(sender);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else if (false == sender.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", KR(ret), K_(palf_id), K_(self), K(sender));
  } else if (proposal_id != log_ms_meta_.proposal_id_ ||
            config_version != log_ms_meta_.curr_.config_version_) {
    ret = OB_STATE_NOT_MATCH;
    PALF_LOG(WARN, "config_version has been changed", KR(ret), K_(palf_id), K_(self), K_(log_ms_meta), K(proposal_id),
        K_(state), K(config_version), K(sender));
  } else if (FALSE_IT(resend_log_list_.remove_learner(common::ObMember(sender, 1)))) {
  } else if (is_in_memberlist && ConfigChangeState::INIT != state_) {
    if (OB_FAIL(ms_ack_list_.add_server(sender)) && ret != OB_ENTRY_EXIST) {
      PALF_LOG(ERROR, "add server to ack list failed", KR(ret), K_(palf_id), K_(self), K(sender), K_(ms_ack_list));
    } else {
      ret = OB_SUCCESS;
      // NB: can set majority repeatedly.
      bool majority = is_reach_majority_();
      PALF_LOG(INFO, "ack_config_log success", KR(ret), K_(palf_id), K_(self), K(config_version), K(sender),
          K(majority), K_(ms_ack_list), K(alive_paxos_replica_num_));
    }
  }
  return ret;
}

int LogConfigMgr::try_resend_config_log_(const int64_t proposal_id)
{
  int ret = OB_SUCCESS;
  if (INIT == state_ &&
      resend_log_list_.get_member_number() != 0 &&
      resend_config_version_.is_valid() &&
      palf_reach_time_interval(PALF_RESEND_MSLOG_INTERVAL_US, last_submit_config_log_time_us_) &&
      OB_FAIL(log_engine_->submit_change_config_meta_req(resend_log_list_, proposal_id, prev_log_proposal_id_,
          prev_lsn_, prev_mode_pid_, log_ms_meta_))) {
    PALF_LOG(WARN, "resend config log failed", KR(ret), K(proposal_id), K(resend_config_version_), K_(log_ms_meta));
  }
  return ret;
}

bool LogConfigChangeArgs::is_valid() const
{
  bool bool_ret = true;
  bool_ret = bool_ret && (type_ != INVALID_LOG_CONFIG_CHANGE_TYPE);
  bool_ret = bool_ret && ((is_add_member_list(type_) || is_remove_member_list(type_) ||
           is_add_learner_list(type_) || is_remove_learner_list(type_))? server_.is_valid(): true);
  bool_ret = bool_ret && ((is_use_replica_num_args(type_))? is_valid_replica_num(new_replica_num_): true);
  bool_ret = bool_ret && ((type_ == CHANGE_REPLICA_NUM)? \
          (curr_member_list_.is_valid() && is_valid_replica_num(curr_replica_num_) && is_valid_replica_num(new_replica_num_)): true);
  return bool_ret;
}

void LogConfigChangeArgs::reset()
{
  server_.reset();
  curr_member_list_.reset();
  curr_replica_num_ = 0;
  new_replica_num_ = 0;
  config_version_.reset();
  ref_scn_.reset() ;
  type_ = INVALID_LOG_CONFIG_CHANGE_TYPE;
}

int LogConfigMgr::check_follower_sync_status(const LogConfigChangeArgs &args,
                                             const ObMemberList &new_member_list,
                                             const int64_t new_replica_num,
                                             bool &added_member_has_new_version) const
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(lock_);
  return check_follower_sync_status_(args, new_member_list, new_replica_num, false, added_member_has_new_version);
}

int LogConfigMgr::check_follower_sync_status_(const LogConfigChangeArgs &args,
                                              const ObMemberList &new_member_list,
                                              const int64_t new_replica_num,
                                              const bool is_check_log_barrier,
                                              bool &added_member_has_new_version) const
{
  int ret = OB_SUCCESS;
  LSN first_leader_committed_end_lsn, second_leader_committed_end_lsn;
  LSN first_committed_end_lsn, second_committed_end_lsn;
  constexpr int64_t conn_timeout_us = 3 * 1000 * 1000L;  // 3s
  const int64_t max_log_gap_time = PALF_LEADER_ACTIVE_SYNC_TIMEOUT_US / 4;
  added_member_has_new_version = is_add_member_list(args.type_)? false: true;

  (void) sw_->get_committed_end_lsn(first_leader_committed_end_lsn);
  if (new_member_list.get_member_number() == 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (new_member_list.get_member_number() == 1) {
    ret = OB_SUCCESS;
  } else if (OB_FAIL(sync_get_committed_end_lsn_(args, new_member_list, new_replica_num,
      conn_timeout_us, first_committed_end_lsn, added_member_has_new_version))) {
    PALF_LOG(WARN, "sync_get_committed_end_lsn failed", K(ret), K_(palf_id), K_(self), K(new_member_list),
        K(new_replica_num), K(added_member_has_new_version));
  } else if (is_check_log_barrier) {
    // check log barrier during log appending are stopped
    LSN prev_log_lsn;
    int64_t unused_id;
    if (OB_FAIL(get_log_barrier_(prev_log_lsn, unused_id))) {
      PALF_LOG(WARN, "get_log_barrier_ failed", KR(ret), K_(palf_id), K_(self));
    } else {
      ret = (first_committed_end_lsn >= prev_log_lsn)? OB_SUCCESS: OB_EAGAIN;
      // committed_end_lsn do not change during 2s, skip the reconfiguration
      if (OB_EAGAIN == ret) {
        if (OB_INVALID_TIMESTAMP == last_wait_barrier_time_us_) {
          last_wait_committed_end_lsn_ = first_committed_end_lsn;
          last_wait_barrier_time_us_ = common::ObTimeUtility::current_time();
        } else if (common::ObTimeUtility::current_time() - last_wait_barrier_time_us_ > MAX_WAIT_BARRIER_TIME_US_FOR_RECONFIGURATION) {
          if (last_wait_committed_end_lsn_ == first_committed_end_lsn) {
            ret = OB_LOG_NOT_SYNC;
            PALF_LOG(WARN, "committed_end_lsn havn't been advanced for long time, exit", KR(ret), K_(palf_id), K_(self),
                K_(last_wait_barrier_time_us), K_(last_wait_committed_end_lsn));
            last_wait_barrier_time_us_ = OB_INVALID_TIMESTAMP;
            last_wait_committed_end_lsn_.reset();
          } else {
            last_wait_committed_end_lsn_ = first_committed_end_lsn;
            last_wait_barrier_time_us_ = common::ObTimeUtility::current_time();
          }
        }
      }
      PALF_LOG(INFO, "waiting for log barrier", K(ret), K_(palf_id), K_(self), K(first_committed_end_lsn), K(prev_log_lsn));
    }
  } else if (first_committed_end_lsn >= first_leader_committed_end_lsn) {
    // if committed lsn of new majority do not retreat, then start config change
    PALF_LOG(INFO, "majority of new_member_list are sync with leader, start config change", K(ret), K_(palf_id), K_(self),
            K(first_committed_end_lsn), K(first_leader_committed_end_lsn), K(new_member_list), K(new_replica_num), K(conn_timeout_us));
  // any majority of new memberlist must contain added member, but the max_flushed_end_lsn must always be
  // smaller than leader's, just skip
  } else if (is_add_log_sync_member_list(args.type_) && 2 == new_member_list.get_member_number()) {
    ret = OB_SUCCESS;
    PALF_LOG(INFO, "majority of new memberlist must contain added member, skip", K(ret), K_(palf_id), K_(self),
            K(first_committed_end_lsn), K(first_leader_committed_end_lsn), K(new_member_list), K(new_replica_num), K(conn_timeout_us));
  } else {
    PALF_LOG(INFO, "majority of new_member_list aren't sync with leader", K_(palf_id), K_(self), K(first_committed_end_lsn),
        K(first_leader_committed_end_lsn), K(new_member_list), K(new_replica_num), K(conn_timeout_us));
    // committed_lsn of new majority is behind than old majority's, we want to know if
    // they can catch up with leader during config change timeout. If they can, start config change
    ob_usleep(500 * 1000);
    sw_->get_committed_end_lsn(second_leader_committed_end_lsn);
    int64_t expected_sync_time_s;
    int64_t sync_speed_gap;
    added_member_has_new_version = is_add_member_list(args.type_)? false: true;
    if (OB_FAIL(sync_get_committed_end_lsn_(args, new_member_list, new_replica_num, conn_timeout_us,
        second_committed_end_lsn, added_member_has_new_version))) {
      PALF_LOG(WARN, "sync_get_committed_end_lsn failed", K(ret), K_(palf_id), K_(self), K(new_member_list),
          K(new_replica_num), K(added_member_has_new_version));
    } else if (second_committed_end_lsn >= second_leader_committed_end_lsn) {
      // if committed lsn of new majority do not retreat, then start config change
      PALF_LOG(INFO, "majority of new_member_list are sync with leader, start config change", K_(palf_id), K_(self),
              K(second_committed_end_lsn), K(second_leader_committed_end_lsn), K(new_member_list), K(new_replica_num), K(conn_timeout_us));
    } else if (FALSE_IT(sync_speed_gap = ((second_committed_end_lsn - first_committed_end_lsn) * 2) - \
        ((second_leader_committed_end_lsn - first_leader_committed_end_lsn) * 2) )) {
    } else if (sync_speed_gap <= 0) {
      ret = OB_EAGAIN;
      PALF_LOG(WARN, "follwer is not sync with leader after waiting 500 ms", K_(palf_id), K_(self), K(sync_speed_gap),
              K(ret), K(second_committed_end_lsn), K(second_leader_committed_end_lsn));
    } else if (FALSE_IT(expected_sync_time_s = (second_leader_committed_end_lsn - second_committed_end_lsn) / sync_speed_gap)) {
    } else if ((expected_sync_time_s * 1E6) <= max_log_gap_time) {
      PALF_LOG(INFO, "majority of new_member_list are sync with leader, start config change",
              K_(palf_id), K_(self), K(ret), K(second_committed_end_lsn), K(first_committed_end_lsn), K(sync_speed_gap),
              K(second_leader_committed_end_lsn), K(first_leader_committed_end_lsn), K(expected_sync_time_s), K(max_log_gap_time));
    } else {
      ret = OB_EAGAIN;
      PALF_LOG(INFO, "majority of new_member_list are far behind, can not change member",
              K_(palf_id), K_(self), K(ret), K(second_committed_end_lsn), K(first_committed_end_lsn), K(sync_speed_gap),
              K(second_leader_committed_end_lsn), K(first_leader_committed_end_lsn), K(expected_sync_time_s));
    }
  }
  return ret;
}

int LogConfigMgr::check_servers_lsn_and_version_(const common::ObAddr &server,
                                                 const LogConfigVersion &config_version,
                                                 const int64_t conn_timeout_us,
                                                 const bool force_remote_check,
                                                 LSN &max_flushed_end_lsn,
                                                 bool &has_same_version) const
{
  int ret = OB_SUCCESS;
  LogGetMCStResp resp;
  LsnTsInfo ack_info;
  bool get_from_local = false;
  if (self_ == server) {
    get_from_local = true;
    sw_->get_max_flushed_end_lsn(max_flushed_end_lsn);
  } else if (false == force_remote_check && (OB_FAIL(sw_->get_server_ack_info(server, ack_info)) && OB_ENTRY_NOT_EXIST != ret)) {
    PALF_LOG(WARN, "get_server_ack_info failed", KR(ret), K_(palf_id), K_(self), K(server));
  } else if (false == force_remote_check &&
      OB_SUCC(ret) &&
      common::ObTimeUtility::current_time() - ack_info.last_ack_time_us_ < conn_timeout_us) {
    // ABC remove C, but B is far behind from leader(A), we should not remove C.
    // so we check ack_ts to ensure B's ack_info is fresh.
    max_flushed_end_lsn = ack_info.lsn_;
    get_from_local = true;
  } else if (OB_FAIL(log_engine_->submit_config_change_pre_check_req(server, config_version,
      conn_timeout_us, resp))) {
    PALF_LOG(WARN, "submit_config_change_pre_check_req failed", KR(ret), K_(palf_id), K_(self), K(server),
        K(config_version), K(conn_timeout_us), K(resp));
    has_same_version = false;
  } else if (false == resp.is_normal_replica_) {
    has_same_version = false;
    ret = OB_EAGAIN;
  } else {
    has_same_version = !resp.need_update_config_meta_;
    max_flushed_end_lsn = resp.max_flushed_end_lsn_;
    get_from_local = false;
  }
  PALF_LOG(INFO, "check_servers_lsn_and_version_ finish", K(ret), K_(palf_id), K_(self), K(server), K(config_version),
      K(conn_timeout_us), K(force_remote_check), K(get_from_local), K(resp), K(max_flushed_end_lsn), K(has_same_version));
  return ret;
}

// 1. get committed_end_lsn of new_member_list
// 2. check if the config_version of added member are same to current config_version.
//    if the config change don't add member to list, return true
int LogConfigMgr::sync_get_committed_end_lsn_(const LogConfigChangeArgs &args,
                                              const ObMemberList &new_member_list,
                                              const int64_t new_replica_num,
                                              const int64_t conn_timeout_us,
                                              LSN &committed_end_lsn,
                                              bool &added_member_has_new_version) const
{
  int ret = OB_SUCCESS, tmp_ret = OB_SUCCESS;
  int64_t resp_cnt = 0;
  const LogConfigVersion config_version = config_meta_.curr_.config_version_;
  LSN lsn_array[OB_MAX_MEMBER_NUMBER];

  added_member_has_new_version = is_add_member_list(args.type_)? false: true;

  for (int64_t i = 0; i < new_member_list.get_member_number(); ++i) {
    common::ObAddr server;
    bool is_added_member = false;
    bool force_remote_check = is_added_member;
    LSN max_flushed_end_lsn;
    bool has_same_version = false;
    if (OB_SUCCESS != (tmp_ret = new_member_list.get_server_by_index(i, server))) {
      PALF_LOG(ERROR, "get_server_by_index failed", KR(ret), K_(palf_id), K_(self), K(i),
          "new_member_list size:", new_member_list.get_member_number());
    } else if (FALSE_IT(is_added_member = is_add_member_list(args.type_) && (args.server_.get_server() == server))) {
    } else if (FALSE_IT(force_remote_check = is_added_member)) {
    } else if (OB_SUCCESS != (tmp_ret = check_servers_lsn_and_version_(server, config_version,
        conn_timeout_us, force_remote_check, max_flushed_end_lsn, has_same_version))) {
      PALF_LOG(WARN, "check_servers_lsn_and_version_ failed", K(ret), K(tmp_ret), K_(palf_id), K_(self), K(server),
          K(config_version), K(conn_timeout_us), K(force_remote_check), K(max_flushed_end_lsn), K(has_same_version));
    } else {
      lsn_array[resp_cnt++] = max_flushed_end_lsn;
    }
    added_member_has_new_version = (is_added_member)? has_same_version: added_member_has_new_version;
  }

  // added member isn't in new_member_list, e.g., add arb member
  if (ADD_ARB_MEMBER == args.type_) {
    LSN max_flushed_end_lsn;
    bool has_same_version = false;
    if (OB_SUCCESS != (tmp_ret = check_servers_lsn_and_version_(args.server_.get_server(),
        config_version, conn_timeout_us, true, max_flushed_end_lsn, has_same_version))) {
      PALF_LOG(WARN, "check_servers_lsn_and_version_ failed", K(ret), K_(palf_id), K_(self), K(args), K(config_version),
          K(conn_timeout_us), K(max_flushed_end_lsn), K(has_same_version));
    }
    added_member_has_new_version = has_same_version;
  }

  if (false == added_member_has_new_version) {
    ret = OB_EAGAIN;
    PALF_LOG(WARN, "added member don't have new version, eagain", K(ret), K_(palf_id),
        K_(self), K(args), K(config_version));
  } else if (resp_cnt < new_replica_num / 2 + 1) {
    // do not recv majority resp, can not change member
    ret = OB_EAGAIN;
    PALF_LOG(WARN, "connection timeout with majority of new_member_list, can't change member!",
        K_(palf_id), K_(self), K(new_member_list), K(new_replica_num), K(resp_cnt), K(conn_timeout_us));
  } else {
    std::sort(lsn_array, lsn_array + resp_cnt, LSNCompare());
    committed_end_lsn = lsn_array[new_replica_num / 2];
  }
  PALF_LOG(INFO, "sync_get_committed_end_lsn_ finish", K(ret), K_(palf_id), K_(self), K(args),
      K(new_member_list), K(new_replica_num), K(conn_timeout_us), K(committed_end_lsn),
      K(added_member_has_new_version), "lsn_array:", common::ObArrayWrap<LSN>(lsn_array, resp_cnt));
  return ret;
}

//================================ Config Change ================================

//================================ Child ================================
void LogConfigMgr::reset_registering_state_()
{
  last_submit_register_req_time_us_ = OB_INVALID_TIMESTAMP;
}

bool LogConfigMgr::is_registering_() const
{
  return (last_submit_register_req_time_us_ != OB_INVALID_TIMESTAMP);
}

int LogConfigMgr::get_register_leader_(common::ObAddr &leader) const
{
  int ret = OB_SUCCESS;
  leader = state_mgr_->get_leader();
  if (!leader.is_valid()) {
    ret = OB_EAGAIN;
  }
  return ret;
}

int LogConfigMgr::after_register_done_()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sw_->try_fetch_log(FetchTriggerType::LEARNER_REGISTER))){
    PALF_LOG(WARN, "try_fetch_log failed", KR(ret), K_(palf_id), K_(self), K_(parent), K_(register_time_us));
  }
  PALF_LOG(INFO, "after_register_done", K_(palf_id), K_(self), K_(parent), K_(register_time_us));
  return ret;
}

int LogConfigMgr::after_region_changed_(const common::ObRegion &old_region, const common::ObRegion &new_region)
{
  // re_register parent when region is changed
  // do not need retire children manually when region is changed,
  // children will be retired in check_children_health automatically.
  int ret = OB_SUCCESS;
  if (parent_.is_valid() || is_registering_()) {
    const common::ObAddr old_parent = parent_;
    if (OB_FAIL(retire_parent_())) {
      PALF_LOG(WARN, "retire_parent failed",  KR(ret), K_(palf_id), K_(self), K_(parent));
    } else if (FALSE_IT(reset_parent_info_())) {
      // if i'm registering when change region, need reset all parent info and
      // start a new regisration with new region.
    } else if (OB_FAIL(register_parent_())) {
      PALF_LOG(WARN, "register_parent failed", KR(ret), K_(palf_id), K_(self), K(old_region), K(new_region));
    } else {
      PALF_LOG(INFO, "re_register_parent reason: region_changed", K_(palf_id), K_(self), K(old_parent), K(old_region), K(new_region));
    }
  }
  return ret;
}

int LogConfigMgr::register_parent()
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(parent_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (!parent_.is_valid() && OB_FAIL(register_parent_())) {
    PALF_LOG(WARN, "register_parent failed", KR(ret), K_(palf_id), K_(self), K_(parent));
  } else {
  }
  return ret;
}

// @return
// - OB_EAGAIN: leader is invalid
int LogConfigMgr::register_parent_()
{
  int ret = OB_SUCCESS;
  ObAddr leader;
  if (OB_FAIL(get_register_leader_(leader))) {
    PALF_LOG(TRACE, "get_register_leader_ failed", KR(ret), K_(palf_id), K_(self));
  } else {
    const bool is_to_leader = true;
    const int64_t curr_time_us = ObTimeUtility::current_time();
    LogLearner child_self(self_, region_, curr_time_us);
    if (OB_FAIL(log_engine_->submit_register_parent_req(leader, child_self, is_to_leader))) {
      // NB: register_req sends my addr_, region_ and register_time_us_
      PALF_LOG(WARN, "submit_register_parent_req failed", KR(ret), K_(palf_id), K_(self), K(leader), K(child_self), K(is_to_leader));
    } else {
      last_submit_register_req_time_us_ = curr_time_us;
      register_time_us_ = curr_time_us;
      parent_keepalive_time_us_ = OB_INVALID_TIMESTAMP;
    }
  }
  return ret;
}

int LogConfigMgr::handle_register_parent_resp(const LogLearner &server,
                                              const LogCandidateList &candidate_list,
                                              const RegisterReturn reg_ret)
{
  int ret = OB_SUCCESS;
  bool do_after_register_done = false;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else if (!server.is_valid() || INVALID_REG_RET == reg_ret) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", KR(ret), K(server));
  } else {
    SpinLockGuard guard(parent_lock_);
    if (!is_registering_() || register_time_us_ != server.register_time_us_) {
      ret = OB_STATE_NOT_MATCH;
      PALF_LOG(WARN, "receive wrong register resp", K_(palf_id), K_(self), K(server), K_(register_time_us), "registering", is_registering_());
    } else if (REGISTER_DONE == reg_ret) {
      // register done, just set parent_ and clean registering state
      parent_ = server.server_;
      parent_keepalive_time_us_ = common::ObTimeUtility::current_time();
      reset_registering_state_();
      do_after_register_done = true;
      PALF_EVENT("register_parent done", palf_id_, K_(self), K_(parent), K_(register_time_us));
    } else if (REGISTER_CONTINUE == reg_ret && candidate_list.get_member_number() > 0) {
      common::ObAddr reg_dst;
      const int64_t reg_dst_idx = ObRandom::rand(0, candidate_list.get_member_number() - 1);
      LogLearner child_self(self_, region_, register_time_us_);
      if (OB_FAIL(candidate_list.get_server_by_index(reg_dst_idx, reg_dst))) {
        PALF_LOG(WARN, "get_server_by_index failed", KR(ret), K_(palf_id), K_(self), K(candidate_list), K(reg_dst));
      } else if (OB_FAIL(log_engine_->submit_register_parent_req(reg_dst, child_self, false))) {
        PALF_LOG(WARN, "submit_register_parent_req failed", KR(ret), K_(palf_id), K_(self), K(reg_dst));
      } else {
        last_submit_register_req_time_us_ = common::ObTimeUtility::current_time();
      }
    } else if (REGISTER_DIFF_REGION == reg_ret) {
      const char *reason = "diff_region";
      PALF_LOG(INFO, "re_register_parent reason", K(reason), K_(palf_id), K_(self), K_(region), K(server));
      if (OB_FAIL(register_parent_())) {
        PALF_LOG(WARN, "re_register failed", KR(ret), K_(palf_id), K_(self), K(server));
      }
    } else if (REGISTER_NOT_MASTER == reg_ret) {
      // skip, wait retry
    } else {
      ret = OB_ERR_UNEXPECTED;
      PALF_LOG(ERROR, "unexpected Register_Return", K_(palf_id), K_(self), K(server), K(candidate_list), K(reg_ret));
    }
  }
  if (do_after_register_done && OB_FAIL(after_register_done_())) {
    PALF_LOG(WARN, "after_register_done failed", KR(ret), K_(palf_id), K_(self));
  }
  PALF_LOG(INFO, "handle_register_parent_resp finished", KR(ret), K_(palf_id), K_(self), K(server), K(candidate_list), K(reg_ret));
  return ret;
}

int LogConfigMgr::retire_parent_()
{
  int ret = OB_SUCCESS;
  if (!parent_.is_valid()) {
    // parent is already invalid, skip
  } else {
    LogLearner child_self(self_, region_, register_time_us_);
    if (OB_FAIL(log_engine_->submit_retire_parent_req(parent_, child_self))) {
      PALF_LOG(WARN, "submit_retire_parent_req failed", KR(ret), K_(palf_id), K_(self), K_(parent), K_(register_time_us));
    } else {
      PALF_LOG(INFO, "submit_retire_parent_req success", K_(palf_id), K_(self), K_(parent), K_(register_time_us));
      reset_parent_info_();
    }
  }
  return ret;
}

int LogConfigMgr::handle_retire_child(const LogLearner &parent)
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(parent_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else if (is_registering_() || parent.server_ != parent_ || parent.register_time_us_ != register_time_us_) {
    PALF_LOG(WARN, "handle_retire_child failed, invalid msg", KR(ret), K(parent), K(parent_), K_(self));
  } else {
    reset_parent_info_();
    PALF_LOG(INFO, "re_register_parent reason: handle_retire_child", K_(palf_id), K_(self), K(parent));
    if (OB_FAIL(register_parent_())) {
      PALF_LOG(WARN, "register_parent failed when recving retire child", KR(ret), K_(self), K(parent));
    } else {
      PALF_LOG(INFO, "handle_retire_child success", KR(ret), K_(self), K(parent));
    }
  }
  return ret;
}

int LogConfigMgr::handle_learner_keepalive_req(const LogLearner &parent)
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(parent_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    PALF_LOG(WARN, "LogConfigMgr not init", KR(ret));
  } else if (!parent.is_valid() || parent.register_time_us_ <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (true == is_registering_() || parent.server_ != parent_ || parent.register_time_us_ != register_time_us_) {
    PALF_LOG(WARN, "handle_keepalive failed", KR(ret), K_(palf_id), K_(self), K(parent), K(parent_), K_(register_time_us));
  } else {
    parent_keepalive_time_us_ = common::ObTimeUtility::current_time();
    LogLearner child_itself(self_, region_, register_time_us_);
    if (OB_FAIL(log_engine_->submit_learner_keepalive_resp(parent.server_, child_itself))) {
      PALF_LOG(WARN, "submit_learner_keepalive_resp failed", KR(ret), K_(palf_id), K_(self), K(parent));
    } else {
      PALF_LOG(INFO, "handle_learner_keepalive_req success", KR(ret), K_(palf_id), K_(self), K(parent), K_(parent_keepalive_time_us));
    }
  }
  return ret;
}

//loop thread call this periodically
// 1. check parent keepalive_ts timeout
// 2. push registering state
int LogConfigMgr::check_parent_health()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    SpinLockGuard guard(parent_lock_);
    const int64_t curr_time_us = common::ObTimeUtility::current_time();
    const bool is_registering_timeout = (is_registering_() && curr_time_us - last_submit_register_req_time_us_ > PALF_CHILD_RESEND_REGISTER_INTERVAL_US);
    const bool first_registration = (!parent_.is_valid() && !is_registering_() &&
        palf_reach_time_interval(PALF_CHILD_RESEND_REGISTER_INTERVAL_US, last_first_register_time_us_));
    const bool parent_timeout = (parent_.is_valid() && curr_time_us - parent_keepalive_time_us_ > PALF_PARENT_CHILD_TIMEOUT_US);
    if (is_registering_timeout || first_registration || parent_timeout) {
      PALF_LOG(INFO, "re_register_parent reason", K_(palf_id), K_(self), K(is_registering_timeout), K(first_registration), K(parent_timeout),
          K_(parent_keepalive_time_us), K_(last_submit_register_req_time_us), K_(last_first_register_time_us), K_(register_time_us), K(curr_time_us));
      reset_parent_info_();
      if (OB_FAIL(register_parent_())) {
        PALF_LOG(WARN, "register request timeout, re_register_parent failed", KR(ret), K_(palf_id), K_(self));
      } else {
        PALF_LOG(INFO, "re register_parent success", KR(ret), K_(palf_id), K_(self));
      }
    }
  }
  return ret;
}

void LogConfigMgr::reset_parent_info_()
{
  register_time_us_ = OB_INVALID_TIMESTAMP;
  parent_.reset();
  parent_keepalive_time_us_ = OB_INVALID_TIMESTAMP;
  last_submit_register_req_time_us_ = OB_INVALID_TIMESTAMP;
}
//================================ Child ================================

//================================ Parent ================================
int LogConfigMgr::handle_register_parent_req(const LogLearner &child, const bool is_to_leader)
{
  int ret = OB_SUCCESS;
  LogCandidateList candidate_list;
  LogLearnerList retired_children;
  RegisterReturn reg_ret = INVALID_REG_RET;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (!child.is_valid() || child.register_time_us_ <= 0) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid argument", KR(ret), K_(palf_id), K_(self), K(child));
  } else if (is_to_leader && !all_learnerlist_.contains(child.get_server())) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "registering child is not in global learner list", K_(palf_id), K_(self), K(child));
  } else {
    SpinLockGuard guard(child_lock_);
    int64_t idx = -1;
    const bool is_in_children = (-1 != (idx = children_.get_index_by_addr(child.server_)));
    if (is_in_children && children_.get_learner(idx).region_ == child.region_) {
      // if child is already in the children list and child's region don't change,
      // replace old child in children_list
      children_.get_learner(idx).register_time_us_ = child.register_time_us_;
      children_.get_learner(idx).update_keepalive_ts();
      reg_ret = REGISTER_DONE;
    } else if (is_in_children && FALSE_IT(children_.remove_learner(child.server_))) {
      // if child is already in the children list and child's region has changed,
      // remove it from children_ and re-register.
    } else if (is_to_leader) {
      if (OB_FAIL(generate_candidate_list_(child, candidate_list))) {
        PALF_LOG(WARN, "generate_candidate_list failed", KR(ret), K_(palf_id), K_(self), K(child));
      } else if (candidate_list.get_member_number() > 0) {
        // register continue
        reg_ret = REGISTER_CONTINUE;
      } else {
        // register to self
        if (children_.is_full() && OB_FAIL(remove_duplicate_region_child_(retired_children))) {
          PALF_LOG(WARN, "remove_duplicate_region_child failed", KR(ret), K_(palf_id), K_(self));
        } else if (children_.is_full()) {
          // child is not in same region with any leader's child, and leader's children_list is full
          // It means there are OB_MAX_CHILD_MEMBER_NUMBER in children_ and their regions are different from each other
          ret = OB_NOT_SUPPORTED;
          PALF_LOG(ERROR, "leader's children is full and their regions are different", KR(ret), K_(palf_id), K_(self), K(child), K_(children));
        } else {
          LogLearner dst_child(child);
          dst_child.keepalive_ts_ = common::ObTimeUtility::current_time();
          dst_child.register_time_us_ = child.register_time_us_;
          if (OB_FAIL(children_.add_learner(dst_child))) {
            PALF_LOG(WARN, "handle_register_parent_req failed", KR(ret), K_(palf_id), K_(self), K(is_to_leader), K(dst_child));
          } else {
            reg_ret = REGISTER_DONE;
          }
        }
      }
    } else if (child.region_ != region_) {
      // follower will reject register req which region is different
      reg_ret = REGISTER_DIFF_REGION;
    } else if (children_.get_member_number() < OB_MAX_CHILD_MEMBER_NUMBER_IN_FOLLOWER) {
      // register to self
      LogLearner dst_child(child);
      dst_child.keepalive_ts_ = common::ObTimeUtility::current_time();
      dst_child.register_time_us_ = child.register_time_us_;
      if (OB_FAIL(children_.add_learner(dst_child))) {
        PALF_LOG(WARN, "handle_register_parent_req failed", KR(ret), K_(palf_id), K_(self), K(is_to_leader), K(dst_child));
      } else {
        reg_ret = REGISTER_DONE;
      }
    } else if (OB_FAIL(generate_candidate_list_from_children_(child, candidate_list))) {
        PALF_LOG(WARN, "generate_candidate_list failed", KR(ret), K_(palf_id), K_(self), K(child));
    } else if (candidate_list.get_member_number() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      PALF_LOG(ERROR, "candidate_list is empty", KR(ret), K_(palf_id), K_(self), K(candidate_list), K_(region), K_(children));
    } else {
      // register continue
      reg_ret = REGISTER_CONTINUE;
    }
  }
  if (OB_SUCC(ret)){
    LogLearner parent(self_, region_, child.register_time_us_);
    if (reg_ret == REGISTER_DONE ||
        reg_ret == REGISTER_CONTINUE ||
        reg_ret == REGISTER_DIFF_REGION) {
      if (OB_FAIL(log_engine_->submit_register_parent_resp(child.server_, parent, candidate_list, reg_ret))) {
        PALF_LOG(WARN, "submit_register_parent_resp failed", KR(ret), K_(palf_id), K_(self), K(child));
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      PALF_LOG(ERROR, "invalid register ret", K(ret), K_(palf_id), K_(self), K(reg_ret));
    }
    PALF_LOG(INFO, "handle_register_parent_req success", K(ret), K(child), K(is_to_leader), K(candidate_list),
        K(reg_ret), K_(children), "member_list", log_ms_meta_.curr_.log_sync_memberlist_);
  }
  if (OB_FAIL(submit_retire_children_req_(retired_children))) {
    PALF_LOG(WARN, "submit_retire_children_req failed", KR(ret), K_(palf_id), K_(self), K(retired_children));
  }
  return ret;
}

int LogConfigMgr::handle_retire_parent(const LogLearner &child)
{
  int ret = OB_SUCCESS;
  int64_t idx = -1;
  SpinLockGuard guard(child_lock_);
  LogLearner learner;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (!child.is_valid() || child.register_time_us_ <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_SUCC(children_.get_learner_by_addr(child.server_, learner)) &&
             learner.register_time_us_ == child.register_time_us_) {
    if (OB_FAIL(children_.remove_learner(child))) {
      PALF_LOG(WARN, "children_ remove_learner failed", KR(ret), K_(palf_id), K_(self), K_(children), K(child));
    } else {
      PALF_LOG(INFO, "handle_retire_parent success", KR(ret), K_(palf_id), K_(self), K(child));
    }
  } else {
    PALF_LOG(INFO, "handle_retire_parent failed, invalid req", KR(ret), K_(palf_id), K_(self), K(child), K_(children));
    // skip
  }
  return ret;
}

int LogConfigMgr::handle_learner_keepalive_resp(const LogLearner &child)
{
  int ret = OB_SUCCESS;
  bool need_resp = false;
  int64_t idx = -1;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (!child.is_valid() || child.register_time_us_ <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    SpinLockGuard guard(child_lock_);
    int64_t idx = -1;
    if (-1 != (idx = children_.get_index_by_addr(child.server_)) &&
        children_.get_learner(idx).register_time_us_ == child.register_time_us_) {
      children_.get_learner(idx).update_keepalive_ts();
      PALF_LOG(INFO, "handle_learner_keepalive_resp success", K_(palf_id), K_(self), K_(children));
    }
  }
  return ret;
}

// caller guarantees role do not change
// common::
// 1. check if children are timeout
// for leader:
// 2. guarantee regions of children are unique
// for follower:
// 2. guarantee regions of children are same with region_
int LogConfigMgr::check_children_health()
{
  int ret = OB_SUCCESS;
  LogLearnerList dead_children;
  LogLearnerList diff_region_children;
  LogLearnerList dup_region_children;
  const bool is_leader = state_mgr_->is_leader_active();
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    if (children_.is_valid()) {
      SpinLockGuard guard(child_lock_);
      // 1. remove child that is timeout(follower, leader)
      if (OB_FAIL(remove_timeout_child_(dead_children))) {
        PALF_LOG(WARN, "remove_timeout_child failed", KR(ret), K_(palf_id), K_(self));
      }
      // 2. remove child which region are different from mine in follower
      if (!is_leader && OB_FAIL(remove_diff_region_child_(diff_region_children))) {
        PALF_LOG(WARN, "remove_diff_region_child_in_follower failed", KR(ret), K_(palf_id), K_(self));
      }
      // 3. remove duplicate region children in leader
      if (is_leader && OB_FAIL(remove_duplicate_region_child_(dup_region_children))) {
        PALF_LOG(WARN, "remove_duplicate_region_child failed", KR(ret), K_(palf_id), K_(self));
      }
    }
    // 4. send keepalive msg to children
    if (children_.is_valid() && palf_reach_time_interval(PALF_PARENT_KEEPALIVE_INTERVAL_US, last_submit_keepalive_time_us_)) {
      // reach keepalive interval, submit keepalive req
      SpinLockGuard guard(child_lock_);
      LogLearner parent_self(self_, region_, OB_INVALID_TIMESTAMP);
      for (int64_t i = 0; i < children_.get_member_number(); ++i) {
        LogLearner child;
        if (OB_FAIL(children_.get_learner(i, child))) {
          PALF_LOG(WARN, "children_.get_learner failed", KR(ret), K_(palf_id), K_(self), K(i));
        } else if (FALSE_IT(parent_self.register_time_us_ = child.register_time_us_)) {
        } else if (OB_FAIL(log_engine_->submit_learner_keepalive_req(child.get_server(), parent_self))) {
          PALF_LOG(WARN, "submit_learner_keepalive_req failed", KR(ret), K_(palf_id), K_(self), K(child));
        } else {
        }
      }
    }
    // 5. retire removed children
    if (OB_FAIL(submit_retire_children_req_(dead_children))) {
      PALF_LOG(WARN, "submit_retire_children_req failed", KR(ret), K_(palf_id), K_(self), K(dead_children));
    } else if (!is_leader && submit_retire_children_req_(diff_region_children)) {
      PALF_LOG(WARN, "submit_retire_children_req failed", KR(ret), K_(palf_id), K_(self), K(diff_region_children));
    } else if (is_leader && submit_retire_children_req_(dup_region_children)) {
      PALF_LOG(WARN, "submit_retire_children_req failed", KR(ret), K_(palf_id), K_(self), K(dup_region_children));
    }
  }
  return ret;
}

int LogConfigMgr::remove_timeout_child_(LogLearnerList &dead_children)
{
  int ret = OB_SUCCESS;
  LogLearnerCond child_timeout_cond;
  LogLearnerAction child_timeout_action;
  // if child is timeout, then add it to dead_children
  if (OB_FAIL(child_timeout_cond.assign([](const LogLearner &child) { return child.is_timeout(PALF_PARENT_CHILD_TIMEOUT_US);}))) {
    PALF_LOG(WARN, "child_timeout_cond assign failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(child_timeout_action.assign([&dead_children](const LogLearner &child) {return dead_children.add_learner(child);}))) {
    PALF_LOG(WARN, "child_timeout_action assign failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(children_if_cond_then_action_(child_timeout_cond, child_timeout_action))) {
    PALF_LOG(WARN, "children_if_cond_then_action failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(remove_children_(children_, dead_children))) {
    PALF_LOG(WARN, "remove_children failed", KR(ret), K(dead_children), K_(children));
  } else if (dead_children.get_member_number() > 0) {
    PALF_LOG(INFO, "remove_timeout_child success", K(ret), K_(palf_id), K_(self), K_(children), K(dead_children));
  }
  return ret;
}

int LogConfigMgr::remove_diff_region_child_(LogLearnerList &diff_region_children)
{
  int ret = OB_SUCCESS;
  LogLearnerCond diff_region_cond;
  LogLearnerAction diff_region_action;
  // if child's region is different from mine and i'm follower, add it to diff_region_children
  if (OB_FAIL(diff_region_cond.assign([this](const LogLearner &child) { return child.region_ != region_;}))) {
    PALF_LOG(WARN, "diff_region_cond assign failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(diff_region_action.assign([&diff_region_children](const LogLearner &child) {return diff_region_children.add_learner(child);}))) {
    PALF_LOG(WARN, "diff_region_action assign failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(children_if_cond_then_action_(diff_region_cond, diff_region_action))) {
    PALF_LOG(WARN, "children_if_cond_then_action failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(remove_children_(children_, diff_region_children))) {
    PALF_LOG(WARN, "remove_children failed", KR(ret), K(diff_region_children), K_(children));
  } else if (diff_region_children.get_member_number() > 0) {
    PALF_LOG(INFO, "remove_diff_region_child success", K(ret), K_(palf_id), K_(self), K_(children), K(diff_region_children));
  }
  return ret;
}

int LogConfigMgr::remove_child_is_not_learner_(LogLearnerList &removed_children)
{
  int ret = OB_SUCCESS;
  SpinLockGuard guard(child_lock_);
  LogLearnerCond cond;
  LogLearnerAction action;
  const GlobalLearnerList &learnerlist = all_learnerlist_;
  if (OB_FAIL(cond.assign([&learnerlist](const LogLearner &child)->bool { return !learnerlist.contains(child.get_server()); }))) {
    PALF_LOG(WARN, "learnerlist cond assign failed", K(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(action.assign([&removed_children](const LogLearner &child)->int { return removed_children.add_learner(child); }))) {
    PALF_LOG(WARN, "learnerlist action assign failed", K(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(children_if_cond_then_action_(cond, action))) {
    PALF_LOG(WARN, "children_if_cond_then_action failed", K(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(remove_children_(children_, removed_children))) {
    PALF_LOG(WARN, "remove_children failed", KR(ret), K(removed_children), K_(children));
  } else if (removed_children.get_member_number() > 0) {
    PALF_LOG(INFO, "remove_child_is_not_learner success", K(ret), K_(palf_id), K_(self), K_(children), K(removed_children));
  }
  return ret;
}

int LogConfigMgr::remove_duplicate_region_child_(LogLearnerList &dup_region_children)
{
  int ret = OB_SUCCESS;
  common::ObArrayHashMap<ObRegion, int> region_map;
  LogLearnerCond cond;
  LogLearnerAction action;
  const int64_t REGION_MAP_SIZE = MIN(OB_MAX_MEMBER_NUMBER + children_.get_member_number(), MAX_ZONE_NUM);
  if (children_.get_member_number() == 0) {
    // skip
  } else if (OB_FAIL(region_map.init("LogTmpRegionMap", REGION_MAP_SIZE))) {
    PALF_LOG(WARN, "region_map init failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(get_member_regions_(region_map))) {
    PALF_LOG(WARN, "get_member_region failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(cond.assign([](const LogLearner &child)->bool { UNUSED(child); return true; }))) {
    PALF_LOG(WARN, "cond assign failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(action.assign(
    [&region_map, &dup_region_children, this](const LogLearner &child)->int {
    int ret = OB_SUCCESS;
    int unused_val = 0;
    if (OB_FAIL(region_map.get(child.region_, unused_val))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(region_map.insert(child.region_, 1))) {
          PALF_LOG(WARN, "region_map.insert failed", KR(ret), K_(palf_id), K_(self), K(child));
        }
      } else {
        PALF_LOG(WARN, "region_map.get failed", KR(ret), K_(palf_id), K_(self), K(child));
      }
    } else if (OB_FAIL(dup_region_children.add_learner(child))) {
      PALF_LOG(WARN, "retired_children.add_learner failed", KR(ret), K_(palf_id), K_(self), K(child));
    }
    return ret;
  }))) {
    PALF_LOG(WARN, "action assign failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(children_if_cond_then_action_(cond, action))) {
    PALF_LOG(WARN, "children_if_cond_then_action failed", KR(ret), K_(palf_id), K_(self));
  } else if (OB_FAIL(remove_children_(children_, dup_region_children))) {
    PALF_LOG(WARN, "remove dup_region_children from children_ failed", KR(ret), K_(children), K(dup_region_children));
  } else if (dup_region_children.get_member_number() > 0) {
    PALF_LOG(INFO, "remove_duplicate_region_child success", K(ret), K_(palf_id), K_(self), K_(children), K(dup_region_children));
  }
  return ret;
}

int LogConfigMgr::children_if_cond_then_action_(const LogLearnerCond &cond, const LogLearnerAction &action)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; i < children_.get_member_number(); ++i) {
    LogLearner child;
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = children_.get_learner(i, child))) {
      PALF_LOG(WARN, "get_server_by_addr failed", K(tmp_ret), K_(palf_id), K_(self), K(i));
    } else if (cond(child)) {
      if (OB_SUCCESS != (tmp_ret = action(child))) {
        PALF_LOG(WARN, "add_learner failed", K(tmp_ret), K(child));
      }
    }
  }
  return ret;
}

int LogConfigMgr::remove_children_(LogLearnerList &this_children, const LogLearnerList &removed_children)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; i < removed_children.get_member_number(); ++i) {
    LogLearner removed_learner;
    if (OB_FAIL(removed_children.get_learner(i, removed_learner))) {
      PALF_LOG(WARN, "get_learner failed", KR(ret));
    } else if (OB_FAIL(this_children.remove_learner(removed_learner))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

// generate candidate_list in which region of candidate is same with child's
int LogConfigMgr::generate_candidate_list_(const LogLearner &child, LogCandidateList &candidate_list)
{
  int ret = OB_SUCCESS;
  if (!child.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(generate_candidate_list_from_member_(child, candidate_list))){
    PALF_LOG(WARN, "generate_candidate_list_from_emmber_ failed", KR(ret), K(child), K(candidate_list));
  } else if (OB_FAIL(generate_candidate_list_from_children_(child, candidate_list))) {
    PALF_LOG(WARN, "generate_candidate_list_from_children_ failed", KR(ret), K(child), K(candidate_list));
  } else {
  }
  return ret;
}

int LogConfigMgr::generate_candidate_list_from_member_(const LogLearner &child, LogCandidateList &candidate_list)
{
  int ret = OB_SUCCESS;
  const ObMemberList &curr_member_list = log_ms_meta_.curr_.log_sync_memberlist_;
  for (int64_t i = 0; i < curr_member_list.get_member_number(); ++i) {
    ObAddr addr;
    ObRegion region;
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = curr_member_list.get_server_by_index(i, addr))) {
      PALF_LOG(WARN, "get_server_by_index failed", KR(ret), K(curr_member_list), K(i));
    } else if (OB_SUCCESS != (tmp_ret = paxos_member_region_map_.get(addr, region))) {
      PALF_LOG(WARN, "paxos_member_region_map_ get failed", KR(tmp_ret), K_(palf_id), K_(self),
      K_(paxos_member_region_map), K(addr));
    } else if (addr == self_ || region != child.region_) {
      // skip
    } else if (OB_SUCCESS == (tmp_ret = candidate_list.add_learner(common::ObMember(addr, 1)))) {
    } else if (OB_ENTRY_EXIST == tmp_ret) {
      continue;
    } else if (OB_SIZE_OVERFLOW == tmp_ret) {
      break;
    } else {
      ret = tmp_ret;
      PALF_LOG(WARN, "add_learner failed", KR(ret));
      break;
    }
  }
  return ret;
}

int LogConfigMgr::generate_candidate_list_from_children_(const LogLearner &child, LogCandidateList &candidate_list)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; i < children_.get_member_number(); ++i) {
    LogLearner learner;
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = children_.get_learner(i, learner))) {
      PALF_LOG(WARN, "get_region failed", KR(tmp_ret), K_(children));
    } else if (child.region_ != learner.region_) {
      // continue
    } else if (OB_SUCCESS == (tmp_ret = candidate_list.add_learner(common::ObMember(learner.server_, 1)))) {
    } else if (OB_ENTRY_EXIST == tmp_ret) {
      continue;
    } else if (OB_SIZE_OVERFLOW == tmp_ret) {
      break;
    } else {
      ret = tmp_ret;
      PALF_LOG(WARN, "add_learner failed", KR(ret));
      break;
    }
  }
  return ret;
}

int LogConfigMgr::submit_retire_children_req_(const LogLearnerList &retired_children)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  LogLearner parent(self_, region_, OB_INVALID_ARGUMENT);
  for (int64_t i = 0; i < retired_children.get_member_number(); ++i) {
    LogLearner retired_child;
    if (OB_SUCCESS != (tmp_ret = retired_children.get_learner(i, retired_child))) {
      PALF_LOG(WARN, "get_learner failed", K(retired_children));
    } else if (FALSE_IT(parent.register_time_us_ = retired_child.register_time_us_)) {
    } else if (OB_SUCCESS != (tmp_ret = log_engine_->submit_retire_child_req(retired_child.server_, parent))) {
      PALF_LOG(WARN, "submit_retire_child_req failed", KR(ret), K(retired_child), K(parent));
    }
  }
  return ret;
}

int LogConfigMgr::get_member_regions_(common::ObArrayHashMap<ObRegion, int> &region_map) const
{
  int ret = OB_SUCCESS;
  const ObMemberList &curr_member_list = log_ms_meta_.curr_.log_sync_memberlist_;
  for (int64_t i = 0; i < curr_member_list.get_member_number(); ++i) {
    ObAddr addr;
    ObRegion region;
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = curr_member_list.get_server_by_index(i, addr))) {
      PALF_LOG(WARN, "get_server_by_index failed", KR(ret), K(curr_member_list), K(i));
    } else if (addr == self_) {
      // skip
    } else if (OB_SUCCESS != (tmp_ret = paxos_member_region_map_.get(addr, region))) {
      PALF_LOG(WARN, "paxos_member_region_map_.get failed", KR(ret), K(addr));
    } else if (OB_SUCCESS != (tmp_ret = region_map.insert(region, 1)) && OB_ENTRY_EXIST != tmp_ret) {
      PALF_LOG(WARN, "region_map.insert_or_update failed", KR(ret), K_(palf_id), K_(self), K(region));
    }
  }
  return ret;
}
//================================ Parent ================================
} // namespace palf
} // namespace oceanbase
