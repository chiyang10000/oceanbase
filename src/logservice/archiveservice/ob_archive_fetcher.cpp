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

#include <cstdint>
#include "lib/time/ob_time_utility.h"
#include "lib/utility/ob_macro_utils.h"
#include "ob_archive_fetcher.h"
#include "lib/ob_define.h"
#include "lib/ob_errno.h"
#include "lib/thread/ob_thread_name.h"        // lib::set_thread_name
#include "logservice/ob_log_service.h"        // ObLogService
#include "logservice/palf/log_group_entry.h"  // LogGroupEntry
#include "logservice/palf_handle_guard.h"     // PalfHandleGuard
#include "ob_archive_allocator.h"             // ObArchiveAllocator
#include "ob_archive_define.h"                // ArchiveWorkStation
#include "ob_archive_sender.h"                // ObArchiveSender
#include "ob_ls_mgr.h"                        // ObArchiveLSMgr
#include "ob_archive_task.h"                  // ObArchive.*Task
#include "ob_ls_task.h"                       // ObLSArchiveTask
#include "ob_archive_round_mgr.h"             // ObArchiveRoundMgr
#include "ob_archive_util.h"
#include "ob_archive_sequencer.h"             // ObArchivesSequencer
#include "objit/common/ob_item_type.h"        // print
#include "share/ob_debug_sync.h"              // DEBUG_SYNC
#include "share/ob_debug_sync_point.h"        // LOG_ARCHIVE_PUSH_LOG
#include "share/ob_ls_id.h"

namespace oceanbase
{
namespace archive
{
using namespace oceanbase::logservice;
using namespace oceanbase::share;
using namespace oceanbase::palf;

ObArchiveFetcher::ObArchiveFetcher() :
  inited_(false),
  tenant_id_(OB_INVALID_TENANT_ID),
  unit_size_(0),
  piece_interval_(0),
  genesis_scn_(),
  base_piece_id_(0),
  need_compress_(false),
  compress_type_(INVALID_COMPRESSOR),
  need_encrypt_(false),
  log_service_(NULL),
  allocator_(NULL),
  archive_sender_(NULL),
  ls_mgr_(NULL),
  round_mgr_(NULL),
  task_queue_(),
  fetch_cond_()
{}

ObArchiveFetcher::~ObArchiveFetcher()
{
  ARCHIVE_LOG(INFO, "ObArchiveFetcher destroy");
  destroy();
}

int ObArchiveFetcher::init(const uint64_t tenant_id,
    ObLogService *log_service,
    ObArchiveAllocator *allocator,
    ObArchiveSender *sender,
    ObArchiveSequencer *sequencer,
    ObArchiveLSMgr *mgr,
    ObArchiveRoundMgr *round_mgr)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(inited_)) {
    ret = OB_INIT_TWICE;
    ARCHIVE_LOG(ERROR, "ObArchiveFetcher has been initialized", K(ret));
  } else if (OB_UNLIKELY(OB_INVALID_TENANT_ID == tenant_id)
      || OB_ISNULL(log_service_ = log_service)
      || OB_ISNULL(allocator_ = allocator)
      || OB_ISNULL(archive_sender_ = sender)
      || OB_ISNULL(archive_sequencer_ = sequencer)
      || OB_ISNULL(ls_mgr_ = mgr)
      || OB_ISNULL(round_mgr_ = round_mgr)) {
    ret = OB_INVALID_ARGUMENT;
    ARCHIVE_LOG(WARN, "invalid argument", K(ret), K(tenant_id), K(log_service),
        K(allocator), K(sender), K(sequencer), K(mgr), K(round_mgr));
  } else if (OB_FAIL(task_queue_.init(1024 * 1024, "ArcFetchQueue", tenant_id))) {
    ARCHIVE_LOG(WARN, "task queue init failed", K(ret));
  } else {
    tenant_id_ = tenant_id;
    inited_ = true;
  }

  if (OB_FAIL(ret) && ! inited_) {
    destroy();
  }
  return ret;
}

void ObArchiveFetcher::destroy()
{
  int ret = OB_SUCCESS;
  stop();
  wait();
  if (inited_) {
    void *data = NULL;
    while (OB_SUCC(ret) && 0 < task_queue_.size()) {
      if (OB_FAIL(task_queue_.pop(data))) {
        ARCHIVE_LOG(WARN, "pop task failed", K(ret));
      } else {
        ObArchiveLogFetchTask *task = static_cast<ObArchiveLogFetchTask *>(data);
        ARCHIVE_LOG(INFO, "free residual log fetch task when fetcher destroy", K(tenant_id_), KPC(task));
        free_log_fetch_task(task);
      }
    }
    task_queue_.reset();
    task_queue_.destroy();
    tenant_id_ = OB_INVALID_TENANT_ID;
    unit_size_ = 0;
    piece_interval_ = OB_INVALID_TIMESTAMP;
    genesis_scn_.reset();
    base_piece_id_ = 0;
    need_compress_ = false;
    compress_type_ = INVALID_COMPRESSOR;
    need_encrypt_ = false;
    log_service_ = NULL;
    allocator_ = NULL;
    archive_sender_ = NULL;
    ls_mgr_ = NULL;
    round_mgr_ = NULL;
    inited_ = false;
  }
}

int ObArchiveFetcher::start()
{
  int ret = OB_SUCCESS;
  ObThreadPool::set_run_wrapper(MTL_CTX(), lib::ThreadCGroup::BACK_CGROUP);
  if (OB_UNLIKELY(! inited_)) {
    ARCHIVE_LOG(ERROR, "ObArchiveFetcher not init");
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(ObThreadPool::start())) {
    ARCHIVE_LOG(WARN, "ObArchiveFetcher start fail", K(ret));
  } else {
    ARCHIVE_LOG(INFO, "ObArchiveFetcher start succ");
  }
  return ret;
}

void ObArchiveFetcher::stop()
{
  ARCHIVE_LOG(INFO, "ObArchiveFetcher stop");
  ObThreadPool::stop();
}

void ObArchiveFetcher::wait()
{
  ARCHIVE_LOG(INFO, "ObArchiveFetcher wait");
  ObThreadPool::wait();
}

