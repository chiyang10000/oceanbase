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

#include "log_group_buffer.h"
#include "share/rc/ob_tenant_base.h"
#include "log_writer_utils.h"

namespace oceanbase
{
using namespace share;
namespace palf
{
LogGroupBuffer::LogGroupBuffer()
{
  reset();
}
LogGroupBuffer::~LogGroupBuffer()
{
  destroy();
}

void LogGroupBuffer::reset()
{
  is_inited_ = false;
  start_lsn_.reset();
  reuse_lsn_.reset();
  data_buf_ = NULL;
  ATOMIC_STORE(&reserved_buffer_size_, 0);
  ATOMIC_STORE(&available_buffer_size_, 0);
}

int LogGroupBuffer::init(const LSN &start_lsn)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (!start_lsn.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid arguments", K(ret), K(start_lsn));
  } else {
    int64_t group_buffer_size = FOLLOWER_DEFAULT_GROUP_BUFFER_SIZE;
    // omt::ObTenantConfigGuard tenant_config(TENANT_CONF(tenant_id));
    // if (!tenant_config.is_valid()) {
    //  PALF_LOG(WARN, "get tenant config failed", K(ret), K(tenant_id));
    //  // TODO: add tenant config
    //  // group_buffer_size = tenant_config->_log_groupgation_buffer_size;
    //}
    ObMemAttr mem_attr(PALF_ENV_ID, "LogGroupBuffer");
    if (NULL == (data_buf_ = static_cast<char *>(mtl_malloc(group_buffer_size, mem_attr)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      PALF_LOG(ERROR, "alloc memory failed", K(ret));
    } else {
      memset(data_buf_, 0, group_buffer_size);
      start_lsn_ = start_lsn;
      reuse_lsn_ = start_lsn;
      ATOMIC_STORE(&reserved_buffer_size_, group_buffer_size);
      ATOMIC_STORE(&available_buffer_size_, group_buffer_size);
      is_inited_ = true;
    }
    if (OB_FAIL(ret) && NULL != data_buf_) {
      mtl_free(data_buf_);
      data_buf_ = NULL;
    }
    PALF_LOG(INFO, "LogGroupBuffer init finished", K(ret), K_(start_lsn), KP(data_buf_),
        K_(reserved_buffer_size), K_(available_buffer_size));
  }
  return ret;
}

void LogGroupBuffer::destroy()
{
  PALF_LOG(INFO, "LogGroupBuffer destroy", K(is_inited_), K_(start_lsn), KP(data_buf_), K_(reserved_buffer_size));
  is_inited_ = false;
  start_lsn_.reset();
  reuse_lsn_.reset();
  if (NULL != data_buf_) {
    mtl_free(data_buf_);
    data_buf_ = NULL;
  }
  ATOMIC_STORE(&reserved_buffer_size_, 0);
  ATOMIC_STORE(&available_buffer_size_, 0);
}

int LogGroupBuffer::get_buffer_pos_(const LSN &lsn,
                                    int64_t &start_pos) const
{
  // 根据lsn获取buffer中对应的位置
  // 转换方法依赖buffer start_lsn之后的文件size不变
  // 如果文件size发生了变化，buffer append到切文件位置时需要做barrier处理
  // 等前一个文件都pop出去后整体reuse为下一个文件
  int ret = OB_SUCCESS;
  LSN start_lsn;
  get_buffer_start_lsn_(start_lsn);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (!lsn.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid arguments", K(ret), K(lsn));
  } else if (lsn < start_lsn) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "lsn is less than start_lsn", K(ret), K(lsn), K_(start_lsn));
  } else {
    const int64_t diff_len = lsn - start_lsn;
    assert(diff_len >= 0);
    // Use reserved_buffer_size_ to calculate dest pos.
    start_pos = diff_len % get_reserved_buffer_size();
  }
  return ret;
}

bool LogGroupBuffer::can_handle_new_log(const LSN &lsn,
                                        const int64_t total_len) const
{
  bool bool_ret = false;
  if (IS_NOT_INIT) {
  } else if (!lsn.is_valid() || total_len <= 0) {
    PALF_LOG(WARN, "invalid arguments", K(bool_ret), K(lsn), K(total_len));
  } else {
    LSN fake_ref_lsn(LOG_MAX_LSN_VAL);
    bool_ret = can_handle_new_log(lsn, total_len, fake_ref_lsn);
  }
  return bool_ret;
}

bool LogGroupBuffer::can_handle_new_log(const LSN &lsn,
                                        const int64_t total_len,
                                        const LSN &ref_reuse_lsn) const
{
  bool bool_ret = false;
  const LSN end_lsn = lsn + total_len;
  LSN start_lsn, reuse_lsn;
  get_buffer_start_lsn_(start_lsn);
  get_reuse_lsn_(reuse_lsn);
  reuse_lsn = MIN(reuse_lsn, ref_reuse_lsn);
  if (IS_NOT_INIT) {
  } else if (!lsn.is_valid() || total_len <= 0 || !ref_reuse_lsn.is_valid()) {
    PALF_LOG(WARN, "invalid arguments", K(bool_ret), K(lsn), K(total_len), K(ref_reuse_lsn));
  } else if (lsn < start_lsn) {
    PALF_LOG(WARN, "lsn is less than start_lsn", K(bool_ret), K(lsn), K_(start_lsn));
  } else if (end_lsn > reuse_lsn + get_available_buffer_size()) {
    PALF_LOG(WARN, "end_lsn is larger than max reuse pos", K(bool_ret), K(lsn), K(end_lsn),
        K(reuse_lsn), K_(available_buffer_size));
  } else {
    bool_ret = true;
  }
  return bool_ret;
}

// 获取完整的log buffer(含log_group_entry_header)
int LogGroupBuffer::get_log_buf(const LSN &lsn, const int64_t total_len, LogWriteBuf &log_buf)
{
  int ret = OB_SUCCESS;
  int64_t start_pos = 0;
  LSN start_lsn;
  get_buffer_start_lsn_(start_lsn);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (!lsn.is_valid() || total_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid arguments", K(ret), K(lsn), K(total_len));
  } else if (lsn < start_lsn) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "lsn is less than start_lsn", K(ret), K(lsn), K_(start_lsn));
  } else if (OB_FAIL(get_buffer_pos_(lsn, start_pos))) {
    PALF_LOG(WARN, "get_buffer_pos_ failed", K(ret), K(lsn));
  } else {
    const int64_t group_buf_tail_len = get_reserved_buffer_size() - start_pos;
    const int64_t first_part_len = min(group_buf_tail_len, total_len);
    if (OB_FAIL(log_buf.push_back(data_buf_ + start_pos, first_part_len))) {
      PALF_LOG(WARN, "log_buf push_back failed", K(ret), K(lsn));
    } else if (total_len > first_part_len
               && OB_FAIL(log_buf.push_back(data_buf_, total_len - first_part_len))) {
      PALF_LOG(WARN, "log_buf push_back failed", K(ret), K(lsn));
    } else {
      // do nothing
    }
    PALF_LOG(TRACE, "get_log_buf finished", K(ret), K(lsn), K(start_pos), K(total_len), K(group_buf_tail_len), K(first_part_len),
        "second_part_len", total_len - first_part_len, K(log_buf));
  }
  return ret;
}

