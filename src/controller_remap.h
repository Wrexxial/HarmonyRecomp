// harmonyrecomp - controller button remapping (Controls tab of the F6 menu).
//
// Each game button has a setting naming the physical controller button that
// presses it: remap_a = "B" makes the B button act as A in the game. The
// mapping is applied by the SDK's input system (SDK change 17) to real
// controllers only; keyboard/mouse emulation has its own keybind_* settings.
// The SDK's own dialogs always see the unmapped controller (A confirms).

#pragma once

#include <cstddef>

namespace rex {
class Runtime;
}

namespace harmony {

// Buttons in the order shown in the menu.
struct PadButton {
  const char* name;    // value used in the settings ("A", "LT", "Up", ...)
  const char* label;   // shown in the menu
  const char* cvar;    // remap_* setting for this game button
};
const PadButton* PadButtons(size_t* count);

// Installs the filter; reads the remap_* settings.
void InstallControllerRemap(rex::Runtime* runtime);
// Re-reads the remap_* settings after the menu changed them.
void ReloadControllerRemap();
// Sets every remap_* setting back to its own button.
void ResetControllerRemap();

}  // namespace harmony