int ObArchiveFetcher::set_archive_info(
    const int64_t interval_us,
    const SCN &genesis_scn,
    const int64_t base_piece_id,
    const int64_t unit_size,
    const bool need_compress,
    const ObCompressorType type,
    const bool need_encrypt)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(interval_us <= 0 || !genesis_scn.is_valid() || base_piece_id < 1 || unit_size <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    ARCHIVE_LOG(WARN, "invalid argument", K(ret), K(interval_us), K(genesis_scn), K(base_piece_id), K(unit_size));
  } else {
    piece_interval_ = interval_us;
    UNUSED(need_compress);
    UNUSED(type);
    UNUSED(need_encrypt);
    genesis_scn_ = genesis_scn;
    base_piece_id_ = base_piece_id;
    unit_size_ = unit_size;
    unit_size_ = 1;
  }
  return ret;
}

void ObArchiveFetcher::clear_archive_info()
{
  piece_interval_ = 0;
  need_compress_ = false;
  unit_size_ = 0;
  ARCHIVE_LOG(INFO, "fetcher clear info succ");
}

void ObArchiveFetcher::signal()
{
  fetch_cond_.signal();
}

int ObArchiveFetcher::submit_log_fetch_task(ObArchiveLogFetchTask *task)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(! inited_)) {
    ret = OB_NOT_INIT;
    ARCHIVE_LOG(WARN, "archive fetcher not init", K(ret), K(inited_));
  } else if (OB_ISNULL(task) || !task->is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    ARCHIVE_LOG(WARN, "invalid argument", K(ret), KPC(task));
  } else {
    RETRY_FUNC_ON_ERROR(OB_SIZE_OVERFLOW, has_set_stop(), task_queue_, push, task);
  }
  if (OB_SUCC(ret)) {
    ARCHIVE_LOG(INFO, "submit log fetch task succ", KPC(task));
  }
  return ret;
}

ObArchiveLogFetchTask *ObArchiveFetcher::alloc_log_fetch_task()
{
  ObArchiveLogFetchTask *task = NULL;
  if (OB_UNLIKELY(! inited_)) {
    ARCHIVE_LOG(WARN, "ObArchiveFetcher not init");
  } else if (OB_ISNULL(allocator_)) {
    ARCHIVE_LOG(ERROR, "allocator_ is NULL", K(allocator_));
  } else {
    task = allocator_->alloc_log_fetch_task();
  }
  return task;
}

void ObArchiveFetcher::free_log_fetch_task(ObArchiveLogFetchTask *task)
{
  if (NULL != allocator_ && NULL != task) {
    allocator_->free_log_fetch_task(task);
    task = NULL;
  }
}

int64_t ObArchiveFetcher::get_log_fetch_task_count() const
{
  return task_queue_.size();
}

int ObArchiveFetcher::modify_thread_count(const int64_t thread_count)
{
  int ret = OB_SUCCESS;
  int64_t count = thread_count;
  if (thread_count < MIN_FETCHER_THREAD_COUNT) {
    count = MIN_FETCHER_THREAD_COUNT;
  } else if (thread_count > MAX_FETCHER_THREAD_COUNT) {
    count = MAX_FETCHER_THREAD_COUNT;
  }
  if (count == get_thread_count()) {
    // do nothing
  } else if (OB_FAIL(set_thread_count(count))) {
    ARCHIVE_LOG(WARN, "set thread count failed", K(ret));
  } else {
    ARCHIVE_LOG(INFO, "set thread count succ", K(count));
  }
  return ret;
}

void ObArchiveFetcher::run1()
{
  ARCHIVE_LOG(INFO, "ObArchiveFetcher thread start");
  lib::set_thread_name("ArcFetcher");
  ObCurTraceId::init(GCONF.self_addr_);

  if (OB_UNLIKELY(! inited_)) {
    ARCHIVE_LOG(ERROR, "ObArchiveFetcher not init");
  } else {
    while (!has_set_stop() && !(OB_NOT_NULL(&lib::Thread::current()) ? lib::Thread::current().has_set_stop() : false)) {
      int64_t begin_tstamp = ObTimeUtility::current_time();
      do_thread_task_();
      int64_t end_tstamp = ObTimeUtility::current_time();
      int64_t wait_interval = THREAD_RUN_INTERVAL - (end_tstamp - begin_tstamp);
      if (wait_interval > 0) {
        fetch_cond_.timedwait(wait_interval);
      }
    }
  }
  ARCHIVE_LOG(INFO, "ObArchiveFetcher thread end");
}

void ObArchiveFetcher::do_thread_task_()
{
  int ret = OB_SUCCESS;
  int64_t size = task_queue_.size();
  for (int64_t i = 0; i < size && OB_SUCC(ret) && !has_set_stop(); i++) {
    if (OB_FAIL(handle_single_task_())) {
      ARCHIVE_LOG(WARN, "handle single task failed", K(ret));
    }
  }
}

int ObArchiveFetcher::handle_single_task_()
{
  int ret = OB_SUCCESS;
  void *data = NULL;

  if (OB_FAIL(task_queue_.pop(data))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ARCHIVE_LOG(TRACE, "no task exist, just skip", K(ret));
      ret = OB_SUCCESS;
    } else {
      ARCHIVE_LOG(WARN, "pop failed", K(ret));
    }
  } else if (OB_ISNULL(data)) {
    ret = OB_ERR_UNEXPECTED;
    ARCHIVE_LOG(ERROR, "data is NULL", K(ret), K(data));
  } else {
    ObArchiveLogFetchTask *task = static_cast<ObArchiveLogFetchTask*>(data);
    ObLSID id = task->get_ls_id_copy();
    ArchiveKey key = task->get_station().get_round();

    // task will be submit to fetch_log_queue or re-submit to handle or free due to fatal error
    // task will not be safe after handle
    if (OB_FAIL(handle_log_fetch_task_(*task))) {
      ARCHIVE_LOG(WARN, "handle failed", K(ret), K(id));
    } else {
      ARCHIVE_LOG(INFO, "handle task succ", K(id));
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(try_consume_fetch_log_(id))) {
        ARCHIVE_LOG(WARN, "try consume task status failed", K(ret), K(id));
      } else {
        archive_sequencer_->signal();
        ARCHIVE_LOG(INFO, "try consume task status succ", K(id));
      }
    }

    // only report error here
    if (OB_FAIL(ret)) {
      handle_log_fetch_ret_(id, key, ret);
    }
  }
  return ret;
}

