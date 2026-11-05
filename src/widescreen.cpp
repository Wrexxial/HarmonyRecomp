// harmonyrecomp - experimental: other picture aspect ratios (16:10, 21:9, 32:9).
//
// The game picks its back buffer size once at startup from the video mode:
// 1920x1080 or 1280x720 (sub_825C7210 in 2.04), stores it at 0x8291CC70 /
// 0x8291CC6C and creates the Direct3D device with it. Two mid-asm hooks
// (harmonyrecomp_config_tu2.toml) run right before those stores and replace
// the size, so the game renders a wider (or taller) picture; the presenter
// letterboxes it into the window. What the game does with the extra room
// (camera, HUD) is what this experiment is for.

#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_STRING(aspect_ratio, "16:9", "Video",
                      "Experimental: the game's picture aspect ratio, e.g. 16:10, 21:9, 32:9 "
                      "(16:9 is the original; needs a restart)");

namespace {

// "21:9" -> 21, 9; false for anything else.
bool ParseRatio(const std::string& text, int* num, int* den) {
  int a = 0, b = 0;
  char sep = 0;
  if (std::sscanf(text.c_str(), "%d%c%d", &a, &sep, &b) != 3 || (sep != ':' && sep != 'x') ||
      a <= 0 || b <= 0) {
    return false;
  }
  *num = a;
  *den = b;
  return true;
}

// Sizes in whole EDRAM tiles (80x16 pixels at 32 bits per pixel, a width of
// two tiles keeps texture rows 32-pixel aligned too): 2520 wide (31.5 tiles)
// showed the bottom rows wrapped to the top.
uint32_t RoundTo(double value, uint32_t step) {
  return static_cast<uint32_t>((value + step / 2.0) / step) * step;
}

}  // namespace

// r11 = back buffer width, r10 = height (the values the game is about to store).
void HarmonyBackBufferSize(PPCRegister& r10, PPCRegister& r11) {
  const uint32_t width = r11.u32;
  const uint32_t height = r10.u32;
  int num = 16, den = 9;
  if (!ParseRatio(REXCVAR_GET(aspect_ratio), &num, &den) || num * 9 == den * 16) return;
  uint32_t new_width = width;
  uint32_t new_height = height;
  if (num * 9 > den * 16) {
    new_width = RoundTo(double(height) * num / den, 160);  // wider: keep the height
  } else {
    new_height = RoundTo(double(width) * den / num, 16);  // taller: keep the width
  }
  REXLOG_INFO("[widescreen] back buffer {}x{} -> {}x{} ({}:{})", width, height, new_width,
              new_height, num, den);
  r11.u64 = new_width;
  r10.u64 = new_height;
  // The presenter shapes the picture by the video mode (16:9); make it use the
  // picture's own shape, so it is letterboxed instead of squeezed (SDK change 31).
  rex::cvar::SetFlagByName("present_guest_aspect", "true");
}