int LogGroupBuffer::wait(const LSN &lsn, const int64_t data_len)
{
  int ret = OB_SUCCESS;
  const LSN end_lsn = lsn + data_len;
  LSN start_lsn, reuse_lsn;
  get_buffer_start_lsn_(start_lsn);
  get_reuse_lsn_(reuse_lsn);
  const int64_t buf_size = get_available_buffer_size();
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (!lsn.is_valid() || data_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid arguments", K(ret), K(lsn), K(data_len));
  } else if (lsn < start_lsn) {
    ret = OB_ERR_UNEXPECTED;
    PALF_LOG(WARN, "lsn is less than start_lsn", K(ret), K(lsn), K(start_lsn));
  } else if (end_lsn > reuse_lsn + buf_size) {
    ret = OB_EAGAIN;
    PALF_LOG(WARN, "need retry", K(ret), K(lsn), K(data_len), K(end_lsn), K(reuse_lsn), K(start_lsn));
  } else {
    // wait success
  }
  return ret;
}

int LogGroupBuffer::fill(const LSN &lsn,
                         const char *data,
                         const int64_t data_len)
{
  int ret = OB_SUCCESS;
  int64_t start_pos = 0;
  const LSN end_lsn = lsn + data_len;
  LSN start_lsn, reuse_lsn;
  get_buffer_start_lsn_(start_lsn);
  get_reuse_lsn_(reuse_lsn);
  const int64_t reserved_buf_size = get_reserved_buffer_size();
  const int64_t available_buf_size = get_available_buffer_size();
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (!lsn.is_valid() || NULL == data || data_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid arguments", K(ret), K(lsn), KP(data), K(data_len));
  } else if (lsn < start_lsn) {
    ret = OB_ERR_UNEXPECTED;
    PALF_LOG(WARN, "lsn is less than start_lsn", K(ret), K(lsn), K(end_lsn), K(start_lsn), K(reuse_lsn));
  } else if (end_lsn <= reuse_lsn) {
    // 要填充的终点预期应该比buffer复用的起点大
    ret = OB_ERR_UNEXPECTED;
    PALF_LOG(WARN, "end_lsn is less than reuse_lsn", K(ret), K(lsn), K(end_lsn), K(start_lsn), K(reuse_lsn));
  } else if (end_lsn > reuse_lsn + available_buf_size) {
    // double check: 要填充的终点超过了buffer可复用的范围
    ret = OB_EAGAIN;
    PALF_LOG(WARN, "end_lsn is greater than reuse end pos", K(ret), K(lsn), K(end_lsn), K(reuse_lsn), K(available_buf_size));
  } else if (OB_FAIL(get_buffer_pos_(lsn, start_pos))) {
    PALF_LOG(WARN, "get_buffer_pos_ failed", K(ret), K(lsn));
  } else {
    const int64_t group_buf_tail_len = reserved_buf_size - start_pos;
    int64_t first_part_len = min(group_buf_tail_len, data_len);
    memcpy(data_buf_ + start_pos, data, first_part_len);
    if (data_len > first_part_len) {
      // seeking to buffer's beginning
      memcpy(data_buf_, data + first_part_len, data_len - first_part_len);
    }
    PALF_LOG(TRACE, "fill group buffer success", K(ret), K(lsn), K(data_len), K(start_pos), K(group_buf_tail_len),
        K(first_part_len), "second_part_len", data_len - first_part_len, KP(data_buf_));
  }
  return ret;
}

