// harmonyrecomp - controller button remapping.

#include "controller_remap.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_STRING(remap_a, "A", "Input/Controller remap", "Physical button for the game's A");
REXCVAR_DEFINE_STRING(remap_b, "B", "Input/Controller remap", "Physical button for the game's B");
REXCVAR_DEFINE_STRING(remap_x, "X", "Input/Controller remap", "Physical button for the game's X");
REXCVAR_DEFINE_STRING(remap_y, "Y", "Input/Controller remap", "Physical button for the game's Y");
REXCVAR_DEFINE_STRING(remap_lb, "LB", "Input/Controller remap", "Physical button for the game's LB");
REXCVAR_DEFINE_STRING(remap_rb, "RB", "Input/Controller remap", "Physical button for the game's RB");
REXCVAR_DEFINE_STRING(remap_lt, "LT", "Input/Controller remap", "Physical button for the game's LT");
REXCVAR_DEFINE_STRING(remap_rt, "RT", "Input/Controller remap", "Physical button for the game's RT");
REXCVAR_DEFINE_STRING(remap_back, "Back", "Input/Controller remap",
                      "Physical button for the game's Back");
REXCVAR_DEFINE_STRING(remap_start, "Start", "Input/Controller remap",
                      "Physical button for the game's Start");
REXCVAR_DEFINE_STRING(remap_ls, "LS", "Input/Controller remap",
                      "Physical button for the game's left stick press");
REXCVAR_DEFINE_STRING(remap_rs, "RS", "Input/Controller remap",
                      "Physical button for the game's right stick press");
REXCVAR_DEFINE_STRING(remap_up, "Up", "Input/Controller remap",
                      "Physical button for the game's D-pad up");
REXCVAR_DEFINE_STRING(remap_down, "Down", "Input/Controller remap",
                      "Physical button for the game's D-pad down");
REXCVAR_DEFINE_STRING(remap_left, "Left", "Input/Controller remap",
                      "Physical button for the game's D-pad left");
REXCVAR_DEFINE_STRING(remap_right, "Right", "Input/Controller remap",
                      "Physical button for the game's D-pad right");

namespace harmony {
namespace {

constexpr size_t kCount = 16;
constexpr uint8_t kNone = 0xFF;
constexpr size_t kLT = 6;
constexpr size_t kRT = 7;
// Pressing a trigger halfway counts as a button press.
constexpr uint8_t kTriggerThreshold = 30;

constexpr PadButton kButtons[kCount] = {
    {"A", "A", "remap_a"},
    {"B", "B", "remap_b"},
    {"X", "X", "remap_x"},
    {"Y", "Y", "remap_y"},
    {"LB", "LB (left bumper)", "remap_lb"},
    {"RB", "RB (right bumper)", "remap_rb"},
    {"LT", "LT (left trigger)", "remap_lt"},
    {"RT", "RT (right trigger)", "remap_rt"},
    {"Back", "Back", "remap_back"},
    {"Start", "Start", "remap_start"},
    {"LS", "Left stick press", "remap_ls"},
    {"RS", "Right stick press", "remap_rs"},
    {"Up", "D-pad up", "remap_up"},
    {"Down", "D-pad down", "remap_down"},
    {"Left", "D-pad left", "remap_left"},
    {"Right", "D-pad right", "remap_right"},
};
// XInput button bit of each entry (0 for the analog triggers).
constexpr uint16_t kBits[kCount] = {
    0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0, 0,
    0x0020, 0x0010, 0x0040, 0x0080, 0x0001, 0x0002, 0x0004, 0x0008,
};

// For each game button: index of the physical button that presses it.
std::array<std::atomic<uint8_t>, kCount> g_source;
std::atomic<bool> g_identity{true};

uint8_t IndexOf(const std::string& name) {
  for (size_t i = 0; i < kCount; ++i) {
    if (name == kButtons[i].name) {
      return static_cast<uint8_t>(i);
    }
  }
  return kNone;  // "None" or anything unknown: the game button is unused
}

void Filter(uint32_t /*user_index*/, rex::input::X_INPUT_GAMEPAD* pad) {
  if (g_identity.load(std::memory_order_relaxed)) {
    return;
  }
  const uint16_t in_buttons = pad->buttons;
  const uint8_t analog[kCount] = {0, 0, 0, 0, 0, 0, pad->left_trigger, pad->right_trigger};
  auto pressed = [&](uint8_t src) -> bool {
    if (src == kLT || src == kRT) {
      return analog[src] > kTriggerThreshold;
    }
    return (in_buttons & kBits[src]) != 0;
  };
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  for (size_t game = 0; game < kCount; ++game) {
    const uint8_t src = g_source[game].load(std::memory_order_relaxed);
    if (src == kNone) {
      continue;
    }
    if (game == kLT || game == kRT) {
      // Trigger from a trigger keeps its analog value; from a button it is
      // fully pressed or released.
      const uint8_t value = (src == kLT || src == kRT) ? analog[src] : (pressed(src) ? 0xFF : 0);
      (game == kLT ? lt : rt) = value;
    } else if (pressed(src)) {
      buttons |= kBits[game];
    }
  }
  // Guide and anything else the table does not cover pass through.
  constexpr uint16_t kMapped = 0xF3FF;
  buttons |= in_buttons & static_cast<uint16_t>(~kMapped);
  pad->buttons = buttons;
  pad->left_trigger = lt;
  pad->right_trigger = rt;
}

}  // namespace

const PadButton* PadButtons(size_t* count) {
  *count = kCount;
  return kButtons;
}

void ReloadControllerRemap() {
  bool identity = true;
  for (size_t i = 0; i < kCount; ++i) {
    const uint8_t src = IndexOf(rex::cvar::GetFlagByName(kButtons[i].cvar));
    g_source[i].store(src, std::memory_order_relaxed);
    identity = identity && src == i;
  }
  g_identity.store(identity);
}

void ResetControllerRemap() {
  for (size_t i = 0; i < kCount; ++i) {
    rex::cvar::SetFlagByName(kButtons[i].cvar, kButtons[i].name);
  }
  ReloadControllerRemap();
}

void InstallControllerRemap(rex::Runtime* runtime) {
  ReloadControllerRemap();
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!input) {
    return;
  }
  input->SetGamepadFilter(&Filter);
  if (!g_identity) {
    REXLOG_INFO("Controller buttons remapped (remap_* settings)");
  }
}

}  // namespace harmony
