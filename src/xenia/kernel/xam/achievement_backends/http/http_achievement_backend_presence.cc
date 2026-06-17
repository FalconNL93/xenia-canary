/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/achievement_backends/http/http_achievement_backend.h"
#include "xenia/kernel/xam/achievement_backends/http/http_achievement_backend_internal.h"

#include <string>

#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/presence_string_builder.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/util/xlast.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/kernel/xnet.h"

DECLARE_string(http_achievement_backend_url);

namespace xe {
namespace kernel {
namespace xam {

using http_backend_internal::JsonEscape;
#if XE_PLATFORM_WIN32
using http_backend_internal::PostJsonAsync;
#endif  // XE_PLATFORM_WIN32

void HttpAchievementBackend::PostPresenceNow() const {
#if XE_PLATFORM_WIN32
  const std::string& base_url = cvars::http_achievement_backend_url;
  if (base_url.empty()) {
    return;
  }

  const auto* xam_state = kernel_state()->xam_state();
  if (!xam_state) {
    return;
  }

  auto* spa = xam_state->spa_info();
  if (!spa) {
    return;
  }

  const auto* pm = xam_state->profile_manager();
  if (!pm || !pm->IsAnyProfileSignedIn()) {
    return;
  }

  const auto* user_tracker = xam_state->user_tracker();
  if (!user_tracker) {
    return;
  }

  const UserProfile* profile = pm->GetProfile(static_cast<uint8_t>(0));
  if (!profile) {
    return;
  }

  const uint64_t xuid = profile->xuid();
  const uint32_t title_id = spa->title_id();

  const XLanguage lang = spa->GetExistingLanguage(
      static_cast<XLanguage>(
          kernel_state()->xconfig()->ReadSetting<uint32_t>(
              kernel::XCONFIG_USER_CATEGORY,
              kernel::XCONFIG_USER_LANGUAGE)));

  const std::string title_name = JsonEscape(spa->title_name(lang));

  std::string rich_presence;
  uint32_t compressed_size = 0, decompressed_size = 0;
  const uint8_t* xlast_ptr = spa->ReadXLast(compressed_size, decompressed_size);
  const xam::Property* presence_property =
      user_tracker->GetProperty(xuid, XCONTEXT_PRESENCE);
  if (xlast_ptr && presence_property) {
    xe::kernel::util::XLast xlast(xlast_ptr, compressed_size,
                                  decompressed_size);
    if (xlast.HasXLast()) {
      const auto raw_presence = xlast.GetPresenceRawString(presence_property);
      const auto formatter =
          xe::kernel::util::AttributeStringFormatter(raw_presence, &xlast,
                                                     xuid);
      if (formatter.IsComplete()) {
        rich_presence = xe::to_utf8(formatter.GetPresenceString());
      }
    }
  }

  const std::string body = fmt::format(
      R"({{"xuid":"{:016X}","title_id":"{:08X}","title_name":"{}","rich_presence":"{}"}})",
      xuid, title_id, title_name, JsonEscape(rich_presence));

  PostJsonAsync(base_url + "/presence", body);
#endif  // XE_PLATFORM_WIN32
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