int ObArchiveFetcher::handle_log_fetch_task_(ObArchiveLogFetchTask &task)
{
  int ret = OB_SUCCESS;
  bool need_delay = false;
  bool submit_log = false;
  PalfGroupBufferIterator iter;
  PalfHandleGuard palf_handle_guard;
  TmpMemoryHelper helper(unit_size_, allocator_);
  ObArchiveSendTask *send_task = NULL;
  const ObLSID &id = task.get_ls_id();
  const ArchiveWorkStation &station = task.get_station();
  ArchiveKey key = station.get_round();
  LSN commit_lsn;
  SCN commit_scn;

  DEBUG_SYNC(BEFORE_ARCHIVE_FETCH_LOG);

  if (! in_normal_status_(key)) {
    // skip
  } else if (OB_UNLIKELY(! task.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    ARCHIVE_LOG(WARN, "invalid task", K(ret), K(task));
  } else if (OB_FAIL(get_max_lsn_scn_(id, commit_lsn, commit_scn))) {
    ARCHIVE_LOG(WARN, "get max lsn scn failed", K(ret), K(id));
  } else if (OB_FAIL(check_need_delay_(id, station, task.get_cur_offset(),
          task.get_end_offset(), commit_lsn, commit_scn, need_delay))) {
    ARCHIVE_LOG(WARN, "check need delay failed", K(ret), K(commit_lsn), K(commit_scn), K(task));
  } else if (need_delay) {
    // just skip
      ARCHIVE_LOG(INFO, "need delay", K(task), K(need_delay));
  } else if (OB_FAIL(init_helper_(task, commit_lsn, helper))) {
    ARCHIVE_LOG(WARN, "init helper failed", K(ret), K(task));
  } else if (OB_FAIL(init_iterator_(task.get_ls_id(), helper, palf_handle_guard, iter))) {
    ARCHIVE_LOG(WARN, "init iterator failed", K(ret), K(task));
  } else if (OB_FAIL(generate_send_buffer_(iter, helper))) {
    ARCHIVE_LOG(WARN, "generate send buffer failed", K(ret), K(task));
  } else if (OB_FAIL(build_send_task_(id, station, helper, send_task))) {
    ARCHIVE_LOG(WARN, "build send task failed", K(ret), K(task), K(helper));
  } else if (OB_FAIL(update_log_fetch_task_(task, helper, send_task))) {
    ARCHIVE_LOG(WARN, "set send task failed", K(ret));
  } else if (OB_FAIL(submit_fetch_log_(task, submit_log))) {
    ARCHIVE_LOG(WARN, "submit send task failed", K(ret), KPC(send_task));
  } else {
    ARCHIVE_LOG(INFO, "handle log fetch task succ", K(task));
  }

  // 0. task submit to sort queue, do nothing
  if (submit_log) {
  }
  // 1. task not complete and encounter error can be solved with retry
  // 2. task not handled
  else if (OB_SUCC(ret) || is_retry_ret_(ret)) {
    if (in_normal_status_(key) && OB_SUCC(submit_residual_log_fetch_task_(task))) {
    } else {
      // 3. task can not re submit to queue, free task
      inner_free_task_(&task);
    }
  }
  // 4. task handled and encounter fatal error
  else {
    inner_free_task_(&task);
  }
  return ret;
}

int ObArchiveFetcher::get_max_lsn_scn_(const ObLSID &id, palf::LSN &lsn, SCN &scn)
{
  int ret = OB_SUCCESS;
  palf::PalfHandleGuard palf_handle_guard;
  if (OB_FAIL(log_service_->open_palf(id, palf_handle_guard))) {
    ARCHIVE_LOG(WARN, "get log service failed", K(ret), K(id));
  } else if (OB_FAIL(palf_handle_guard.get_end_lsn(lsn))) {
    ARCHIVE_LOG(WARN, "get end lsn failed", K(ret), K(id));
  } else if (OB_FAIL(palf_handle_guard.get_end_scn(scn))) {
    ARCHIVE_LOG(WARN, "get end ts ns failed", K(ret), K(id));
  } else {
    ARCHIVE_LOG(INFO, "get end lsn scn succ", K(ret), K(id), K(lsn), K(scn));
  }
  return ret;
}

// 消费LogFetchTask任务条件必须满足条件:
// 1. 日志量足够压缩
// 2. 该日志流归档任务有效(leader/backup_zone)
// 以及以上条件其中一条:
// 3. 日志流delay超过一定阈值
// 4. 可以塞满任务
int ObArchiveFetcher::check_need_delay_(const ObLSID &id,
    const ArchiveWorkStation &station,
    const LSN &cur_lsn,
    const LSN &end_lsn,
    const LSN &commit_lsn,
    const share::SCN &commit_scn,
    bool &need_delay)
{
  int ret = OB_SUCCESS;
  bool data_enough = false;
  bool data_full = false;
  LSN offset;
  SCN fetch_scn;
  need_delay = false;
  int64_t send_task_count = 0;

  if (FALSE_IT(check_capacity_enough_(commit_lsn, cur_lsn, end_lsn, data_enough, data_full))) {
  } else if (data_full) {
    // data is full, do archive
  } else if (! data_enough) {
    // data not enough to fill unit, just wait
    need_delay = true;
    ARCHIVE_LOG(TRACE, "data not enough, need delay", K(id), K(station), K(commit_lsn),
        K(cur_lsn), K(end_lsn), K(data_enough));
  } else {
    GET_LS_TASK_CTX(ls_mgr_, id) {
      if (OB_FAIL(ls_archive_task->get_fetcher_progress(station, offset, fetch_scn))) {
        ARCHIVE_LOG(WARN, "get fetch progress failed", K(ret), K(id), K(station));
      } else if (OB_FAIL(ls_archive_task->get_send_task_count(station, send_task_count))) {
        ARCHIVE_LOG(WARN, "get send task count failed", K(ret), K(id), K(station));
      } else if (true == (need_delay = (send_task_count >= MAX_LS_SEND_TASK_COUNT_LIMIT))) {
        ARCHIVE_LOG(TRACE, "send_task_count exceed threshold, need delay",
            K(id), K(station), K(send_task_count));
      } else if (true == (need_delay = ! check_scn_enough_(fetch_scn, commit_scn))) {
        ARCHIVE_LOG(TRACE, "scn not enough, need delay", K(id),
            K(station), K(fetch_scn), K(commit_scn));
      }
    }
  }
  return ret;
}

void ObArchiveFetcher::check_capacity_enough_(const LSN &commit_lsn,
    const LSN &cur_lsn,
    const LSN &end_lsn,
    bool &data_enough,
    bool &data_full)
{
  // 已有足够大用以压缩加密单元或者到达归档文件尾(也是ob日志文件尾)
  data_full = end_lsn <= commit_lsn;
  data_enough = data_full || commit_lsn >= cur_lsn + unit_size_;
}

bool ObArchiveFetcher::check_scn_enough_(const SCN &fetch_scn, const SCN &end_scn) const
{
  return true;
}

int ObArchiveFetcher::init_helper_(ObArchiveLogFetchTask &task, const LSN &commit_lsn, TmpMemoryHelper &helper)
{
  int ret = OB_SUCCESS;
  LSN start_offset;
  const ObLSID &id = task.get_ls_id();
  const LSN &end_offset = task.get_end_offset();
  const ObArchivePiece &cur_piece = task.get_piece();
  const ObArchivePiece &next_piece = task.get_next_piece();
  ObArchivePiece *piece = NULL;

  // 1. inital task, piece info is not set
  if (! next_piece.is_valid() && ! cur_piece.is_valid()) {
    // not init initial piece info
  }
  // 2. next piece is set, cur piece reach end
  else if (next_piece.is_valid()) {
    piece = &const_cast<ObArchivePiece &>(next_piece);
  }
  // 3. next piece is invalid, and cur piece is valid, cur piece not reach end
  else {
    piece = &const_cast<ObArchivePiece &>(cur_piece);
  }

  start_offset = task.get_cur_offset();
  if (OB_FAIL(helper.init(tenant_id_, id, start_offset, end_offset, commit_lsn, piece))) {
    ARCHIVE_LOG(WARN, "helper init failed", K(ret), K(start_offset), K(end_offset));
  } else {
    ARCHIVE_LOG(TRACE, "init helper succ", K(helper));
  }
  return ret;
}

int ObArchiveFetcher::init_iterator_(const ObLSID &id,
    const TmpMemoryHelper &helper,
    PalfHandleGuard &palf_handle_guard,
    PalfGroupBufferIterator &iter)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(log_service_->open_palf(id, palf_handle_guard))) {
    if (OB_LS_NOT_EXIST == ret) {
      ARCHIVE_LOG(WARN, "ls not exist", K(ret), K(id), "tenant_id", MTL_ID());
      ret = OB_LOG_ARCHIVE_LEADER_CHANGED;
    } else {
      ARCHIVE_LOG(WARN, "open ls failed", K(ret), K(id), K(helper));
    }
  } else if (OB_FAIL(palf_handle_guard.seek(helper.get_start_offset(), iter))) {
    ARCHIVE_LOG(WARN, "iterator seek failed", K(ret), K(id), K(helper));
  } else {
    ARCHIVE_LOG(TRACE, "init iterator succ", K(id), K(helper));
  }
  return ret;
}

