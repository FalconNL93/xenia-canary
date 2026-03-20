/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_ACHIEVEMENT_BACKENDS_SAN_ACHIEVEMENT_BACKEND_H_
#define XENIA_KERNEL_XAM_ACHIEVEMENT_BACKENDS_SAN_ACHIEVEMENT_BACKEND_H_

#include <filesystem>
#include <span>

#include "xenia/kernel/xam/achievement_manager.h"

namespace xe {
namespace kernel {
namespace xam {

// Achievement backend for SteamAchievementNotifier (SAN).
// When enabled via --san_achievement_backend, appends one JSON line per
// unlocked achievement to:
//   <xenia_executable_dir>/san_events.jsonl
// and saves the achievement icon (from the title SPA) as a PNG to:
//   <xenia_executable_dir>/san_icons/<TitleId>_<achievementId>.png
//
// SAN polls san_events.jsonl for new lines and displays a notification for
// each one.  The format of each line is:
//   {"titleId":"4D5307E4","titleName":"...","achievementId":1,
//    "achievementName":"...","description":"...","gamerscore":25,
//    "hidden":false,"iconPath":"...","timestamp":"2025-01-01T00:00:00Z"}
class SanAchievementBackend : public AchievementBackendInterface {
 public:
  SanAchievementBackend();
  ~SanAchievementBackend() = default;

  void EarnAchievement(const uint64_t xuid, const uint32_t title_id,
                       const uint32_t achievement_id) override;

  bool IsAchievementUnlocked(const uint64_t xuid, const uint32_t title_id,
                             const uint32_t achievement_id) const override {
    return false;
  }

  const std::optional<Achievement> GetAchievementInfo(
      const uint64_t xuid, const uint32_t title_id,
      const uint32_t achievement_id) const override {
    return std::nullopt;
  }

  const std::vector<Achievement> GetTitleAchievements(
      const uint64_t xuid, const uint32_t title_id) const override {
    return {};
  }

  const std::span<const uint8_t> GetAchievementIcon(
      const uint64_t xuid, const uint32_t title_id,
      const uint32_t achievement_id) const override {
    return {};
  }

  bool LoadAchievementsData(const uint64_t xuid) override { return true; }

 private:
  bool SaveAchievementsData(const uint64_t xuid,
                            const uint32_t title_id) override {
    return false;
  }

  bool SaveAchievementData(const uint64_t xuid, const uint32_t title_id,
                           const Achievement* achievement) override {
    return false;
  }

  std::filesystem::path events_file_;
  std::filesystem::path icons_dir_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_ACHIEVEMENT_BACKENDS_SAN_ACHIEVEMENT_BACKEND_H_
