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

#include <map>
#include <string>
#include <variant>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/presence_string_builder.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/util/xlast.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"
#include "xenia/kernel/xconfig.h"

DECLARE_string(http_achievement_backend_url);

namespace xe {
namespace kernel {
namespace xam {

using http_backend_internal::Base64Encode;
using http_backend_internal::JsonEscape;
#if XE_PLATFORM_WIN32
using http_backend_internal::PostJsonAsync;
using http_backend_internal::PostJsonBlocking;
#endif  // XE_PLATFORM_WIN32

void HttpAchievementBackend::EarnAchievement(const uint64_t xuid,
                                             const uint32_t title_id,
                                             const uint32_t achievement_id) {
#if XE_PLATFORM_WIN32
  const std::string& url = cvars::http_achievement_backend_url;
  if (url.empty()) {
    return;
  }

  // Gather achievement metadata from the SPA (static game data).
  std::string name;
  std::string description;
  uint32_t gamerscore = 0;
  uint32_t flags = 0;

  auto* spa = kernel_state()->xam_state()->spa_info();
  if (spa) {
    const auto* entry = spa->GetAchievement(achievement_id);
    if (entry) {
      const XLanguage lang = spa->GetExistingLanguage(
          static_cast<XLanguage>(
              kernel_state()->xconfig()->ReadSetting<uint32_t>(
                  kernel::XCONFIG_USER_CATEGORY,
                  kernel::XCONFIG_USER_LANGUAGE)));
      name = spa->GetStringTableEntry(lang, entry->label_id);
      description = spa->GetStringTableEntry(lang, entry->description_id);
      gamerscore = entry->gamerscore;
      flags = entry->flags;
    }
  }

  // Build the JSON payload.
  const std::string body =
      fmt::format(R"({{"xuid":"{:016X}","title_id":"{:08X}","achievement_id":{},"achievement_name":"{}","description":"{}","gamerscore":{},"flags":{}}})",
                  xuid, title_id, achievement_id,
                  JsonEscape(name), JsonEscape(description),
                  gamerscore, flags);

  XELOGI("HttpAchievementBackend: Posting achievement {} to {}", achievement_id,
         url);
  fmt::print("[HttpAchievementBackend] Posting achievement {} ('{}') for title {:08X} to {}\n",
             achievement_id, name, title_id, url);
  PostJsonAsync(url + "/achievements", body);
#endif  // XE_PLATFORM_WIN32
}

void HttpAchievementBackend::SyncAchievements(const uint64_t xuid) const {
#if XE_PLATFORM_WIN32
  const std::string& base_url = cvars::http_achievement_backend_url;
  if (base_url.empty()) {
    return;
  }

  // ── 1. Profile ────────────────────────────────────────────────────────
  // Sync the profile first (blocking) so the profile row exists in the DB
  // before any game/achievement rows arrive.
  {
    std::string gamertag;
    std::string gamerpic_b64;
    std::string motto;
    std::string location;
    int32_t gamerscore = 0;
    int32_t titles_played = 0;
    int32_t achievements_earned = 0;

    const auto* pm = kernel_state()->xam_state()->profile_manager();
    UserProfile* profile = pm ? pm->GetProfile(xuid) : nullptr;
    if (profile) {
      gamertag = JsonEscape(profile->name());

      const auto gamerpic_span =
          profile->GetProfileIcon(XTileType::kGamerTile);
      if (!gamerpic_span.empty()) {
        gamerpic_b64 = Base64Encode(gamerpic_span);
      }

      // Read dashboard GPD settings via user_tracker.
      const auto* ut = kernel_state()->xam_state()->user_tracker();

      auto read_int32 = [&](UserSettingId id) -> int32_t {
        auto s = ut->GetSetting(
            profile, kDashboardID, static_cast<uint32_t>(id));
        if (!s) return 0;
        auto v = s->get_host_data();
        if (auto* p = std::get_if<int32_t>(&v)) return *p;
        return 0;
      };
      auto read_wstring = [&](UserSettingId id) -> std::string {
        auto s = ut->GetSetting(
            profile, kDashboardID, static_cast<uint32_t>(id));
        if (!s) return "";
        auto v = s->get_host_data();
        if (auto* p = std::get_if<std::u16string>(&v))
          return JsonEscape(xe::to_utf8(*p));
        return "";
      };

      gamerscore = read_int32(UserSettingId::XPROFILE_GAMERCARD_CRED);
      titles_played =
          read_int32(UserSettingId::XPROFILE_GAMERCARD_TITLES_PLAYED);
      achievements_earned =
          read_int32(UserSettingId::XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED);
      motto = read_wstring(UserSettingId::XPROFILE_GAMERCARD_MOTTO);
      location = read_wstring(UserSettingId::XPROFILE_GAMERCARD_USER_LOCATION);
    }

    const std::string profile_body = fmt::format(
        R"({{"xuid":"{:016X}","gamertag":"{}","gamerpic":"{}","motto":"{}","location":"{}","gamerscore":{},"titles_played":{},"achievements_earned":{}}})",
        xuid, gamertag, gamerpic_b64, motto, location, gamerscore,
        titles_played, achievements_earned);

    XELOGI("HttpAchievementBackend: Syncing profile {:016X} ({})", xuid,
           gamertag);
    fmt::print("[HttpAchievementBackend] Syncing profile {:016X} ('{}') -> {}/profile\n",
               xuid, gamertag, base_url);
    PostJsonBlocking(base_url + "/profile", profile_body);
  }

  // ── 2. Games + Achievements ───────────────────────────────────────────
  // Gather all played titles for this user.
  const std::string sync_url = base_url + "/achievements/sync";
  const auto* user_tracker = kernel_state()->xam_state()->user_tracker();
  const std::vector<TitleInfo> played_titles =
      user_tracker->GetPlayedTitles(xuid);

  if (played_titles.empty()) {
    XELOGI("HttpAchievementBackend: No played titles found for xuid {:016X}",
           xuid);
    fmt::print("[HttpAchievementBackend] No played titles found for xuid {:016X}\n", xuid);
    return;
  }

  // For each title, gather achievements and POST one payload (fire-and-forget).
  for (const auto& title : played_titles) {
    const std::vector<Achievement> achievements =
        kernel_state()->achievement_manager()->GetTitleAchievements(xuid,
                                                                    title.id);

    // Build JSON achievement array.
    std::string ach_array = "[";
    bool first = true;
    for (const auto& ach : achievements) {
      if (!first) {
        ach_array += ",";
      }
      first = false;

      const uint64_t unlock_filetime =
          (static_cast<uint64_t>(ach.unlock_time.high_part) << 32) |
          static_cast<uint32_t>(ach.unlock_time.low_part);

      // Fetch achievement icon from game GPD.
      const auto icon_span =
          user_tracker->GetAchievementIcon(xuid, title.id, ach.achievement_id);
      const std::string icon_b64 =
          icon_span.empty() ? "" : Base64Encode(icon_span);

      ach_array += fmt::format(
          R"({{"id":{},"name":"{}","unlocked_description":"{}","locked_description":"{}","gamerscore":{},"flags":{},"is_unlocked":{},"unlock_time":{},"image":"{}"}})",
          ach.achievement_id,
          JsonEscape(xe::to_utf8(ach.achievement_name)),
          JsonEscape(xe::to_utf8(ach.unlocked_description)),
          JsonEscape(xe::to_utf8(ach.locked_description)), ach.gamerscore,
          ach.flags, ach.IsUnlocked() ? "true" : "false", unlock_filetime,
          icon_b64);
    }
    ach_array += "]";

    // Game thumbnail from TitleInfo (loaded from game GPD).
    const std::string thumbnail_b64 =
        title.icon.empty() ? "" : Base64Encode(title.icon);

    // Rich presence — only available for the currently loaded game.
    std::string rich_presence;
    auto* spa = kernel_state()->xam_state()->spa_info();
    if (spa && spa->title_id() == title.id) {
      // Build XLast from the current SPA.
      uint32_t compressed_size = 0, decompressed_size = 0;
      const uint8_t* xlast_ptr =
          spa->ReadXLast(compressed_size, decompressed_size);
      if (xlast_ptr) {
        xe::kernel::util::XLast xlast(xlast_ptr, compressed_size,
                                      decompressed_size);
        if (xlast.HasXLast()) {
          // Collect all current context values for this user.
          std::map<uint32_t, uint32_t> contexts;
          for (const auto& key : user_tracker->GetUserContextIds(xuid)) {
            auto val = user_tracker->GetUserContext(xuid, key.value);
            if (val.has_value()) {
              contexts[key.value] = val.value();
            }
          }

          // Find presence mode by trying each presence context value.
          const auto* presence_entry = spa->GetPresence();
          if (presence_entry) {
            for (const uint32_t ctx_id :
                 presence_entry->property_bag.contexts) {
              auto it = contexts.find(ctx_id);
              if (it == contexts.end()) {
                continue;
              }
              const uint32_t mode_val = it->second;
              const auto raw = xlast.GetPresenceRawString(
                  mode_val, XLanguage::kEnglish);
              if (!raw.empty()) {
                xe::kernel::util::AttributeStringFormatter formatter(
                    xe::to_utf8(raw), &xlast, contexts);
                rich_presence = formatter.GetPresenceString();
                break;
              }
            }
          }
        }
      }
    }

    const std::string title_name = xe::to_utf8(title.title_name);
    const std::string body = fmt::format(
        R"({{"xuid":"{:016X}","title_id":"{:08X}","title_name":"{}","thumbnail":"{}","rich_presence":"{}","achievements":{}}})",
        xuid, title.id, JsonEscape(title_name), thumbnail_b64,
        JsonEscape(rich_presence), ach_array);

    XELOGI(
        "HttpAchievementBackend: Syncing title {:08X} ({} achievements) for "
        "xuid {:016X}",
        title.id, achievements.size(), xuid);
    fmt::print("[HttpAchievementBackend] Syncing title {:08X} '{}' ({} achievements) for xuid {:016X} -> {}\n",
               title.id, title_name, achievements.size(), xuid, sync_url);
    PostJsonAsync(sync_url, body);
  }
#endif  // XE_PLATFORM_WIN32
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
