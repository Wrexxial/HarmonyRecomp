// harmonyrecomp - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/filesystem.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/flags.h>
#include <rex/ui/keybinds.h>

#include "crash_log.h"
#include "dlc.h"
#include "game_fixes.h"
#include "graphics_menu.h"
#include "netplay/netplay.h"

class HarmonyrecompApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<HarmonyrecompApp>(new HarmonyrecompApp(ctx, "harmonyrecomp",
        PPCImageConfig));
  }

  // Remember where the runtime settings live (harmonyrecomp.toml next to the
  // exe), so the graphics menu saves to the same file the F4 menu uses.
  //
  // Also find the game data without --game_data_root. The SDK reads that
  // option before it loads harmonyrecomp.toml, so a value in the file alone
  // would never be used; read it here ourselves, then fall back to the
  // project's assets folder relative to the exe.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    config_path_ = paths.config_path;
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::vector<std::filesystem::path> candidates;
    if (auto from_file = ReadGameDataRootFromConfig(); !from_file.empty()) {
      candidates.push_back(from_file);
    }
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    candidates.push_back(exe_dir / "assets");
    candidates.push_back(exe_dir / ".." / ".." / ".." / "assets");  // out/build/<preset>/
    for (const auto& dir : candidates) {
      std::error_code ec;
      if (std::filesystem::exists(dir / "default.xex", ec)) {
        paths.game_data_root = std::filesystem::weakly_canonical(dir, ec);
        if (ec) {
          paths.game_data_root = dir;
        }
        return;
      }
    }
  }

  // A title update applied at build time leaves default.xexp and *.dllp next
  // to the binaries; the recompiled code was generated from the patched
  // modules, so the runtime must apply the same patches when it loads them.
  // Without a title update there is no patch file and nothing changes.
  void OnLoadXexImage(std::string& xex_image) override {
    (void)xex_image;
    REXCVAR_SET(xex_apply_patches, true);
    // The loader also reads this (SDK change 9): the cvar set here may be a
    // different copy than the one rexruntime.dll checks.
#ifdef _WIN32
    _putenv_s("REX_XEX_APPLY_PATCHES", "1");
#else
    setenv("REX_XEX_APPLY_PATCHES", "1", 1);
#endif
  }

  // Runtime and kernel exist now; start the LAN lobby service if netplay is on.
  void OnPostSetup() override {
    harmony::InstallGameFixes(runtime());
    harmony::netplay::Startup();
  }

  // The game is loaded (title ID known) but its main thread has not started:
  // install the player's DLC packages so the game's content scan finds them.
  void OnPostLaunchModule(rex::system::XThread* thread) override {
    (void)thread;
    harmony::InstallDlcPackages(runtime());
  }

  // Logging is up now; report a settings file the SDK could not parse. The SDK
  // logs that error before the log file exists, so it was invisible so far.
  void OnPostInitLogging() override {
    harmony::InstallCrashLog();
    std::error_code ec;
    if (config_path_.empty() || !std::filesystem::exists(config_path_, ec)) {
      return;
    }
    try {
      (void)toml::parse_file(config_path_.string());
    } catch (const toml::parse_error& err) {
      std::string message = std::string(err.description());
      const auto& where = err.source().begin;
      message += " (line " + std::to_string(where.line) + ", column " +
                 std::to_string(where.column) + ")";
      REXLOG_ERROR("{} is invalid and was ignored: {}", config_path_.filename().string(), message);
      harmony::SetConfigError(std::move(message));
    }
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    drawer_ = drawer;
    rex::ui::RegisterBind("bind_graphics_menu", "F6", "Toggle graphics menu",
                          [this] { ToggleGraphicsMenu(); });
    rex::ui::RegisterBind("bind_fps_counter", "F2", "Toggle FPS counter",
                          [this] { SetFpsCounter(!harmony::ShowFps()); });
    if (harmony::ShowFps()) {
      SetFpsCounter(true);
    }
  }

  // Runs before the SDK tears down the ImGui drawer.
  void OnShutdown() override {
    harmony::netplay::Shutdown();
    rex::ui::UnregisterBind("bind_graphics_menu");
    rex::ui::UnregisterBind("bind_fps_counter");
    graphics_menu_.reset();
    fps_counter_.reset();
    drawer_ = nullptr;
  }

 private:
  std::filesystem::path ReadGameDataRootFromConfig() const {
    std::error_code ec;
    if (config_path_.empty() || !std::filesystem::exists(config_path_, ec)) {
      return {};
    }
    try {
      auto table = toml::parse_file(config_path_.string());
      if (auto value = table["game_data_root"].value<std::string>()) {
        return std::filesystem::path(*value);
      }
    } catch (const toml::parse_error&) {
      // Reported in OnPostInitLogging once logging is available.
    }
    return {};
  }

  // Dialogs are created when switched on and destroyed when switched off.
  // A dialog never destroys itself: the FPS checkbox lives in the graphics
  // menu and only adds/removes the FPS window, which the SDK's drawer handles
  // safely during its draw loop (RemoveDialog adjusts the loop index).
  void ToggleGraphicsMenu() {
    if (graphics_menu_) {
      graphics_menu_.reset();
    } else if (drawer_) {
      graphics_menu_ = std::make_unique<harmony::GraphicsMenuDialog>(
          drawer_, config_path_, [this](bool on) { SetFpsCounter(on); });
    }
  }

  void SetFpsCounter(bool on) {
    harmony::SetShowFps(on);
    if (!on) {
      fps_counter_.reset();
    } else if (!fps_counter_ && drawer_) {
      fps_counter_ = std::make_unique<harmony::FpsCounterDialog>(
          drawer_, [this] { return GuestFrameCount(); });
    }
  }

  // Guest frames = the game's buffer swaps, counted by the GPU command processor.
  uint32_t GuestFrameCount() const {
    auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime() ? runtime()->graphics_system()
                                                                      : nullptr);
    auto* cp = gs ? gs->command_processor() : nullptr;
    return cp ? cp->counter() : 0;
  }

  std::filesystem::path config_path_;
  rex::ui::ImGuiDrawer* drawer_ = nullptr;
  std::unique_ptr<harmony::GraphicsMenuDialog> graphics_menu_;
  std::unique_ptr<harmony::FpsCounterDialog> fps_counter_;
};
