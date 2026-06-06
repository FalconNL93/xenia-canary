/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_ACHIEVEMENT_BACKENDS_HTTP_ACHIEVEMENT_BACKEND_H_
#define XENIA_KERNEL_XAM_ACHIEVEMENT_BACKENDS_HTTP_ACHIEVEMENT_BACKEND_H_

#include <optional>
#include <span>
#include <vector>

#include "xenia/kernel/xam/achievement_manager.h"

namespace xe {
namespace kernel {
namespace xam {

// A 3PP achievement backend that POSTs a JSON payload to a configurable HTTP
// endpoint whenever an achievement is earned. Controlled via the
// --http_achievement_backend_url cvar. Requests are fire-and-forget and do
// not block emulation.
class HttpAchievementBackend : public AchievementBackendInterface {
 public:
  HttpAchievementBackend() = default;
  ~HttpAchievementBackend() = default;

  void EarnAchievement(const uint64_t xuid, const uint32_t title_id,
                       const uint32_t achievement_id) override;

  // Posts all achievement states for every played title of the given user
  // to <base_url>/sync. Fire-and-forget, runs on a detached thread.
  void SyncAchievements(const uint64_t xuid) const;

  // These are unused for a notification-only backend.
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
  bool LoadAchievementsData(const uint64_t xuid) override { return false; }

 private:
  bool SaveAchievementsData(const uint64_t xuid,
                            const uint32_t title_id) override {
    return false;
  }
  bool SaveAchievementData(const uint64_t xuid, const uint32_t title_id,
                           const Achievement* achievement) override {
    return false;
  }
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_ACHIEVEMENT_BACKENDS_HTTP_ACHIEVEMENT_BACKEND_H_