// generate send buffer 前一定已经有足够数据
int ObArchiveFetcher::generate_send_buffer_(PalfGroupBufferIterator &iter, TmpMemoryHelper &helper)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  int64_t buf_len = 0;
  LogGroupEntry entry;
  LSN offset;
  bool piece_change = false;  // 遇到更大piece, 不同piece内数据分开, 当前piece已到终点
  bool iter_end = false;    // 任务已处理到end_lsn
  SCN max_scn;
  const char *buffer = NULL;

  ret = get_max_archive_point_(max_scn);
  const int64_t start_ts = common::ObTimeUtility::fast_current_time();
  while (OB_SUCC(ret) && ! iter_end && ! piece_change && ! has_set_stop()) {
    buffer = NULL;
    if (OB_FAIL(iter.next(max_scn))) {
      if (OB_ITER_END == ret) {
        ARCHIVE_LOG(TRACE, "iterate log entry to end", K(ret), K(iter));
      } else {
        ARCHIVE_LOG(WARN, "iterate log entry failed", K(ret), K(iter));
      }
    } else if (OB_FAIL(iter.get_entry(buffer, entry, offset))) {
      ARCHIVE_LOG(WARN, "get entry failed", K(ret));
    } else if (OB_UNLIKELY(! entry.check_integrity())) {
      ret = OB_INVALID_DATA;
      ARCHIVE_LOG(ERROR, "iterate buf not valid", K(ret), K(entry));
    } else if (OB_UNLIKELY(entry.get_serialize_size() > helper.get_capaicity())) {
      ret = OB_ERR_UNEXPECTED;
       ARCHIVE_LOG(ERROR, "iterate buf not valid", K(ret), K(helper), K(entry));
    } else {
      // 由于归档按照palf block起始LSN开始归档, 因此存在部分日志其scn是小于归档round_start_scn的,
      // 对于这部分日志, 归档到第一个piece
      const SCN scn = entry.get_scn() > genesis_scn_ ? entry.get_scn() : genesis_scn_;
      ObArchivePiece piece(scn, piece_interval_, genesis_scn_, base_piece_id_);
      if (OB_FAIL(fill_helper_piece_if_empty_(piece, helper))) {
        ARCHIVE_LOG(WARN, "fill helper piece failed", K(ret), K(piece), K(helper));
      } else if (OB_UNLIKELY(check_piece_inc_(piece, helper.get_piece()))) {
        // 日志所属piece超过当前task piece, 该日志不被归档到task描述piece下
        piece_change = true;
        if (OB_FAIL(set_next_piece_(helper))) {
          ARCHIVE_LOG(WARN, "helper set next piece failed", K(ret));
        } else {
          ARCHIVE_LOG(INFO, "set next piece succ", K(piece), K(entry), K(helper));
        }
      } else if (OB_FAIL(append_log_entry_(buffer, entry, helper))) {
        ARCHIVE_LOG(WARN, "helper append buf failed", K(ret));
      } else {
        iter_end = helper.reach_end();
        ARCHIVE_LOG(TRACE, "append log entry succ", K(entry), K(helper), K(iter));
      }

      // 如果需要切piece或者已经有足够单元化处理的数据, 进行处理
      if (OB_SUCC(ret) && ! piece_change && cached_buffer_full_(helper)) {
        if (OB_FAIL(handle_origin_buffer_(helper))) {
          ARCHIVE_LOG(WARN, "handle buffer failed", K(ret), K(piece_change), K(piece), K(helper));
        } else {
          ARCHIVE_LOG(TRACE, "handle buffer succ", K(ret), K(helper));
        }
      }
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }

  if (OB_SUCC(ret)) {
    const int64_t used_ts = common::ObTimeUtility::fast_current_time() - start_ts;
    statistic(helper.get_log_fetch_size(), used_ts);
  }
  return ret;
}

