// harmonyrecomp - in-game graphics menu (F6) and FPS counter (F2)

#include "graphics_menu.h"

#include <iterator>
#include <string_view>
#include <utility>

#include <imgui.h>
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(show_fps, false, "HarmonyRecomp", "Show the FPS counter (toggle with F2)");

namespace harmony {
namespace {

std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }
void Set(const char* name, std::string_view value) { rex::cvar::SetFlagByName(name, value); }
bool GetBool(const char* name) {
  const std::string v = Get(name);
  return v == "true" || v == "1";
}
void SetBool(const char* name, bool on) { Set(name, on ? "true" : "false"); }

struct Choice {
  const char* label;
  const char* value;
};

// Combo box bound to a setting. Unknown current values are shown as-is.
template <size_t N>
void ComboSetting(const char* label, const char* cvar, const Choice (&choices)[N]) {
  const std::string current = Get(cvar);
  int index = -1;
  for (size_t i = 0; i < N; ++i) {
    if (current == choices[i].value) {
      index = static_cast<int>(i);
    }
  }
  const char* preview = index >= 0 ? choices[index].label : current.c_str();
  if (ImGui::BeginCombo(label, preview)) {
    for (size_t i = 0; i < N; ++i) {
      const bool selected = static_cast<int>(i) == index;
      if (ImGui::Selectable(choices[i].label, selected)) {
        Set(cvar, choices[i].value);
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
}

void RestartHint(const std::string& start_value, const char* cvar) {
  if (Get(cvar) != start_value) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "restart");
  }
}

// anisotropic_override: -1 = game decides, else xenos::AnisoFilter
// (0 off, 1 = 1:1, 2 = 2:1, 3 = 4:1, 4 = 8:1, 5 = 16:1).
constexpr Choice kAniso[] = {
    {"Game default", "-1"}, {"Off", "0"}, {"2x", "2"},
    {"4x", "3"},                 {"8x", "4"},  {"16x", "5"},
};
// resolution_scale multiplies the guest's 1280x720 render targets.
constexpr Choice kScale[] = {
    {"1x (1280x720)", "1"},
    {"2x (2560x1440)", "2"},
    {"3x (3840x2160)", "3"},
};
constexpr Choice kAntiAlias[] = {
    {"Off", "none"},
    {"FXAA", "fxaa"},
    {"FXAA strong", "fxaa_extreme"},
};

}  // namespace

bool ShowFps() { return REXCVAR_GET(show_fps); }
void SetShowFps(bool on) { SetBool("show_fps", on); }

namespace {
std::string& ConfigErrorStorage() {
  static std::string error;
  return error;
}
}  // namespace

void SetConfigError(std::string message) { ConfigErrorStorage() = std::move(message); }
const std::string& ConfigError() { return ConfigErrorStorage(); }

//=============================================================================
// FPS counter
//=============================================================================

FpsCounterDialog::FpsCounterDialog(rex::ui::ImGuiDrawer* drawer, FrameCounter guest_frames)
    : ImGuiDialog(drawer),
      guest_frames_(std::move(guest_frames)),
      last_poll_(std::chrono::steady_clock::now()) {}

void FpsCounterDialog::OnDraw(ImGuiIO& io) {
  const auto now = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(now - last_poll_).count();
  if (guest_frames_) {
    // Recompute about four times a second so the number stays readable.
    if (elapsed >= 0.25) {
      const uint32_t count = guest_frames_();
      if (have_sample_) {
        fps_ = static_cast<double>(count - last_count_) / elapsed;
      }
      last_count_ = count;
      have_sample_ = true;
      last_poll_ = now;
    }
  } else {
    fps_ = io.Framerate;
  }

  ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.5f);
  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("##harmony_fps", nullptr, kFlags)) {
    ImGui::Text("%.0f FPS", fps_);
    ImGui::TextDisabled("%.1f ms", fps_ > 0.0 ? 1000.0 / fps_ : 0.0);
  }
  ImGui::End();
}

//=============================================================================
// Graphics menu
//=============================================================================

GraphicsMenuDialog::GraphicsMenuDialog(rex::ui::ImGuiDrawer* drawer,
                                       std::filesystem::path config_path, ToggleFps toggle_fps)
    : ImGuiDialog(drawer),
      config_path_(std::move(config_path)),
      toggle_fps_(std::move(toggle_fps)),
      start_scale_(Get("resolution_scale")),
      start_aa_(Get("swap_post_effect")),
      start_fullscreen_(Get("fullscreen")) {}

void GraphicsMenuDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Graphics (F6 to close)", nullptr,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  if (!ConfigError().empty()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                       "harmonyrecomp.toml was invalid and was ignored at startup:");
    ImGui::TextWrapped("%s", ConfigError().c_str());
    ImGui::TextDisabled("\"Save\" writes a valid file.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();
  }

  ImGui::TextDisabled("Applies immediately");
  bool vsync = GetBool("vsync");
  // The SDK's "vsync" drives the game's own 60 Hz frame clock (guest vblank),
  // so it is the frame rate lock: off, the game runs as fast as it can.
  if (ImGui::Checkbox("60 FPS lock (VSync)", &vsync)) {
    SetBool("vsync", vsync);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Keeps the game at 60 FPS like on the Xbox 360. Leave it on.");
  }
  bool fps = ShowFps();
  if (ImGui::Checkbox("FPS counter (F2)", &fps) && toggle_fps_) {
    toggle_fps_(fps);
  }
  ComboSetting("Anisotropic filtering", "anisotropic_override", kAniso);
  ImGui::TextDisabled("  applies to newly loaded textures");

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextDisabled("Applies after a restart");
  ComboSetting("Resolution", "resolution_scale", kScale);
  RestartHint(start_scale_, "resolution_scale");
  ComboSetting("Anti-aliasing", "swap_post_effect", kAntiAlias);
  RestartHint(start_aa_, "swap_post_effect");
  bool fullscreen = GetBool("fullscreen");
  if (ImGui::Checkbox("Start in fullscreen", &fullscreen)) {
    SetBool("fullscreen", fullscreen);
  }
  RestartHint(start_fullscreen_, "fullscreen");

  ImGui::Spacing();
  ImGui::Separator();
  if (ImGui::Button("Save")) {
    rex::cvar::SaveConfig(config_path_);
    status_ = "saved to " + config_path_.filename().string();
    SetConfigError({});
  }
  if (!status_.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status_.c_str());
  }

  ImGui::End();
}

}  // namespace harmony
