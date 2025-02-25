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

#define USING_LOG_PREFIX RS

#include "rootserver/freeze/ob_major_freeze_util.h"

#include "lib/time/ob_time_utility.h"
#include "share/ob_define.h"
#include "rootserver/freeze/ob_major_freeze_service.h"

namespace oceanbase
{
namespace rootserver
{

int ObMajorFreezeUtil::get_major_freeze_service(
    ObPrimaryMajorFreezeService *primary_major_freeze_service,
    ObRestoreMajorFreezeService *restore_major_freeze_service,
    ObMajorFreezeService *&major_freeze_service,
    bool &is_primary_service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(primary_major_freeze_service) || OB_ISNULL(restore_major_freeze_service)) {
    ret = OB_INVALID_ARGUMENT;
    RS_LOG(ERROR, "primary or restore major_freeze_service is nullptr", KR(ret),
           KP(primary_major_freeze_service), KP(restore_major_freeze_service));
  } else {
    bool is_primary_service_paused = primary_major_freeze_service->is_paused();
    bool is_restore_service_paused = restore_major_freeze_service->is_paused();
    if (0 == (is_primary_service_paused ^ is_restore_service_paused)) {
      ret = OB_ERR_UNEXPECTED;
      RS_LOG(ERROR, "both primary and restore major_freeze_service are paused or not paused",
             KR(ret), K(is_primary_service_paused), K(is_restore_service_paused));
    } else if (!is_primary_service_paused) {
      major_freeze_service = primary_major_freeze_service;
      is_primary_service = true;
    } else if (!is_restore_service_paused) {
      major_freeze_service = restore_major_freeze_service;
      is_primary_service = false;
    }
  }
  return ret;
}

ObFreezeTimeGuard::ObFreezeTimeGuard(
  const char *file,
  const int64_t line,
  const char *func,
  const char *mod)
  : warn_threshold_us_(FREEZE_WARN_THRESHOLD_US),
    start_time_us_(common::ObTimeUtility::fast_current_time()),
    file_(file),
    line_(line),
    func_name_(func),
    log_mod_(mod)
{
}

ObFreezeTimeGuard::~ObFreezeTimeGuard()
{
  int64_t now_us = common::ObTimeUtility::fast_current_time();
  int64_t total_cost_us = now_us - start_time_us_;
  if (OB_UNLIKELY(total_cost_us >= warn_threshold_us_)) {
    constexpr int buffer_size = 256;
    char strbuffer[buffer_size] = { 0 };
    int n = snprintf(strbuffer, buffer_size, "cost too much time: %s (%s:%ld), ",
                     func_name_, strrchr(file_, '/') ? (strrchr(file_, '/') + 1) : file_, line_);
    if (n >= buffer_size) {
      snprintf(&strbuffer[buffer_size - 6], 6, "..., ");
    }
    common::OB_PRINT(log_mod_, OB_LOG_LEVEL_DIRECT(WARN), strbuffer,
      LOG_KVS(K_(warn_threshold_us), K(total_cost_us), K_(start_time_us), K(now_us)));
  }
}

} // end namespace rootserver
} // end namespace oceanbase
