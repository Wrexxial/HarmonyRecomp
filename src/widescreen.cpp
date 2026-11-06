// harmonyrecomp - experimental: other picture aspect ratios (16:10, 21:9, 32:9).
//
// The game picks its back buffer size once at startup from the video mode:
// 1920x1080 or 1280x720 (sub_825C7210 in 2.04), stores it at 0x8291CC70 /
// 0x8291CC6C and creates the Direct3D device with it. Two mid-asm hooks
// (harmonyrecomp_config_tu2.toml) run right before those stores and replace
// the size, so the game renders a wider (or taller) picture; the presenter
// letterboxes it into the window. What the game does with the extra room
// (camera, HUD) is what this experiment is for.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_STRING(aspect_ratio, "auto", "Video",
                      "Experimental: the game's picture aspect ratio, e.g. 16:10, 21:9, 32:9, or "
                      "an exact size like 1280x800 (16:9 is the original; auto = 16:9, on a "
                      "Steam Deck its screen's 1280x800; needs a restart)");

REXCVAR_DEFINE_BOOL(aspect_ratio_ui_fix, true, "Video",
                    "With another aspect_ratio: draw HUD and menus in their real shape, centred "
                    "(false: stretched over the whole width like the game does)");

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

uint32_t RoundTo(double value, uint32_t step) {
  return static_cast<uint32_t>((value + step / 2.0) / step) * step;
}

// The Xbox 360 renders into 10 MB of EDRAM. The game always splits the screen
// into two horizontal strips (predicated tiling), each of which must fit with
// 8 bytes per pixel (colour + depth): at 2560 wide only 512 of 540 rows fit,
// and the missing bottom rows showed a copy of the top. 2400x1080 fits.
constexpr uint64_t kEdramBytes = 10ull * 1024 * 1024;
bool FitsEdram(uint32_t width, uint32_t height) {
  const uint64_t rows = (height / 2 + 15) / 16 * 16;  // one strip, in whole 16-row tiles
  return uint64_t(width) * rows * 8 <= kEdramBytes;
}

// The biggest picture of the ratio num:den that fits, starting at the game's
// own size: width in steps of 160 (two 80-pixel tiles), height in steps of 16.
// Of the heights that fit (up to 64 rows below the first), the one whose
// rounded width comes closest to the ratio wins.
void FitRatio(int num, int den, uint32_t width, uint32_t height, uint32_t* out_w,
              uint32_t* out_h) {
  const double ratio = double(num) / den;
  uint32_t start = num * 9 > den * 16 ? height : RoundTo(double(width) * den / num, 16);
  *out_w = width;
  *out_h = height;
  double best = 1e9;
  uint32_t first = 0;
  for (uint32_t h = start; h >= height / 2; h -= 16) {
    const uint32_t w = RoundTo(h * ratio, 160);
    if (!FitsEdram(w, h)) continue;
    if (!first) first = h;
    if (h + 64 < first) break;
    const double error = std::abs(double(w) / h - ratio) / ratio;
    if (error < best - 0.001) {
      best = error;
      *out_w = w;
      *out_h = h;
    }
  }
}

// Steam sets SteamDeck=1 for games it starts on a Steam Deck; started from
// the desktop without Steam, the board name tells (Jupiter: LCD, Galileo: OLED).
bool IsSteamDeck() {
  const char* env = std::getenv("SteamDeck");
  if (env && std::string(env) == "1") return true;
#if !defined(_WIN32)
  if (FILE* f = std::fopen("/sys/devices/virtual/dmi/id/product_name", "r")) {
    char name[64] = {};
    const bool read = std::fgets(name, sizeof(name), f) != nullptr;
    std::fclose(f);
    const std::string product = read ? name : "";
    return product.rfind("Jupiter", 0) == 0 || product.rfind("Galileo", 0) == 0;
  }
#endif
  return false;
}

// The picture the game renders, once HarmonyBackBufferSize changed it (0: unchanged).
uint32_t g_width = 0;
uint32_t g_height = 0;

}  // namespace

