// harmonyrecomp - in-game graphics menu (F6) and FPS counter (F2)
//
// Both windows exist only while they are switched on: the SDK's ImGui drawer
// hooks window input and repaints continuously as long as any dialog exists,
// so nothing changes for the game while they are closed.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

#include "settings_tabs.h"

namespace harmony {

// Value of the show_fps setting (saved in harmonyrecomp.toml).
bool ShowFps();
void SetShowFps(bool on);

// Parse error of harmonyrecomp.toml at startup, empty if it was fine. The SDK
// ignores an unreadable file completely, so the graphics menu shows this.
void SetConfigError(std::string message);
const std::string& ConfigError();

// Small FPS readout in the top-left corner. Counts guest frames (the game's own
// buffer swaps), not window repaints.
class FpsCounterDialog final : public rex::ui::ImGuiDialog {
 public:
  using FrameCounter = std::function<uint32_t()>;
  FpsCounterDialog(rex::ui::ImGuiDrawer* drawer, FrameCounter guest_frames);
  // Display only: keyboard/mouse mode keeps the mouse (SDK change 14).
  bool WantsMouse() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  FrameCounter guest_frames_;
  std::chrono::steady_clock::time_point last_poll_;
  uint32_t last_count_ = 0;
  bool have_sample_ = false;
  double fps_ = 0.0;
};

// Settings window (F6) with Graphics, Controls and Online tabs. Changes apply to
// the running session where possible; "Save" writes them to harmonyrecomp.toml.
class GraphicsMenuDialog final : public rex::ui::ImGuiDialog {
 public:
  using ToggleFps = std::function<void(bool)>;
  GraphicsMenuDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                     ToggleFps toggle_fps);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void DrawGraphics();

  std::filesystem::path config_path_;
  ToggleFps toggle_fps_;
  std::string status_;
  // Values at startup, to flag settings that only apply after a restart.
  std::string start_scale_;
  std::string start_aa_;
  std::string start_fullscreen_;
  std::string start_language_;
  ControlsTab controls_;
  OnlineTab online_;
};

}  // namespace harmony
