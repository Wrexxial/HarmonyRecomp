// harmonyrecomp - Controls tab of the F6 settings menu.

#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <imgui.h>
#include <rex/cvar.h>

#include "controller_remap.h"
#include "settings_tabs.h"

namespace harmony {
namespace {

std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }
void Set(const char* name, std::string_view value) { rex::cvar::SetFlagByName(name, value); }
bool IsTrue(const std::string& v) { return v == "true" || v == "1"; }

void Help(const char* text) {
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", text);
  }
}

// The SDK's keyboard/mouse bindings (mnk_input_driver.cpp) and defaults.
struct KeyBind {
  const char* cvar;
  const char* label;
  const char* default_key;
};
constexpr KeyBind kKeyBinds[] = {
    {"keybind_lstick_up", "Move up", "W"},
    {"keybind_lstick_down", "Move down", "S"},
    {"keybind_lstick_left", "Move left", "A"},
    {"keybind_lstick_right", "Move right", "D"},
    {"keybind_a", "A", "Space"},
    {"keybind_b", "B", "Shift"},
    {"keybind_x", "X", "R"},
    {"keybind_y", "Y", "E"},
    {"keybind_left_shoulder", "LB", "Q"},
    {"keybind_right_shoulder", "RB", "F"},
    {"keybind_left_trigger", "LT", "RMB"},
    {"keybind_right_trigger", "RT", "LMB"},
    {"keybind_back", "Back", "Tab"},
    {"keybind_start", "Start", "Escape"},
    {"keybind_lstick_press", "Left stick press", "C"},
    {"keybind_rstick_press", "Right stick press", "MMB"},
    {"keybind_dpad_up", "D-pad up", "Up"},
    {"keybind_dpad_down", "D-pad down", "Down"},
    {"keybind_dpad_left", "D-pad left", "Left"},
    {"keybind_dpad_right", "D-pad right", "Right"},
};

