/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/achievement_backends/http/http_achievement_backend_internal.h"

namespace xe {
namespace kernel {
namespace xam {

using namespace http_achievement_backend_internal;

void HttpAchievementBackend::PostPresenceNow() const {
#if XE_PLATFORM_WIN32
  const std::string& base_url = cvars::http_achievement_backend_url;
  if (base_url.empty()) {
    return;
  }

  auto* spa = kernel_state()->xam_state()->spa_info();
  if (!spa) {
    return;
  }

  const auto* pm = kernel_state()->xam_state()->profile_manager();
  if (!pm || !pm->IsAnyProfileSignedIn()) {
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

  // Build rich presence string (same logic as SyncAchievements).
  std::string rich_presence;
  const auto* user_tracker = kernel_state()->xam_state()->user_tracker();
  uint32_t compressed_size = 0, decompressed_size = 0;
  const uint8_t* xlast_ptr = spa->ReadXLast(compressed_size, decompressed_size);
  if (xlast_ptr) {
    xe::kernel::util::XLast xlast(xlast_ptr, compressed_size,
                                  decompressed_size);
    if (xlast.HasXLast()) {
      std::map<uint32_t, uint32_t> contexts;
      for (const auto& key : user_tracker->GetUserContextIds(xuid)) {
        auto val = user_tracker->GetUserContext(xuid, key.value);
        if (val.has_value()) {
          contexts[key.value] = val.value();
        }
      }

      const auto* presence_entry = spa->GetPresence();
      if (presence_entry) {
        for (const uint32_t ctx_id :
             presence_entry->property_bag.contexts) {
          auto it = contexts.find(ctx_id);
          if (it == contexts.end()) {
            continue;
          }
          const uint32_t mode_val = it->second;
          for (const auto& property_id : presence_entry->property_bag.properties) {
              const auto* presence_property =
                  kernel_state()->xam_state()->user_tracker()->GetProperty(xuid, property_id);
              if (!presence_property) {
                continue;
              }

              const auto raw_presence =
                  xlast.GetPresenceRawString(presence_property);
              if (raw_presence.empty()) {
                continue;
              }

              const auto formatter =
                  xe::kernel::util::AttributeStringFormatter(
                      raw_presence, &xlast, xuid);

              if (!formatter.IsComplete()) {
                continue;
              }

              rich_presence = xe::to_utf8(formatter.GetPresenceString());
              break;
            }

            if (!rich_presence.empty()) {
              break;
            }
        }
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
