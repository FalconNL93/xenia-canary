/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/achievement_backends/http_achievement_backend.h"

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

namespace {

// Escapes a UTF-8 string for use as a JSON string value (without surrounding
// quotes). Handles the characters required by RFC 8259 §7.
std::string JsonEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (unsigned char c : input) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
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
          // Control character – encode as \uXXXX
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  return out;
}

// Encodes binary data as a base64 string (RFC 4648).
static std::string Base64Encode(std::span<const uint8_t> data) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out += kTable[(v >> 18) & 0x3F];
    out += kTable[(v >> 12) & 0x3F];
    out += kTable[(v >> 6) & 0x3F];
    out += kTable[v & 0x3F];
  }
  if (i + 1 == data.size()) {
    uint32_t v = data[i] << 16;
    out += kTable[(v >> 18) & 0x3F];
    out += kTable[(v >> 12) & 0x3F];
    out += "==";
  } else if (i + 2 == data.size()) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
    out += kTable[(v >> 18) & 0x3F];
    out += kTable[(v >> 12) & 0x3F];
    out += kTable[(v >> 6) & 0x3F];
    out += '=';
  }
  return out;
}

#if XE_PLATFORM_WIN32
// Performs a synchronous (blocking) HTTP POST with |body| to |url|.
// Must only be called from a background thread — never from the emulation or
// UI thread.
static void DoPostJson(const std::string& url, const std::string& body) {
  // Convert URL to wide string for WinHttp.
  std::wstring wurl(url.begin(), url.end());

  URL_COMPONENTS uc = {};
  uc.dwStructSize = sizeof(uc);
  wchar_t scheme[16] = {};
  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  uc.lpszScheme = scheme;
  uc.dwSchemeLength = static_cast<DWORD>(std::size(scheme));
  uc.lpszHostName = host;
  uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));

  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    XELOGE("HttpAchievementBackend: Failed to parse URL: {}", url);
    return;
  }

  const bool is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

  HINTERNET session = WinHttpOpen(
      L"Xenia/1.0 (Achievement Backend)",
      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    XELOGE("HttpAchievementBackend: WinHttpOpen failed ({})", GetLastError());
    return;
  }

  HINTERNET connection = WinHttpConnect(session, host, uc.nPort, 0);
  if (!connection) {
    XELOGE("HttpAchievementBackend: WinHttpConnect failed ({})",
           GetLastError());
    WinHttpCloseHandle(session);
    return;
  }

  DWORD request_flags = is_https ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request =
      WinHttpOpenRequest(connection, L"POST",
                         (uc.dwUrlPathLength ? path : L"/"), nullptr,
                         WINHTTP_NO_REFERER,
                         WINHTTP_DEFAULT_ACCEPT_TYPES, request_flags);
  if (!request) {
    XELOGE("HttpAchievementBackend: WinHttpOpenRequest failed ({})",
           GetLastError());
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return;
  }

  // Set a reasonable send/receive timeout (10 seconds each).
  DWORD timeout_ms = 10000;
  WinHttpSetOption(request, WINHTTP_OPTION_SEND_TIMEOUT, &timeout_ms,
                   sizeof(timeout_ms));
  WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout_ms,
                   sizeof(timeout_ms));

  // Allow self-signed / untrusted dev certificates (common for localhost dev
  // servers such as the .NET Kestrel dev cert).
  if (is_https) {
    DWORD security_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                           SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                           SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                           SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &security_flags,
                     sizeof(security_flags));
  }

  const std::wstring headers = L"Content-Type: application/json\r\n";

  BOOL ok = WinHttpSendRequest(
      request, headers.c_str(),
      static_cast<DWORD>(headers.size()),
      const_cast<char*>(body.c_str()),
      static_cast<DWORD>(body.size()),
      static_cast<DWORD>(body.size()), 0);

  if (ok) {
    BOOL recv_ok = WinHttpReceiveResponse(request, nullptr);
    if (!recv_ok) {
      DWORD err = GetLastError();
      XELOGE("HttpAchievementBackend: WinHttpReceiveResponse failed ({})", err);
      fmt::print("[HttpAchievementBackend] ERROR: WinHttpReceiveResponse failed ({}) - URL: {}\n", err, url);
    } else {
      // Read and log the HTTP status code.
      DWORD status_code = 0;
      DWORD status_size = sizeof(status_code);
      WinHttpQueryHeaders(request,
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status_code,
                          &status_size, WINHTTP_NO_HEADER_INDEX);
      fmt::print("[HttpAchievementBackend] {} -> HTTP {}\n", url, status_code);
      if (status_code >= 400) {
        // Read response body for error detail.
        std::string response_body;
        DWORD bytes_available = 0;
        while (WinHttpQueryDataAvailable(request, &bytes_available) &&
               bytes_available > 0) {
          std::vector<char> buf(bytes_available + 1, '\0');
          DWORD bytes_read = 0;
          WinHttpReadData(request, buf.data(), bytes_available, &bytes_read);
          response_body.append(buf.data(), bytes_read);
        }
        fmt::print("[HttpAchievementBackend] Error body: {}\n", response_body);
      }
    }
  } else {
    DWORD err = GetLastError();
    XELOGE("HttpAchievementBackend: WinHttpSendRequest failed ({})", err);
    fmt::print("[HttpAchievementBackend] ERROR: WinHttpSendRequest failed ({}) - URL: {}\n",
               err, url);
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
}

// Sends a fire-and-forget HTTP POST — spawns a detached thread.
void PostJsonAsync(const std::string& url, const std::string& body) {
  std::thread([url, body]() { DoPostJson(url, body); }).detach();
}

// Sends a blocking HTTP POST — must only be called from a background thread.
void PostJsonBlocking(const std::string& url, const std::string& body) {
  DoPostJson(url, body);
}
#endif  // XE_PLATFORM_WIN32

}  // namespace

HttpAchievementBackend::HttpAchievementBackend() {
#if XE_PLATFORM_WIN32
  if (!cvars::http_achievement_backend_url.empty()) {
    presence_timer_thread_ =
        std::thread([this]() { RunPresenceTimer(); });
  }
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
