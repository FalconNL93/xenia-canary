/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/achievement_backends/san_achievement_backend.h"

#include <chrono>
#include <ctime>
#include <fstream>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xdbf/gpd_info.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"

DECLARE_int32(user_language);

namespace xe {
namespace kernel {
namespace xam {

SanAchievementBackend::SanAchievementBackend() {
  const auto exe_dir = xe::filesystem::GetExecutableFolder();
  events_file_ = exe_dir / "san_events.jsonl";
  icons_dir_ = exe_dir / "san_icons";
  std::filesystem::create_directories(icons_dir_);
}

static std::string EscapeJsonString(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += fmt::format("\\u{:04x}", static_cast<uint32_t>(c));
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

void SanAchievementBackend::EarnAchievement(const uint64_t xuid,
                                            const uint32_t title_id,
                                            const uint32_t achievement_id) {
  SpaInfo* spa = kernel_state()->xam_state()->spa_info();
  if (!spa) {
    XELOGW("SAN: No SPA data available for title {:08X}", title_id);
    return;
  }

  const AchievementTableEntry* entry = spa->GetAchievement(achievement_id);
  if (!entry) {
    XELOGW("SAN: Achievement {} not found in SPA for title {:08X}",
           achievement_id, title_id);
    return;
  }

  const XLanguage lang = static_cast<XLanguage>(cvars::user_language);
  const std::string ach_name =
      spa->GetStringTableEntry(lang, entry->label_id);
  const std::string ach_desc =
      spa->GetStringTableEntry(lang, entry->description_id);
  const std::string title_name = kernel_state()->emulator()->title_name();

  // kShowUnachieved means the achievement is visible before it is earned.
  // Absence of this flag implies it is hidden until earned.
  const bool is_hidden =
      (static_cast<uint32_t>(entry->flags) &
       static_cast<uint32_t>(AchievementFlags::kShowUnachieved)) == 0;

  // Write the achievement icon PNG alongside the events file.
  std::string icon_path_str;
  const auto icon_data = spa->GetIcon(entry->image_id);
  if (!icon_data.empty()) {
    const auto icon_file =
        icons_dir_ / fmt::format("{:08X}_{}.png", title_id, achievement_id);
    std::ofstream icon_out(icon_file, std::ios::binary | std::ios::trunc);
    if (icon_out) {
      icon_out.write(reinterpret_cast<const char*>(icon_data.data()),
                     static_cast<std::streamsize>(icon_data.size()));
      icon_path_str = xe::path_to_utf8(icon_file);
    }
  }

  // ISO-8601 UTC timestamp.
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_t = std::chrono::system_clock::to_time_t(now);
  std::tm utc_tm{};
#if XE_PLATFORM_WIN32 == 1
  gmtime_s(&utc_tm, &now_t);
#else
  gmtime_r(&now_t, &utc_tm);
#endif
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);

  // Forward-slash separators for cross-platform compatibility inside JSON.
  std::string icon_path_json = icon_path_str;
  std::replace(icon_path_json.begin(), icon_path_json.end(), '\\', '/');

  const std::string line = fmt::format(
      "{{\"titleId\":\"{:08X}\",\"titleName\":\"{}\","
      "\"achievementId\":{},\"achievementName\":\"{}\","
      "\"description\":\"{}\",\"gamerscore\":{},\"hidden\":{},"
      "\"iconPath\":\"{}\",\"timestamp\":\"{}\"}}\n",
      title_id, EscapeJsonString(title_name), static_cast<uint32_t>(achievement_id),
      EscapeJsonString(ach_name), EscapeJsonString(ach_desc),
      static_cast<uint32_t>(entry->gamerscore),
      is_hidden ? "true" : "false",
      EscapeJsonString(icon_path_json), ts);

  std::ofstream out(events_file_, std::ios::app);
  if (out) {
    out << line;
    XELOGI("SAN: Wrote achievement event \"{}\" to {}", ach_name,
           xe::path_to_utf8(events_file_));
  } else {
    XELOGW("SAN: Failed to write achievement event to {}",
           xe::path_to_utf8(events_file_));
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