int ObArchiveFetcher::get_max_archive_point_(SCN &scn)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(log_service_->get_replayable_point(scn))) {
    ARCHIVE_LOG(WARN, "get replayable point failed", K(ret));
  }
  return ret;
}

int ObArchiveFetcher::fill_helper_piece_if_empty_(const ObArchivePiece &piece, TmpMemoryHelper &helper)
{
  int ret = OB_SUCCESS;
  if (helper.is_piece_set()) {
  } else {
    ret = helper.set_piece(piece);
  }
  return ret;
}

int ObArchiveFetcher::append_log_entry_(const char *buffer, LogGroupEntry &entry, TmpMemoryHelper &helper)
{
  return helper.append_log_entry(buffer, entry);
}

bool ObArchiveFetcher::cached_buffer_full_(TmpMemoryHelper &helper)
{
  return helper.original_buffer_enough(unit_size_);
}

bool ObArchiveFetcher::check_piece_inc_(const ObArchivePiece &p1, const ObArchivePiece &p2)
{
  return p1 > p2;
}

int ObArchiveFetcher::set_next_piece_(TmpMemoryHelper &helper)
{
  return helper.set_next_piece();
}

// 压缩加密buffer
int ObArchiveFetcher::handle_origin_buffer_(TmpMemoryHelper &helper)
{
  int ret = OB_SUCCESS;
  const char *origin_buf = NULL;
  int64_t origin_buf_size = 0;
  char *ec_buf = NULL;
  int64_t ec_buf_size = 0;
  if (OB_FAIL(helper.get_original_buf(origin_buf, origin_buf_size))) {
    ARCHIVE_LOG(WARN, "get original buf failed", K(ret), K(helper));
  } else if (OB_ISNULL(origin_buf) || OB_UNLIKELY(origin_buf_size < 0)) {
    ret = OB_ERR_UNEXPECTED;
    ARCHIVE_LOG(ERROR, "helper origin_buf unexpected error",
        K(ret), K(origin_buf), K(origin_buf_size), K(helper));
  } else if (0 == origin_buf_size) {
    // no data, just skip
    ARCHIVE_LOG(INFO, "no data exist, skip it", K(helper));
  } else if (OB_FAIL(do_compress_(helper))) {
    ARCHIVE_LOG(WARN, "do compress failed", K(ret), K(helper));
  } else if (OB_FAIL(do_encrypt_(helper))) {
    ARCHIVE_LOG(WARN, "do encrypt failed", K(ret), K(helper));
  } else {
    ec_buf = const_cast<char *>(origin_buf);
    ec_buf_size = origin_buf_size;
    if (OB_FAIL(helper.append_handled_buf(ec_buf, ec_buf_size))) {
      ARCHIVE_LOG(WARN, "append handled buf failed", K(ret), K(helper));
    } else {
      helper.freeze_log_entry();
      helper.reset_original_buffer();
    }
  }
  return ret;
}

int ObArchiveFetcher::do_compress_(TmpMemoryHelper &helper)
{
  UNUSED(helper);
  return OB_SUCCESS;
}

int ObArchiveFetcher::do_encrypt_(TmpMemoryHelper &helper)
{
  UNUSED(helper);
  return OB_SUCCESS;
}

// 由于同一个LogFetchTask可以包含多个piece数据, 需要归档到多个目录, 只有多个目录都归档完成
// 才能消费下一个LogFetchTask数据
bool ObArchiveFetcher::check_log_fetch_task_consume_complete_(ObArchiveLogFetchTask &task)
{
  return task.get_cur_offset() == task.get_end_offset();
}

int ObArchiveFetcher::update_log_fetch_task_(ObArchiveLogFetchTask &fetch_task,
    TmpMemoryHelper &helper,
    ObArchiveSendTask *send_task)
{
  int ret = OB_SUCCESS;
  const ObArchivePiece &task_piece = fetch_task.get_piece();
  const ObArchivePiece &cur_piece = helper.get_piece();
  const ObArchivePiece &next_piece = helper.get_next_piece();
  const LSN &start_offset = helper.get_start_offset();
  const LSN &cur_offset = helper.get_cur_offset();
  const LSN &end_offset = fetch_task.get_end_offset();
  const SCN &scn = helper.get_unitized_scn();

  if (NULL == send_task) {
    ARCHIVE_LOG(INFO, "send_task is NULL", K(send_task), K(helper), K(fetch_task));
  } else if (OB_UNLIKELY(task_piece.is_valid() && task_piece > cur_piece)) {
    ret = OB_ERR_UNEXPECTED;
    ARCHIVE_LOG(ERROR, "invalid pieces", K(ret), K(task_piece), K(cur_piece),
        K(fetch_task), K(helper));
  } else if (OB_FAIL(fetch_task.back_fill(cur_piece, start_offset, cur_offset,
                                          scn, send_task))) {
    ARCHIVE_LOG(WARN, "log fetch task backup fill failed", K(ret), K(helper),
        K(fetch_task), KPC(send_task));
  } else {
    ARCHIVE_LOG(INFO, "back fill log fetch task succ", K(fetch_task));
  }

  if (OB_SUCC(ret)) {
    if (! next_piece.is_valid()) {
      // next piece invalid, just skip
    } else if (cur_offset >= end_offset) {
      ret = OB_ERR_UNEXPECTED;
      ARCHIVE_LOG(ERROR, "invalid offset", K(ret), K(cur_offset), K(end_offset));
    } else if (OB_FAIL(fetch_task.set_next_piece(next_piece))) {
      ARCHIVE_LOG(WARN, "set next piece failed", K(ret), K(helper), K(fetch_task));
    } else {
      ARCHIVE_LOG(INFO, "set next piece succ", K(next_piece), K(fetch_task));
    }
  }
  return ret;
}