int LogGroupBuffer::fill_padding(const LSN &lsn,
                                 const int64_t padding_len)
{
  int ret = OB_SUCCESS;
  int64_t start_pos = 0;
  const LSN end_lsn = lsn + padding_len;
  LSN start_lsn, reuse_lsn;
  get_buffer_start_lsn_(start_lsn);
  get_reuse_lsn_(reuse_lsn);
  const int64_t reserved_buf_size = get_reserved_buffer_size();
  const int64_t available_buf_size = get_available_buffer_size();
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (!lsn.is_valid() || padding_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid arguments", K(ret), K(lsn), K(padding_len));
  } else if (lsn < start_lsn) {
    ret = OB_ERR_UNEXPECTED;
    PALF_LOG(WARN, "lsn is less than start_lsn", K(ret), K(lsn), K_(start_lsn));
  } else if (end_lsn <= reuse_lsn) {
    // 要填充的终点预期应该比buffer复用的起点大
    ret = OB_ERR_UNEXPECTED;
    PALF_LOG(WARN, "end_lsn is less than reuse_lsn", K(ret), K(lsn), K(end_lsn), K(reuse_lsn));
  } else if (end_lsn > reuse_lsn + available_buf_size) {
    // double check: 要填充的终点超过了buffer可复用的范围
    // 因为wait()成功后调用fill()之前buffer的start_lsn_可能发生更新
    ret = OB_EAGAIN;
    PALF_LOG(WARN, "end_lsn is greater than reuse end pos", K(ret), K(lsn), K(end_lsn), K(reuse_lsn), K(available_buf_size));
  } else if (OB_FAIL(get_buffer_pos_(lsn, start_pos))) {
    PALF_LOG(WARN, "get_buffer_pos_ failed", K(ret), K(lsn));
  } else {
    const int64_t group_buf_tail_len = reserved_buf_size - start_pos;
    int64_t first_part_len = min(group_buf_tail_len, padding_len);
    memset(data_buf_ + start_pos, 0, first_part_len);
    if (padding_len > first_part_len) {
      // seeking to buffer's beginning
      memset(data_buf_, 0, padding_len - first_part_len);
    }
    PALF_LOG(INFO, "fill padding success", K(ret), K(lsn), K(padding_len), K(start_pos), K(group_buf_tail_len),
        K(first_part_len), "second_part_len", padding_len - first_part_len);
  }
  return ret;
}

void LogGroupBuffer::get_buffer_start_lsn_(LSN &start_lsn) const
{
  start_lsn.val_ = ATOMIC_LOAD(&start_lsn_.val_);
}

void LogGroupBuffer::get_reuse_lsn_(LSN &reuse_lsn) const
{
  reuse_lsn.val_ = ATOMIC_LOAD(&reuse_lsn_.val_);
}

