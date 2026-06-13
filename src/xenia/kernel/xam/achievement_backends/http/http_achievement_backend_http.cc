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
namespace http_achievement_backend_internal {

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
std::string Base64Encode(std::span<const uint8_t> data) {
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

}  // namespace http_achievement_backend_internal
}  // namespace xam
}  // namespace kernel
}  // namespace xe
