// harmonyrecomp - DLC packages the player provides.
//
// Harmony of Despair finds downloadable characters and chapters with
// XamContentCreateEnumerator (marketplace content, type 2), mounts each package
// with XamContentCreateEx and loads character modules from it with
// XexLoadImage. The SDK serves that from its content folder
// (<user data>/content/0000000000000000/58410A7A/00000002/<package>/), so the
// packages from dlc/ are unpacked there once with the SDK's STFS reader, which
// also records the package license. The code modules in the packages are
// recompiled at build time (scripts/build.py, step_dlc).
//
// Only packages listed in dlc_packages.txt next to the exe are installed: the
// build writes the packages whose code it recompiled. Any other package (added
// after the build, or one the recompiler could not handle) would crash the
// game as soon as it loads that code, so it is skipped, and an earlier
// installed copy is removed again.

#include "dlc.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>

namespace harmony {
namespace {

constexpr uint32_t kTitleId = 0x58410A7A;
constexpr uint32_t kMarketplaceContent = 0x00000002;
constexpr uint32_t kTitleUpdate = 0x000B0000;

struct PackageInfo {
  uint32_t title_id = 0;
  uint32_t content_type = 0;
};

uint32_t ReadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool ReadPackageInfo(const std::filesystem::path& path, PackageInfo* info) {
  std::ifstream file(path, std::ios::binary);
  std::array<uint8_t, 0x400> header{};
  if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) return false;
  const bool stfs = (header[0] == 'L' && header[1] == 'I' && header[2] == 'V' && header[3] == 'E') ||
                    (header[0] == 'P' && header[1] == 'I' && header[2] == 'R' && header[3] == 'S') ||
                    (header[0] == 'C' && header[1] == 'O' && header[2] == 'N' && header[3] == ' ');
  if (!stfs) return false;
  info->content_type = ReadBe32(&header[0x344]);
  info->title_id = ReadBe32(&header[0x360]);
  return true;
}

std::vector<std::filesystem::path> DlcFolders(const std::filesystem::path& game_data_root) {
  std::vector<std::filesystem::path> folders;
  auto add = [&](const std::filesystem::path& dir) {
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return;
    auto canonical = std::filesystem::weakly_canonical(dir, ec);
    for (const auto& known : folders) {
      if (known == canonical) return;
    }
    folders.push_back(canonical);
  };
  // dlc/ next to assets/ in the project, or next to the exe.
  if (!game_data_root.empty()) add(game_data_root.parent_path() / "dlc");
  add(rex::filesystem::GetExecutableFolder() / "dlc");
  return folders;
}

// Packages the build recompiled. Empty if the list is missing (a build from
// before the list existed): then nothing is installed.
std::set<std::string> BuiltPackages(bool* found) {
  std::set<std::string> names;
  std::ifstream file(rex::filesystem::GetExecutableFolder() / "dlc_packages.txt");
  *found = file.is_open();
  std::string line;
  while (std::getline(file, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) names.insert(line);
  }
  return names;
}

rex::system::xam::XCONTENT_AGGREGATE_DATA ContentData(const std::string& name) {
  rex::system::xam::XCONTENT_AGGREGATE_DATA data;
  data.device_id = 1;  // hard drive
  data.content_type = rex::system::XContentType::kMarketplaceContent;
  data.title_id = kTitleId;
  data.xuid = 0;
  data.set_file_name(name);
  return data;
}

}  // namespace

void InstallDlcPackages(rex::Runtime* runtime) {
  auto* kernel_state = runtime ? runtime->kernel_state() : nullptr;
  auto* content = kernel_state ? kernel_state->content_manager() : nullptr;
  if (!content || !kernel_state->GetExecutableModule()) return;  // title ID needs the game

  bool have_list = false;
  const auto built = BuiltPackages(&have_list);
  if (!have_list) {
    REXLOG_WARN("[dlc] dlc_packages.txt missing next to the exe; no DLC is installed");
  }
  for (const auto& folder : DlcFolders(runtime->game_data_root())) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
      if (!entry.is_regular_file(ec)) continue;
      const auto& path = entry.path();
      PackageInfo info;
      if (!ReadPackageInfo(path, &info)) continue;  // README or other files
      const auto name = path.filename().string();
      if (info.title_id == kTitleId && info.content_type == kTitleUpdate) {
        continue;  // applied over the game data at build time
      }
      if (info.title_id != kTitleId || info.content_type != kMarketplaceContent) {
        REXLOG_WARN("[dlc] {} is not a Harmony of Despair DLC package (title {:08X}, type {:08X})",
                    name, info.title_id, info.content_type);
        continue;
      }
      auto data = ContentData(name);
      if (!built.count(name)) {
        // Its code was not recompiled: the game would crash loading it.
        if (content->ContentExists(0, data) && content->DeleteContent(0, data) == 0) {
          REXLOG_WARN("[dlc] {} was not part of the build; removed its installed copy", name);
        } else {
          REXLOG_WARN("[dlc] {} was not part of the build; run the build (or the setup) "
                      "again with it in the DLC folder", name);
        }
        continue;
      }
      if (content->ContentExists(0, data)) {
        REXLOG_INFO("[dlc] {} is installed", name);
        continue;
      }
      auto result = content->InstallContent(path);
      if (result == 0) {
        REXLOG_INFO("[dlc] installed {}", name);
      } else {
        REXLOG_WARN("[dlc] could not install {} (error {:08X})", name, result);
      }
    }
  }
}

}  // namespace harmony