int ObArchiveFetcher::submit_fetch_log_(ObArchiveLogFetchTask &task, bool &submitted)
{
  int ret = OB_SUCCESS;
  const ObLSID &id = task.get_ls_id();
  submitted = false;

  if (! task.has_fetch_log()) {
    // just skip
  } else {
    GET_LS_TASK_CTX(ls_mgr_, id) {
      if (OB_FAIL(ls_archive_task->push_fetch_log(task))) {
        ARCHIVE_LOG(WARN, "push fetch log failed", K(ret), K(task));
      } else {
        submitted = true;
        ARCHIVE_LOG(INFO, "push fetch log succ", K(task));
      }
    }
  }
  return ret;
}

int ObArchiveFetcher::build_send_task_(const ObLSID &id,
    const ArchiveWorkStation &station,
    TmpMemoryHelper &helper,
    ObArchiveSendTask *&task)
{
  int ret = OB_SUCCESS;
  const ObArchivePiece &piece = helper.get_piece();
  const LSN &start_offset = helper.get_start_offset();
  const LSN &end_offset = helper.get_cur_offset();
  const SCN &max_scn = helper.get_unitized_scn();
  if (helper.is_empty()) {
    ARCHIVE_LOG(INFO, "helper is empty, just skip", K(helper));
  } else if (FALSE_IT(task = helper.gen_send_task())) {
  } else if (OB_FAIL(task->init(tenant_id_, id, station,
          piece, start_offset, end_offset, max_scn))) {
    ARCHIVE_LOG(WARN, "send task init failed", K(ret), K(id), K(station), K(helper));
  } else {
    ARCHIVE_LOG(INFO, "build send task succ", K(id), K(station));
  }

  if (OB_FAIL(ret) && NULL != task) {
    allocator_->free_send_task(task);
    task = NULL;
  }
  return ret;
}

int ObArchiveFetcher::try_consume_fetch_log_(const ObLSID &id)
{
  int ret = OB_SUCCESS;
  ObArchiveLogFetchTask *task = NULL;
  ObArchiveSendTask *send_task = NULL;
  bool task_exist = false;
  bool need_break = false;
  GET_LS_TASK_CTX(ls_mgr_, id) {
    // 为保证不把LS上fetch log饿死, 如果获取到消费权, 需要将任务消费完
    for (int64_t i = 0; OB_SUCC(ret) && !need_break && ! has_set_stop(); i++) {
      task = NULL;
      task_exist = false;
      if (OB_FAIL(get_sorted_fetch_log_(*ls_archive_task, task, task_exist))) {
        ARCHIVE_LOG(WARN, "get sorted fetch log failed", K(ret), K(id));
      } else if (! task_exist) {
        need_break = true;
        ARCHIVE_LOG(INFO, "no log fetch task exist, just skip", K(ret), K(id), K(task_exist));
      } else if (OB_ISNULL(send_task = task->get_send_task())) {
        ret = OB_ERR_UNEXPECTED;
        ARCHIVE_LOG(ERROR, "send task is NULL", K(ret), K(send_task), KPC(task));
      } else if (OB_FAIL(submit_send_task_(send_task))) {
        ARCHIVE_LOG(WARN, "submit send task failed", K(ret), KPC(send_task));
      } else if (OB_FAIL(task->clear_send_task())) {
        ARCHIVE_LOG(ERROR, "clear send task failed", K(ret), KPC(task));
      } else if (OB_FAIL(update_fetcher_progress_(*ls_archive_task, *task))) {
        ARCHIVE_LOG(WARN, "update fetcher progresss failed", K(ret), KPC(task), KPC(ls_archive_task));
      } else if (check_log_fetch_task_consume_complete_(*task)) {
        // free log fetch task
        inner_free_task_(task);
        task = NULL;
      } else if (OB_FAIL(submit_residual_log_fetch_task_(*task))) {
        ARCHIVE_LOG(WARN, "submit residual log fetch task failed", K(ret), KPC(task));
      } else {
        // task not complete if need re-submit to task queue
        // although get_sorted_fetch_log interface check this condition
        need_break = true;
        ARCHIVE_LOG(TRACE, "consume log fetch task succ", K(id));
      }

      if (OB_FAIL(ret) && NULL != task) {
        inner_free_task_(task);
        task = NULL;
      }
    } // for
  }
  return ret;
}

int ObArchiveFetcher::get_sorted_fetch_log_(ObLSArchiveTask &ls_archive_task,
    ObArchiveLogFetchTask *&task,
    bool &task_exist)
{
  int ret = OB_SUCCESS;
  ObArchiveLogFetchTask *tmp_task = NULL;
  task_exist = false;
  task = NULL;

  if (OB_FAIL(ls_archive_task.get_sorted_fetch_log(tmp_task))) {
    ARCHIVE_LOG(WARN, "get sorted fetch log failed", K(ret), K(ls_archive_task));
  } else if (NULL == tmp_task) {
    task_exist = false;
  } else if (OB_UNLIKELY(! tmp_task->is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    ARCHIVE_LOG(ERROR, "log fetch task is not valid", K(ret), KPC(tmp_task), K(ls_archive_task));
  } else {
    task_exist = true;
    task = tmp_task;
    ARCHIVE_LOG(INFO, "get sorted fetch log succ", KPC(task));
  }

  if (OB_FAIL(ret) && NULL != tmp_task) {
    inner_free_task_(tmp_task);
  }

  return ret;
}

bool ObArchiveFetcher::is_retry_ret_(const int ret_code) const
{
    return OB_ALLOCATE_MEMORY_FAILED == ret_code
      || OB_EAGAIN == ret_code;
}

bool ObArchiveFetcher::in_normal_status_(const ArchiveKey &key) const
{
  return round_mgr_->is_in_archive_status(key);
}

int ObArchiveFetcher::submit_residual_log_fetch_task_(ObArchiveLogFetchTask &task)
{
  int ret = OB_SUCCESS;
  //const ObArchivePiece &next_piece = task.get_next_piece();
  const ObArchivePiece &cur_piece = task.get_piece();
  const LSN &start_offset = task.get_start_offset();
  const LSN &cur_offset = task.get_cur_offset();

  if (OB_UNLIKELY(cur_piece.is_valid() && cur_offset == start_offset)) {
    ret = OB_INVALID_ARGUMENT;
    ARCHIVE_LOG(WARN, "invalid argument", K(ret), K(task));
  } else if (OB_FAIL(task_queue_.push(&task))) {
    ARCHIVE_LOG(WARN, "push task failed", K(ret), K(task));
  } else {
    ARCHIVE_LOG(INFO, "submit residual log fetch task succ", K(task));
  }
  return ret;
}

int ObArchiveFetcher::submit_send_task_(ObArchiveSendTask *send_task)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(archive_sender_->submit_send_task(send_task))) {
    ARCHIVE_LOG(WARN, "submit send task failed", K(ret), KPC(send_task));
  } else {
    ARCHIVE_LOG(INFO, "submit send task succ");
  }
  return ret;
}