// Dear ImGui key -> key name the SDK's keybinds understand (rex/ui/keybinds).
struct KeyName {
  ImGuiKey key;
  const char* name;
};
constexpr KeyName kKeyNames[] = {
    {ImGuiKey_Tab, "Tab"},
    {ImGuiKey_LeftArrow, "Left"},
    {ImGuiKey_RightArrow, "Right"},
    {ImGuiKey_UpArrow, "Up"},
    {ImGuiKey_DownArrow, "Down"},
    {ImGuiKey_PageUp, "PageUp"},
    {ImGuiKey_PageDown, "PageDown"},
    {ImGuiKey_Home, "Home"},
    {ImGuiKey_End, "End"},
    {ImGuiKey_Insert, "Insert"},
    {ImGuiKey_Delete, "Delete"},
    {ImGuiKey_Space, "Space"},
    {ImGuiKey_Enter, "Return"},
    {ImGuiKey_Escape, "Escape"},
    {ImGuiKey_LeftCtrl, "Control"},
    {ImGuiKey_RightCtrl, "Control"},
    {ImGuiKey_LeftShift, "Shift"},
    {ImGuiKey_RightShift, "Shift"},
    {ImGuiKey_LeftAlt, "Alt"},
    {ImGuiKey_RightAlt, "Alt"},
    {ImGuiKey_0, "0"}, {ImGuiKey_1, "1"}, {ImGuiKey_2, "2"}, {ImGuiKey_3, "3"},
    {ImGuiKey_4, "4"}, {ImGuiKey_5, "5"}, {ImGuiKey_6, "6"}, {ImGuiKey_7, "7"},
    {ImGuiKey_8, "8"}, {ImGuiKey_9, "9"},
    {ImGuiKey_A, "A"}, {ImGuiKey_B, "B"}, {ImGuiKey_C, "C"}, {ImGuiKey_D, "D"},
    {ImGuiKey_E, "E"}, {ImGuiKey_F, "F"}, {ImGuiKey_G, "G"}, {ImGuiKey_H, "H"},
    {ImGuiKey_I, "I"}, {ImGuiKey_J, "J"}, {ImGuiKey_K, "K"}, {ImGuiKey_L, "L"},
    {ImGuiKey_M, "M"}, {ImGuiKey_N, "N"}, {ImGuiKey_O, "O"}, {ImGuiKey_P, "P"},
    {ImGuiKey_Q, "Q"}, {ImGuiKey_R, "R"}, {ImGuiKey_S, "S"}, {ImGuiKey_T, "T"},
    {ImGuiKey_U, "U"}, {ImGuiKey_V, "V"}, {ImGuiKey_W, "W"}, {ImGuiKey_X, "X"},
    {ImGuiKey_Y, "Y"}, {ImGuiKey_Z, "Z"},
    {ImGuiKey_F1, "F1"}, {ImGuiKey_F2, "F2"}, {ImGuiKey_F3, "F3"}, {ImGuiKey_F4, "F4"},
    {ImGuiKey_F5, "F5"}, {ImGuiKey_F7, "F7"}, {ImGuiKey_F8, "F8"}, {ImGuiKey_F9, "F9"},
    {ImGuiKey_F10, "F10"}, {ImGuiKey_F11, "F11"}, {ImGuiKey_F12, "F12"},
    {ImGuiKey_Apostrophe, "Quote"},
    {ImGuiKey_Comma, "Comma"},
    {ImGuiKey_Minus, "Minus"},
    {ImGuiKey_Period, "Period"},
    {ImGuiKey_Slash, "Slash"},
    {ImGuiKey_Semicolon, "Semicolon"},
    {ImGuiKey_Equal, "Plus"},
    {ImGuiKey_LeftBracket, "LBracket"},
    {ImGuiKey_Backslash, "Backslash"},
    {ImGuiKey_RightBracket, "RBracket"},
    {ImGuiKey_GraveAccent, "Backtick"},
    {ImGuiKey_CapsLock, "CapsLock"},
    {ImGuiKey_Keypad0, "Numpad0"}, {ImGuiKey_Keypad1, "Numpad1"},
    {ImGuiKey_Keypad2, "Numpad2"}, {ImGuiKey_Keypad3, "Numpad3"},
    {ImGuiKey_Keypad4, "Numpad4"}, {ImGuiKey_Keypad5, "Numpad5"},
    {ImGuiKey_Keypad6, "Numpad6"}, {ImGuiKey_Keypad7, "Numpad7"},
    {ImGuiKey_Keypad8, "Numpad8"}, {ImGuiKey_Keypad9, "Numpad9"},
    {ImGuiKey_KeypadDivide, "NumpadSlash"},
    {ImGuiKey_KeypadMultiply, "NumpadStar"},
    {ImGuiKey_KeypadSubtract, "NumpadMinus"},
    {ImGuiKey_KeypadAdd, "NumpadPlus"},
    {ImGuiKey_KeypadEnter, "Return"},
};

constexpr double kCaptureSeconds = 5.0;

}  // namespace

void ControlsTab::Draw() {
  if (ImGui::BeginTabBar("##controls")) {
    if (ImGui::BeginTabItem("Controller")) {
      DrawController();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Keyboard & mouse")) {
      DrawKeyboard();
      ImGui::EndTabItem();
    } else {
      capturing_.clear();
    }
    ImGui::EndTabBar();
  }
}

