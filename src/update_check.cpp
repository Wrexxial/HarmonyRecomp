// harmonyrecomp - "a new version is available" check.

#include "update_check.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "notice.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#ifndef HARMONYRECOMP_VERSION
#define HARMONYRECOMP_VERSION "dev"
#endif

REXCVAR_DEFINE_BOOL(check_for_updates, true, "HarmonyRecomp",
                    "Look for a newer HarmonyRecomp release on GitHub at startup");

namespace harmony {
namespace {

constexpr const char* kRepo = "Wrexxial/HarmonyRecomp";

std::mutex g_mutex;
std::string g_available;

// "v0.5.10" -> {0, 5, 10}; empty if it is not a release tag.
std::vector<int> ParseVersion(const std::string& tag) {
  static const std::regex re(R"(^v?(\d+)\.(\d+)(?:\.(\d+))?$)");
  std::smatch m;
  if (!std::regex_match(tag, m, re)) return {};
  return {std::atoi(m[1].str().c_str()), std::atoi(m[2].str().c_str()),
          m[3].matched ? std::atoi(m[3].str().c_str()) : 0};
}

#ifdef _WIN32
// GET https://api.github.com/repos/<repo>/releases/latest; empty on failure.
std::string FetchLatestRelease() {
  std::string body;
  HINTERNET session = WinHttpOpen(L"HarmonyRecomp-update-check", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return body;
  WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
  HINTERNET connect = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
  std::wstring path = L"/repos/";
  for (const char* c = kRepo; *c; ++c) path += static_cast<wchar_t>(*c);
  path += L"/releases/latest";
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   WINHTTP_FLAG_SECURE)
                              : nullptr;
  if (request &&
      WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1),
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status == 200) {
      DWORD available = 0;
      while (WinHttpQueryDataAvailable(request, &available) && available > 0 &&
             body.size() < 512 * 1024) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
        body.append(chunk.data(), read);
      }
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return body;
}
#else
std::string FetchLatestRelease() { return {}; }
#endif

void Check() {
  // Let the game show its title first, so the notice is seen.
  std::this_thread::sleep_for(std::chrono::seconds(8));
  const std::string current = BuildVersion();
  const std::string body = FetchLatestRelease();
  std::smatch m;
  static const std::regex tag_re(R"re("tag_name"\s*:\s*"([^"]+)")re");
  if (!std::regex_search(body, m, tag_re)) {
    REXLOG_INFO("Update check: no answer from GitHub");
    return;
  }
  const std::string latest = m[1].str();
  // The release is published before the build attaches its files; until the
  // setup is there, a player could not update anyway.
  if (body.find("\"HarmonyRecomp-Setup.exe\"") == std::string::npos) {
    REXLOG_INFO("Update check: {} has no setup yet (still building)", latest);
    return;
  }
  const auto have = ParseVersion(current);
  const auto want = ParseVersion(latest);
  if (want.empty() || have.empty() || !(have < want)) {
    REXLOG_INFO("Update check: {} is the newest release (latest {})", current, latest);
    return;
  }
  REXLOG_INFO("Update check: {} is available (this is {})", latest, current);
  {
    std::lock_guard lock(g_mutex);
    g_available = latest;
  }
  notice::Post("HarmonyRecomp " + latest + " is available (you have " + current +
               "). Run the new setup from github.com/" + kRepo +
               "/releases - online play needs everyone on the same version.",
               20.0);
}

}  // namespace

const char* BuildVersion() { return HARMONYRECOMP_VERSION; }

void StartUpdateCheck() {
  if (!REXCVAR_GET(check_for_updates)) return;
  if (ParseVersion(BuildVersion()).empty()) return;  // own build: nothing to compare
  std::thread(Check).detach();
}

std::string AvailableUpdate() {
  std::lock_guard lock(g_mutex);
  return g_available;
}

}  // namespace harmony
