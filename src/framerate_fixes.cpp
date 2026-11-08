// harmonyrecomp - game logic that counts frames instead of time.
//
// The game measures the time of every frame and hands it to the objects
// (seconds; they mostly scale it by 60 to get "frames at 60 FPS"). A few
// places still add 1 per frame, written for the Xbox's fixed 60 FPS; above
// 60 FPS (frame rate limit 90/120/144/unlocked) those run faster. Each is
// patched with a mid-asm hook (harmonyrecomp_config_tu2.toml) right before
// the "fadds f0,f0,f27" (f27 = 1.0) that adds the frame: the hook adds the
// frame's time in 60 FPS frames instead (f28 = the frame time in seconds).

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(fix_frame_counters, true, "HarmonyRecomp",
                    "Enemy logic that counts frames uses the frame time instead (same behaviour "
                    "above 60 FPS); false = the original per-frame counting, for comparison");

// Amalaric Sniper (enemy "amala", update sub_822EE1F8): the counter at
// +0xF7F8 counts how long the player has been in its sight before it draws
// the bow (0x822EE824), and then turns the bow towards the player
// (0x822EFCE8, 0x822F0384, 0x822F0898: 5 degrees per frame). At 144 FPS it reacted and
// aimed 2.4 times as fast.
void HarmonyFrameStep(PPCRegister& f0, PPCRegister& f28) {
  if (!REXCVAR_GET(fix_frame_counters)) return;
  static int logged = 0;
  if (logged < 5) {
    ++logged;
    REXLOG_INFO("[fps-fix] frame step: counter {} + {} (frame time {} s)", f0.f64, f28.f64 * 60.0,
                f28.f64);
  }
  f0.f64 += f28.f64 * 60.0 - 1.0;
}