// r11 = back buffer width, r10 = height (the values the game is about to store).
void HarmonyBackBufferSize(PPCRegister& r10, PPCRegister& r11) {
  const uint32_t width = r11.u32;
  const uint32_t height = r10.u32;
  int num = 16, den = 9;
  std::string setting = REXCVAR_GET(aspect_ratio);
  if (setting == "auto") {
    // On a Steam Deck: render at its screen's native 1280x800 (16:10)
    // instead of 16:9 with black bars.
    setting = IsSteamDeck() ? "1280x800" : "16:9";
  }
  if (!ParseRatio(setting, &num, &den) || num * 9 == den * 16) return;
  uint32_t new_width = width;
  uint32_t new_height = height;
  if (num >= 320 && den >= 180) {
    // An exact size ("2400x1080"), used as it is.
    new_width = static_cast<uint32_t>(num);
    new_height = static_cast<uint32_t>(den);
    if (!FitsEdram(new_width, new_height)) {
      REXLOG_WARN("[widescreen] {}x{} is too big for the game's EDRAM tiling: the bottom "
                  "rows will show a copy of the top", new_width, new_height);
    }
  } else {
    FitRatio(num, den, width, height, &new_width, &new_height);
  }
  REXLOG_INFO("[widescreen] back buffer {}x{} -> {}x{} ({}:{})", width, height, new_width,
              new_height, num, den);
  r11.u64 = new_width;
  r10.u64 = new_height;
  g_width = new_width;
  g_height = new_height;
  // The presenter shapes the picture by the video mode (16:9); make it use the
  // picture's own shape, so it is letterboxed instead of squeezed (SDK change 31).
  rex::cvar::SetFlagByName("present_guest_aspect", "true");
}

// The game's scene layers (sub_82471B10 builds them once at startup; names in
// a table at 0x828F55C4): the world layers are projected with the real back
// buffer size, but the UI layers (SRN2D_UI_GAME, _UI_BG, _UI_FG) with a fixed
// 1920x1080 orthographic projection, scale (1/960, 1/540, ...) from
// 0x821F9A70, and SRN2D_UI_PERS with a fixed 16:9 aspect (0.5625 at
// 0x821DF9B4). On a wider picture that stretched the HUD, the menus and the
// title pictures. The hooks give them the picture's real shape, centred like
// before.

// Right after "lvx128 v63,r0,r23" (0x82471D88) loaded the UI scale vector
// (1/960, 1/540, ...). A wider picture gets a wider UI span (the UI keeps its
// 1080 rows), a taller one (16:10) a taller span (it keeps its 1920 columns),
// so nothing of the UI is cut off.
void HarmonyUiOrthoScale(PPCVRegister& v63) {
  if (!g_width || !g_height || !REXCVAR_GET(aspect_ratio_ui_fix)) return;
  const double aspect = double(g_width) / double(g_height);
  const bool wider = aspect * 9.0 > 16.0;
  const float find = wider ? 1.0f / 960.0f : 1.0f / 540.0f;
  const float half = wider ? float(540.0 * aspect) : float(960.0 / aspect);
  for (float& value : v63.f32) {
    if (std::abs(value - find) < 1e-7f) {
      value = 1.0f / half;
      REXLOG_INFO("[widescreen] UI layers: {:.0f}x{:.0f} units instead of 1920x1080",
                  wider ? 2 * half : 1920.0f, wider ? 1080.0f : 2 * half);
      return;
    }
  }
  REXLOG_WARN("[widescreen] UI scale vector not found: the UI stays stretched");
}

// Right before "fmuls f0,f13,f0" (0x82471E1C): f0 is the 16:9 aspect factor
// that turns the vertical scale into the horizontal one. Only for wider
// pictures: on a taller one this would cut the sides off.
void HarmonyUiPerspectiveAspect(PPCRegister& f0) {
  if (!g_width || !g_height || !REXCVAR_GET(aspect_ratio_ui_fix)) return;
  if (uint64_t(g_width) * 9 <= uint64_t(g_height) * 16) return;
  if (std::abs(f0.f64 - 0.5625) > 1e-6) {
    REXLOG_WARN("[widescreen] UI perspective: unexpected factor {}", f0.f64);
    return;
  }
  f0.f64 = double(g_height) / double(g_width);
}