int LogGroupBuffer::check_log_buf_wrapped(const LSN &lsn, const int64_t log_len, bool &is_buf_wrapped) const
{
  int ret = OB_SUCCESS;
  is_buf_wrapped = false;
  int64_t start_pos = 0;
  LSN start_lsn;
  get_buffer_start_lsn_(start_lsn);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (!lsn.is_valid() || log_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(WARN, "invalid arguments", K(ret), K(lsn), K(log_len));
  } else if (lsn < start_lsn) {
    PALF_LOG(WARN, "lsn is less than start_lsn", K(ret), K(lsn), K_(start_lsn));
  } else if (OB_FAIL(get_buffer_pos_(lsn, start_pos))) {
    PALF_LOG(WARN, "get_buffer_pos_ failed", K(ret), K(lsn));
  } else if (start_pos + log_len > get_reserved_buffer_size()) {
    is_buf_wrapped = true;
    PALF_LOG(INFO, "this log buf is wrapped", K(ret), K(lsn), K(log_len), K(start_pos), K_(reserved_buffer_size));
  } else {
    // do nothing
  }
  return ret;
}

// 依赖palf_handle_impl的写锁确保调用本接口期间无并发更新group_buffer操作
int LogGroupBuffer::to_leader()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (LEADER_DEFAULT_GROUP_BUFFER_SIZE == get_available_buffer_size()) {
    ret = OB_STATE_NOT_MATCH;
    PALF_LOG(WARN, "available_buffer_size_ is already for leader", K(ret), K_(available_buffer_size));
  } else {
    ATOMIC_STORE(&available_buffer_size_, LEADER_DEFAULT_GROUP_BUFFER_SIZE);
  }
  PALF_LOG(INFO, "to_leader finished", K(ret), K_(available_buffer_size), K_(reserved_buffer_size));
  return ret;
}

// 依赖palf_handle_impl的写锁确保调用本接口期间无并发更新group_buffer操作
int LogGroupBuffer::to_follower()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (FOLLOWER_DEFAULT_GROUP_BUFFER_SIZE == get_available_buffer_size()) {
  // The case is maybe : pending -> reconfirm fail -> pending.
  PALF_LOG(INFO, "current buffer_size is already for follower, no need execute again", K(ret),
      K_(available_buffer_size));
  } else {
    // Here we cannot reset buffer, because some data may be waiting to flush.
    ATOMIC_STORE(&available_buffer_size_, FOLLOWER_DEFAULT_GROUP_BUFFER_SIZE);
  }
  PALF_LOG(INFO, "to_follower finished", K(ret), K_(available_buffer_size), K_(reserved_buffer_size));
  return ret;
}

int64_t LogGroupBuffer::get_available_buffer_size() const
{
  // This available_buffer_size_ will change according to role.
  return ATOMIC_LOAD(&available_buffer_size_);
}

int64_t LogGroupBuffer::get_reserved_buffer_size() const
{
  // This reserved_buffer_size_ will not change during role switch.
  return ATOMIC_LOAD(&reserved_buffer_size_);
}

int LogGroupBuffer::inc_update_reuse_lsn(const LSN &new_reuse_lsn)
{
  int ret = OB_SUCCESS;
  if (!new_reuse_lsn.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(ERROR, "invalid argumetns", K(new_reuse_lsn));
  } else {
    LSN curr_reuse_lsn;
    get_reuse_lsn_(curr_reuse_lsn);
    while (new_reuse_lsn > curr_reuse_lsn) {
      if (ATOMIC_BCAS(&reuse_lsn_.val_, curr_reuse_lsn.val_, new_reuse_lsn.val_)) {
        break;
      } else {
        get_reuse_lsn_(curr_reuse_lsn);
      }
    }
    PALF_LOG(TRACE, "inc_update_reuse_lsn success", K(curr_reuse_lsn), K(new_reuse_lsn));
  }
  return ret;
}

int LogGroupBuffer::set_reuse_lsn(const LSN &new_reuse_lsn)
{
  int ret = OB_SUCCESS;
  if (!new_reuse_lsn.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(ERROR, "invalid argumetns", K(new_reuse_lsn));
  } else {
    LSN old_reuse_lsn;
    get_reuse_lsn_(old_reuse_lsn);
    ATOMIC_STORE(&reuse_lsn_.val_, new_reuse_lsn.val_);
    PALF_LOG(INFO, "set_reuse_lsn success", K(old_reuse_lsn), K(new_reuse_lsn));
  }
  return ret;
}
}  // namespace palf
}  // namespace oceanbase