int ObArchiveFetcher::update_fetcher_progress_(ObLSArchiveTask &ls_archive_task,
    ObArchiveLogFetchTask &task)
{
  int ret = OB_SUCCESS;
  const LSN &lsn = task.get_cur_offset();
  const SCN &scn = task.get_max_scn();
  const ObArchivePiece &piece = task.get_piece();
  LogFileTuple tuple(lsn, scn, piece);
  if (OB_FAIL(ls_archive_task.update_fetcher_progress(task.get_station(), tuple))) {
    ARCHIVE_LOG(WARN, "update fetcher progress failed", K(ret), K(task), K(ls_archive_task));
  }
  return ret;
}

void ObArchiveFetcher::inner_free_task_(ObArchiveLogFetchTask *task)
{
  allocator_->free_log_fetch_task(task);
}

void ObArchiveFetcher::handle_log_fetch_ret_(
    const ObLSID &id,
    const ArchiveKey &key,
    const int ret_code)
{
  int ret = OB_SUCCESS;
  ObArchiveInterruptReason reason;
  if (OB_LOG_ARCHIVE_LEADER_CHANGED == ret_code
      || OB_ALLOCATE_MEMORY_FAILED == ret_code
      || OB_ENTRY_NOT_EXIST == ret_code
      || OB_LS_NOT_EXIST == ret_code) {
    // handle ret with retry
  } else {
    if (OB_ERR_OUT_OF_LOWER_BOUND == ret_code) {
      reason.set(ObArchiveInterruptReason::Factor::LOG_RECYCLE, lbt(), ret_code);
    } else {
      reason.set(ObArchiveInterruptReason::Factor::UNKONWN, lbt(), ret_code);
    }
    ls_mgr_->mark_fatal_error(id, key, reason);
  }
}

void ObArchiveFetcher::statistic(const int64_t log_size, const int64_t ts)
{
  static __thread int64_t READ_LOG_SIZE = 0;
  static __thread int64_t READ_COST_TS = 0;
  static __thread int64_t READ_TASK_COUNT = 0;

  READ_LOG_SIZE += log_size;
  READ_COST_TS += ts;
  READ_TASK_COUNT += 1;
  if (TC_REACH_TIME_INTERVAL(10 * 1000 * 1000L)) {
    int64_t avg_task_lsn_size = READ_LOG_SIZE / std::max(READ_TASK_COUNT, 1L);
    int64_t avg_task_cost_ts = READ_COST_TS / std::max(READ_TASK_COUNT, 1L);
    ARCHIVE_LOG(INFO, "archive_fetcher statistic in 10s", "total_read_log_size", READ_LOG_SIZE,
        "total_read_cost_ts", READ_COST_TS, "total_read_task_count", READ_TASK_COUNT,
        K(avg_task_lsn_size), K(avg_task_cost_ts));
    READ_LOG_SIZE = 0;
    READ_COST_TS = 0;
    READ_TASK_COUNT = 0;
  }
}

// TmpMemoryHelper
ObArchiveFetcher::TmpMemoryHelper::TmpMemoryHelper(const int64_t unit_size,
    ObArchiveAllocator *allocator) :
  inited_(false),
  start_offset_(),
  end_offset_(),
  origin_buf_(NULL),
  origin_buf_pos_(0),
  cur_offset_(),
  cur_scn_(),
  ec_buf_(NULL),
  ec_buf_size_(0),
  ec_buf_pos_(0),
  unitized_offset_(),
  cur_piece_(),
  next_piece_(),
  unit_size_(unit_size),
  allocator_(allocator)
{

  unitized_scn_ = SCN::min_scn();
}

ObArchiveFetcher::TmpMemoryHelper::~TmpMemoryHelper()
{
  inited_ = false;
  start_offset_.reset();
  end_offset_.reset();
  cur_offset_.reset();
  cur_scn_.reset();
  unitized_offset_.reset();
  unitized_scn_.reset();
  cur_piece_.reset();
  next_piece_.reset();

  if (NULL != origin_buf_) {
    origin_buf_ = NULL;
    origin_buf_pos_ = 0;
  }

  if (NULL != ec_buf_) {
    inner_free_send_buffer_();
  }
  allocator_ = NULL;
}

int ObArchiveFetcher::TmpMemoryHelper::init(const uint64_t tenant_id,
    const ObLSID &id,
    const LSN &start_offset,
    const LSN &end_offset,
    const LSN &commit_offset,
    ObArchivePiece *piece)
{
  int ret = OB_SUCCESS;
  const int64_t total_size = (int64_t)(std::min(end_offset, commit_offset) - start_offset);
  if (OB_UNLIKELY(OB_INVALID_TENANT_ID == tenant_id
        || ! id.is_valid()
        || ! start_offset.is_valid()
        || ! end_offset.is_valid()
        || ! (start_offset < end_offset)
        || ! commit_offset.is_valid()
        || (NULL != piece && ! piece->is_valid()))) {
    ret = OB_INVALID_ARGUMENT;
    ARCHIVE_LOG(WARN, "invalid argument", K(ret), K(tenant_id), K(id),
        K(start_offset), K(end_offset), K(commit_offset), KPC(piece));
  } else if (OB_FAIL(get_send_buffer_(total_size))) {
    ARCHIVE_LOG(WARN, "get send buffer failed", K(ret), K(total_size));
  } else {
    tenant_id_ = tenant_id;
    id_ = id;
    origin_buf_pos_ = 0;
    start_offset_ = start_offset;
    cur_offset_ = start_offset;
    end_offset_ = end_offset;
    commit_offset_ = commit_offset;
    cur_piece_ = NULL != piece ? *piece : cur_piece_;
  }
  return ret;
}