void ControlsTab::DrawController() {
  ImGui::TextDisabled("Which button does what in the game. Applies immediately;\n"
                      "menus like this one always use A to confirm, B to go back.");
  size_t count = 0;
  const PadButton* buttons = PadButtons(&count);
  bool changed = false;
  if (ImGui::BeginTable("##remap", 2, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("Game button");
    ImGui::TableSetupColumn("Press", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::GetFontSize() * 10.0f);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < count; ++i) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(buttons[i].label);
      ImGui::TableNextColumn();
      const std::string current = Get(buttons[i].cvar);
      const bool remapped = current != buttons[i].name;
      ImGui::PushID(buttons[i].cvar);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (remapped) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
      }
      const bool open = ImGui::BeginCombo("##src", current.empty() ? "None" : current.c_str());
      if (remapped) {
        ImGui::PopStyleColor();
      }
      if (open) {
        for (size_t j = 0; j <= count; ++j) {
          const char* name = j < count ? buttons[j].name : "None";
          const bool selected = current == name;
          if (ImGui::Selectable(j < count ? buttons[j].label : "None (unused)", selected)) {
            Set(buttons[i].cvar, name);
            changed = true;
          }
          if (selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (changed) {
    ReloadControllerRemap();
  }
  if (ImGui::Button("Reset controller")) {
    ResetControllerRemap();
  }
}

void ControlsTab::DrawKeyboard() {
  bool mnk = IsTrue(Get("mnk_mode"));
  if (ImGui::Checkbox("Play with keyboard and mouse", &mnk)) {
    Set("mnk_mode", mnk ? "true" : "false");
  }
  Help("Keyboard and mouse act as a controller (player 1).\n"
       "The mouse is captured while playing; menus like this one release it.");

  float sensitivity = static_cast<float>(std::atof(Get("mnk_sensitivity").c_str()));
  if (sensitivity <= 0.0f) {
    sensitivity = 1.0f;
  }
  if (ImGui::SliderFloat("Mouse sensitivity", &sensitivity, 0.1f, 5.0f, "%.2f")) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", sensitivity);
    Set("mnk_sensitivity", text);
  }

  // A click or key press that sets a binding must not also press a button.
  const bool just_bound = PollCapture();
  ImGui::TextDisabled("Click a key, then press the new key or mouse button.");
  if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("Controller");
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::GetFontSize() * 10.0f);
    ImGui::TableHeadersRow();
    for (const auto& bind : kKeyBinds) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(bind.label);
      ImGui::TableNextColumn();
      ImGui::PushID(bind.cvar);
      const bool waiting = capturing_ == bind.cvar;
      std::string key = Get(bind.cvar);
      const std::string text = waiting ? "press a key..." : key.empty() ? "-" : key;
      if (ImGui::Button(text.c_str(), ImVec2(-FLT_MIN, 0.0f)) && !waiting && !just_bound) {
        capturing_ = bind.cvar;
        capture_armed_ = false;
        capture_start_ = std::chrono::steady_clock::now();
      }
      if (!waiting && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Default: %s", bind.default_key);
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (!capturing_.empty()) {
    ImGui::TextDisabled("Backspace clears the key; wait %.0f s to cancel.", kCaptureSeconds);
  }
  if (ImGui::Button("Reset keyboard")) {
    for (const auto& bind : kKeyBinds) {
      Set(bind.cvar, bind.default_key);
    }
    capturing_.clear();
  }
}

bool ControlsTab::PollCapture() {
  if (capturing_.empty()) {
    return false;
  }
  const double waited =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - capture_start_).count();
  if (waited > kCaptureSeconds) {
    capturing_.clear();
    return false;
  }
  // The click that started the capture must be released before a mouse
  // button can be bound.
  const bool any_mouse_down =
      ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1) || ImGui::IsMouseDown(2);
  if (!capture_armed_) {
    capture_armed_ = !any_mouse_down;
    return false;
  }
  std::string bound;
  if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
    bound = "";
  } else if (ImGui::IsMouseClicked(0)) {
    bound = "LMB";
  } else if (ImGui::IsMouseClicked(1)) {
    bound = "RMB";
  } else if (ImGui::IsMouseClicked(2)) {
    bound = "MMB";
  } else {
    bool found = false;
    for (const auto& entry : kKeyNames) {
      if (ImGui::IsKeyPressed(entry.key, false)) {
        bound = entry.name;
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  Set(capturing_.c_str(), bound);
  capturing_.clear();
  return true;
}

}  // namespace harmony
