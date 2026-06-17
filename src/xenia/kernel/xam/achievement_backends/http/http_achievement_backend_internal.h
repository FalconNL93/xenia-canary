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

#include <cstdint>
#include <span>
#include <string>

namespace xe {
namespace kernel {
namespace xam {
namespace http_backend_internal {

std::string JsonEscape(const std::string& input);
std::string Base64Encode(std::span<const uint8_t> data);

#if XE_PLATFORM_WIN32
void PostJsonAsync(const std::string& url, const std::string& body);
void PostJsonBlocking(const std::string& url, const std::string& body);
#endif  // XE_PLATFORM_WIN32

}  // namespace http_backend_internal
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_ACHIEVEMENT_BACKENDS_HTTP_HTTP_ACHIEVEMENT_BACKEND_INTERNAL_H_