ObArchiveSendTask *ObArchiveFetcher::TmpMemoryHelper::gen_send_task()
{
  void *data = NULL;
  ObArchiveSendTask *task = NULL;
  // send_task + file_header + commit_logs
  const int64_t reserved_size = sizeof(ObArchiveSendTask) + ARCHIVE_FILE_HEADER_SIZE;
  if (NULL != ec_buf_ && ec_buf_pos_ > 0) {
    data = static_cast<void *>(ec_buf_ - reserved_size);
    task = new (data) ObArchiveSendTask();
    task->set_buffer(ec_buf_, ec_buf_pos_);    // task buffer not include the file header
    ec_buf_ = NULL;
    ec_buf_size_ = 0;
    ec_buf_pos_ = 0;
  }
  return task;
}

// 聚合出足够大处理单元, 或者到达归档文件尾
bool ObArchiveFetcher::TmpMemoryHelper::original_buffer_enough(const int64_t size)
{
  return origin_buf_pos_ >= size || reach_end();
}

int ObArchiveFetcher::TmpMemoryHelper::get_original_buf(const char *&buf, int64_t &buf_size)
{
  buf = origin_buf_;
  buf_size = origin_buf_pos_;
  return OB_SUCCESS;
}

int ObArchiveFetcher::TmpMemoryHelper::set_piece(const ObArchivePiece &piece)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!piece.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    ARCHIVE_LOG(WARN, "invalid argument", K(ret), K(piece), KPC(this));
  } else {
    cur_piece_ = piece;
  }
  return ret;
}

int ObArchiveFetcher::TmpMemoryHelper::append_log_entry(const char *buffer, LogGroupEntry &entry)
{
  int ret = OB_SUCCESS;
  int64_t entry_size = entry.get_serialize_size();
  const SCN &scn = entry.get_scn();

  origin_buf_ = buffer;
  origin_buf_pos_ += entry_size;
  cur_offset_ = cur_offset_ + entry_size;
  cur_scn_ = scn;
  return ret;
}

int ObArchiveFetcher::TmpMemoryHelper::append_handled_buf(char *buf, const int64_t buf_size)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(buf) || OB_UNLIKELY(buf_size <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    ARCHIVE_LOG(WARN, "invalid argument", K(ret), K(buf), K(buf_size));
  } else if (OB_UNLIKELY(buf_size > ec_buf_size_ - ec_buf_pos_)) {
    ret = reserve_(std::max(static_cast<int64_t>(ec_buf_pos_ + (unitized_offset_ - start_offset_)), ec_buf_pos_ + buf_size));
  }
  if (OB_SUCC(ret)) {
    MEMCPY(ec_buf_ + ec_buf_pos_, buf, buf_size);
    ec_buf_pos_ += buf_size;
  }
  return ret;
}

void ObArchiveFetcher::TmpMemoryHelper::freeze_log_entry()
{
  unitized_offset_ = cur_offset_;
  unitized_scn_ = cur_scn_;
}

void ObArchiveFetcher::TmpMemoryHelper::reset_original_buffer()
{
  origin_buf_ = NULL;
  origin_buf_pos_ = 0;
}

int ObArchiveFetcher::TmpMemoryHelper::set_next_piece()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!cur_piece_.is_valid() || next_piece_.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    ARCHIVE_LOG(ERROR, "invalid pieces", K(ret), KPC(this));
  } else {
    next_piece_ = cur_piece_;
    next_piece_ = ++next_piece_;
  }
  return ret;
}

int ObArchiveFetcher::TmpMemoryHelper::get_handled_buf(char *&buf, int64_t &buf_size)
{
  int ret = OB_SUCCESS;
  buf = ec_buf_;
  buf_size = ec_buf_pos_;
  return ret;
}

int ObArchiveFetcher::TmpMemoryHelper::get_send_buffer_(const int64_t size)
{
  int ret = OB_SUCCESS;
  char *data = NULL;
  // send_task + file_header + commit_logs
  const int64_t reserved_size = get_reserved_buf_size_();
  int64_t real_size = size + reserved_size;
  // need compress(NOT SUPPORT) and size bigger than a block unit
  if (OB_ISNULL(data = allocator_->alloc_send_task(real_size))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    ARCHIVE_LOG(WARN, "get send buffer failed", K(ret));
  } else {
    ec_buf_ = data + reserved_size;
    ec_buf_size_ = real_size - reserved_size;
    ec_buf_pos_ = 0;
  }
  return ret;
}

void ObArchiveFetcher::TmpMemoryHelper::inner_free_send_buffer_()
{
  const int64_t reserved_size = sizeof(ObArchiveSendTask) + ARCHIVE_FILE_HEADER_SIZE;
  if (NULL != ec_buf_) {
    allocator_->free_send_task(static_cast<ObArchiveSendTask*>((void*)(ec_buf_ - reserved_size)));
    ec_buf_ = NULL;
    ec_buf_size_ = 0;
    ec_buf_pos_ = 0;
  }
}

int ObArchiveFetcher::TmpMemoryHelper::reserve_(const int64_t size)
{
  int ret = OB_SUCCESS;
  char *tmp_buf = NULL;
  const int64_t reserved_size = get_reserved_buf_size_();
  if (size <= ec_buf_size_) {
  } else if (OB_ISNULL(tmp_buf = (char *)allocator_->alloc_send_task(size + reserved_size))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    ARCHIVE_LOG(WARN, "alloc memory failed", K(ret), K(size));
  } else {
    MEMCPY(tmp_buf + reserved_size, ec_buf_, ec_buf_pos_);
    ec_buf_ = tmp_buf;
    ec_buf_size_ = size;
  }
  return ret;
}

int64_t ObArchiveFetcher::TmpMemoryHelper::get_reserved_buf_size_() const
{
  return sizeof(ObArchiveSendTask) + ARCHIVE_FILE_HEADER_SIZE;
}
} // namespace archive
} // namespace oceanbase
