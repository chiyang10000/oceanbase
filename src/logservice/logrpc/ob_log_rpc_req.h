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

#ifndef OCEANBASE_LOGSERVICE_OB_LOG_RPC_REQ_H_
#define OCEANBASE_LOGSERVICE_OB_LOG_RPC_REQ_H_

#include "lib/utility/ob_unify_serialize.h"                    // OB_UNIS_VERSION
#include "lib/utility/ob_print_utils.h"                        // TO_STRING_KV
#include "common/ob_member_list.h"                             // ObMemberList
#include "logservice/palf/palf_options.h"                      // AccessMode
#include "logservice/palf/palf_handle_impl.h"                  // PalfStat
#include "share/scn.h"

namespace oceanbase
{
namespace logservice
{

enum LogConfigChangeCmdType {
  INVALID_CONFIG_CHANGE_CMD = 0,
  CHANGE_REPLICA_NUM_CMD,
  ADD_MEMBER_CMD,
  REMOVE_MEMBER_CMD,
  REPLACE_MEMBER_CMD,
  ADD_LEARNER_CMD,
  REMOVE_LEARNER_CMD,
  SWITCH_TO_ACCEPTOR_CMD,
  SWITCH_TO_LEARNER_CMD,
};

inline const char *log_config_change_cmd2str(const LogConfigChangeCmdType state)
{
  #define CHECK_CMD_TYPE_STR(x) case(LogConfigChangeCmdType::x): return #x
  switch(state)
  {
    CHECK_CMD_TYPE_STR(ADD_MEMBER_CMD);
    CHECK_CMD_TYPE_STR(REMOVE_MEMBER_CMD);
    CHECK_CMD_TYPE_STR(REPLACE_MEMBER_CMD);
    CHECK_CMD_TYPE_STR(ADD_LEARNER_CMD);
    CHECK_CMD_TYPE_STR(REMOVE_LEARNER_CMD);
    CHECK_CMD_TYPE_STR(SWITCH_TO_ACCEPTOR_CMD);
    CHECK_CMD_TYPE_STR(SWITCH_TO_LEARNER_CMD);
    CHECK_CMD_TYPE_STR(CHANGE_REPLICA_NUM_CMD);
    default:
      return "Invalid";
  }
  #undef CHECK_CMD_TYPE_STR
}

struct LogConfigChangeCmd {
  OB_UNIS_VERSION(1);
public:
  LogConfigChangeCmd();
  LogConfigChangeCmd(const common::ObAddr &src,
                     const int64_t palf_id,
                     const common::ObMember &added_member,
                     const common::ObMember &removed_member,
                     const int64_t paxos_replica_num,
                     const LogConfigChangeCmdType cmd_type,
                     const int64_t timeout_us);
  LogConfigChangeCmd(const common::ObAddr &src,
                     const int64_t palf_id,
                     const common::ObMemberList &member_list,
                     const int64_t curr_replica_num,
                     const int64_t new_replica_num,
                     const LogConfigChangeCmdType cmd_type,
                     const int64_t timeout_us);
  ~LogConfigChangeCmd();
  bool is_valid() const;
  void reset();
  bool is_remove_member_list() const;
  bool is_add_member_list() const;
  TO_STRING_KV("cmd_type", log_config_change_cmd2str(cmd_type_), K_(src), K_(palf_id), \
  K_(added_member), K_(removed_member), K_(curr_member_list), K_(curr_replica_num),      \
  K_(new_replica_num), K_(timeout_us));
  common::ObAddr src_;
  int64_t palf_id_;
  common::ObMember added_member_;
  common::ObMember removed_member_;
  common::ObMemberList curr_member_list_;
  int64_t curr_replica_num_;
  int64_t new_replica_num_;
  LogConfigChangeCmdType cmd_type_;
  int64_t timeout_us_;
};

struct LogConfigChangeCmdResp {
  OB_UNIS_VERSION(1);
public:
  LogConfigChangeCmdResp();
  LogConfigChangeCmdResp(const int ret);
  ~LogConfigChangeCmdResp();
  bool is_valid() const;
  void reset();
  TO_STRING_KV(K_(ret));
  int ret_;
};

struct LogGetLeaderMaxScnReq {
  OB_UNIS_VERSION(1);
public:
  LogGetLeaderMaxScnReq(): src_(), palf_id_(palf::INVALID_PALF_ID) { }
  LogGetLeaderMaxScnReq(const common::ObAddr &src,
                      const int64_t palf_id);
  ~LogGetLeaderMaxScnReq();
  bool is_valid() const;
  void reset();
  TO_STRING_KV(K_(src), K_(palf_id));
  common::ObAddr src_;
  int64_t palf_id_;
};

struct LogGetLeaderMaxScnResp {
  OB_UNIS_VERSION(1);
public:
  LogGetLeaderMaxScnResp() : max_scn_()  { }
  LogGetLeaderMaxScnResp(const share::SCN &max_scn);
  ~LogGetLeaderMaxScnResp();
  bool is_valid() const;
  void reset();
  TO_STRING_KV(K_(max_scn));
  share::SCN max_scn_;
};

struct LogGetPalfStatReq {
  OB_UNIS_VERSION(1);
public:
  LogGetPalfStatReq(): src_(), palf_id_(-1), is_to_leader_(false) { }
  LogGetPalfStatReq(const common::ObAddr &src,
                    const int64_t palf_id,
                    const bool is_to_leader);
  ~LogGetPalfStatReq();
  bool is_valid() const;
  void reset();
  TO_STRING_KV(K_(src), K_(palf_id), K_(is_to_leader));
  common::ObAddr src_;
  int64_t palf_id_;
  bool is_to_leader_;
};

struct LogGetPalfStatResp {
  OB_UNIS_VERSION(1);
public:
  LogGetPalfStatResp() : palf_stat_()  { }
  LogGetPalfStatResp(const palf::PalfStat &palf_stat);
  ~LogGetPalfStatResp();
  bool is_valid() const;
  void reset();
  TO_STRING_KV(K_(palf_stat));
  palf::PalfStat palf_stat_;
};

enum LogServerProbeType
{
  PROBE_REQ = 0,
  PROBE_RESP,
};

struct LogServerProbeMsg {
  OB_UNIS_VERSION(1);
public:
  LogServerProbeMsg();
  LogServerProbeMsg(const common::ObAddr &src, const LogServerProbeType msg_type, const int64_t req_ts);
  ~LogServerProbeMsg();
  bool is_valid() const;
  void reset();
  TO_STRING_KV(K_(src), K_(msg_type), K_(req_ts));
  common::ObAddr src_;
  LogServerProbeType msg_type_;
  int64_t req_ts_;
};

struct LogChangeAccessModeCmd {
  OB_UNIS_VERSION(1);
public:
  LogChangeAccessModeCmd();
  LogChangeAccessModeCmd(const common::ObAddr &src,
                         const int64_t ls_id,
                         const int64_t mode_version,
                         const palf::AccessMode &access_mode,
                         const share::SCN &ref_scn);
  ~LogChangeAccessModeCmd()
  {
    reset();
  }
  bool is_valid() const;
  void reset();
  TO_STRING_KV(K_(src), K_(ls_id), K_(mode_version), \
      K_(access_mode), K_(ref_scn));
  common::ObAddr src_;
  int64_t ls_id_;
  int64_t mode_version_;
  palf::AccessMode access_mode_;
  share::SCN ref_scn_;
};

struct LogFlashbackMsg {
  OB_UNIS_VERSION(1);
public:
  LogFlashbackMsg();
  LogFlashbackMsg(const uint64_t src_tenant_id,
                  const common::ObAddr &src,
                  const int64_t ls_id,
                  const int64_t mode_version,
                  const share::SCN &flashback_scn,
                  const bool is_flashback_req);
  ~LogFlashbackMsg()
  {
    reset();
  }
  bool is_valid() const;
  void reset();
  bool is_flashback_req() const { return is_flashback_req_; }
  TO_STRING_KV(K_(src_tenant_id), K_(src), K_(ls_id), K_(mode_version), K_(flashback_scn), K_(is_flashback_req));
  uint64_t src_tenant_id_;
  common::ObAddr src_;
  int64_t ls_id_;
  int64_t mode_version_;
  share::SCN flashback_scn_;
  bool is_flashback_req_;
};
} // end namespace logservice
}// end namespace oceanbase

#endif
