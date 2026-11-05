// harmonyrecomp - save data: where it is and a one-click backup (F6 > Saves).

#include "saves.h"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <rex/logging.h>

namespace harmony::saves {
namespace {

std::mutex g_mutex;
std::filesystem::path g_root;

// The offline profile's content: <profile XUID>/<title ID>/<saved game type>.
constexpr const char* kProfile = "B13EBABEBABEBABE";
constexpr const char* kTitle = "58410A7A";
constexpr const char* kSavedGames = "00000001";

}  // namespace

void SetUserDataRoot(const std::filesystem::path& root) {
  std::lock_guard lock(g_mutex);
  g_root = root;
}

std::filesystem::path SaveFolder() {
  std::lock_guard lock(g_mutex);
  return g_root / kProfile / kTitle / kSavedGames;
}

std::filesystem::path BackupFolder() {
  std::lock_guard lock(g_mutex);
  return g_root / "save-backups";
}

std::string BackupNow() {
  const auto source = SaveFolder();
  std::error_code ec;
  if (!std::filesystem::is_directory(source, ec)) {
    return "no save data yet (" + source.string() + ")";
  }
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", &tm);
  const auto target = BackupFolder() / stamp;
  std::filesystem::create_directories(target, ec);
  if (ec) return "could not create " + target.string() + ": " + ec.message();
  int files = 0;
  for (auto it = std::filesystem::recursive_directory_iterator(source, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    const auto rel = std::filesystem::relative(it->path(), source, ec);
    if (ec) break;
    if (it->is_directory(ec)) {
      std::filesystem::create_directories(target / rel, ec);
    } else if (it->is_regular_file(ec)) {
      std::filesystem::copy_file(it->path(), target / rel,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      if (!ec) ++files;
    }
    if (ec) break;
  }
  if (ec) {
    REXLOG_WARN("[saves] backup to {} failed: {}", target.string(), ec.message());
    return "backup failed: " + ec.message();
  }
  REXLOG_INFO("[saves] backed up {} file(s) to {}", files, target.string());
  return "backed up " + std::to_string(files) + " file(s) to save-backups/" + stamp;
}

int BackupCount() {
  std::error_code ec;
  int count = 0;
  for (auto it = std::filesystem::directory_iterator(BackupFolder(), ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (it->is_directory(ec)) ++count;
  }
  return count;
}

void OpenFolder(const std::filesystem::path& folder) {
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
#ifdef _WIN32
  ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
  // Double fork: the file manager is not our child, so nothing to reap.
  pid_t child = fork();
  if (child == 0) {
    if (fork() == 0) {
      execlp("xdg-open", "xdg-open", folder.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    _exit(0);
  }
  if (child > 0) waitpid(child, nullptr, 0);
#endif
}

}  // namespace harmony::saves
