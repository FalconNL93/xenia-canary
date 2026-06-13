/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_ACHIEVEMENT_BACKENDS_HTTP_HTTP_ACHIEVEMENT_BACKEND_INTERNAL_H_
#define XENIA_KERNEL_XAM_ACHIEVEMENT_BACKENDS_HTTP_HTTP_ACHIEVEMENT_BACKEND_INTERNAL_H_

#include "xenia/kernel/xam/achievement_backends/http/http_achievement_backend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/presence_string_builder.h"
#include "xenia/kernel/util/xlast.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"

#if XE_PLATFORM_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif  // XE_PLATFORM_WIN32

DECLARE_string(http_achievement_backend_url);

namespace xe {
namespace kernel {
namespace xam {
namespace http_achievement_backend_internal {

std::string JsonEscape(const std::string& input);
std::string Base64Encode(std::span<const uint8_t> data);

#if XE_PLATFORM_WIN32
void PostJsonAsync(const std::string& url, const std::string& body);
void PostJsonBlocking(const std::string& url, const std::string& body);
#endif  // XE_PLATFORM_WIN32

}  // namespace http_achievement_backend_internal
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_ACHIEVEMENT_BACKENDS_HTTP_HTTP_ACHIEVEMENT_BACKEND_INTERNAL_H_
