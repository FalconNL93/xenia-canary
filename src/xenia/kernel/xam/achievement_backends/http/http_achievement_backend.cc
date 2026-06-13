/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/achievement_backends/http/http_achievement_backend.h"

#include <chrono>
#include <mutex>
#include <thread>

#include "xenia/base/platform.h"

DECLARE_string(http_achievement_backend_url);

namespace xe {
namespace kernel {
namespace xam {

HttpAchievementBackend::HttpAchievementBackend() {
#if XE_PLATFORM_WIN32
  // if (!cvars::http_achievement_backend_url.empty()) {
  //   presence_timer_thread_ =
  //       std::thread([this]() { RunPresenceTimer(); });
  // }
#endif  // XE_PLATFORM_WIN32
}

HttpAchievementBackend::~HttpAchievementBackend() {
#if XE_PLATFORM_WIN32
  {
    std::lock_guard<std::mutex> lock(presence_timer_mutex_);
    stop_presence_timer_ = true;
  }
  presence_timer_cv_.notify_all();
  if (presence_timer_thread_.joinable()) {
    presence_timer_thread_.join();
  }
#endif  // XE_PLATFORM_WIN32
}

void HttpAchievementBackend::RunPresenceTimer() {
#if XE_PLATFORM_WIN32
  while (!stop_presence_timer_) {
    PostPresenceNow();

    std::unique_lock<std::mutex> lock(presence_timer_mutex_);
    presence_timer_cv_.wait_for(
        lock, std::chrono::seconds(60),
        [this] { return stop_presence_timer_.load(); });
  }
#endif  // XE_PLATFORM_WIN32
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
