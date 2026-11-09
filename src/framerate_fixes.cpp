// harmonyrecomp - game logic that counts frames instead of time.
//
// The game measures the time of every frame and hands it to the objects
// (seconds; they mostly scale it by 60 to get "frames at 60 FPS"). A few
// places still add 1 per frame, written for the Xbox's fixed 60 FPS; above
// 60 FPS (frame rate limit 90/120/144/unlocked) those run faster. Each is
// patched with a mid-asm hook (harmonyrecomp_config_tu2.toml) right before
// the "fadds f0,f0,f27" (f27 = 1.0) that adds the frame: the hook adds the
// frame's time in 60 FPS frames instead (f28 = the frame time in seconds).

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(fix_frame_counters, true, "HarmonyRecomp",
                    "Enemy logic that counts frames uses the frame time instead (same behaviour "
                    "above 60 FPS); false = the original per-frame counting, for comparison");

// Amalaric Sniper (enemy "amala", update sub_822EE1F8): the counter at
// +0xF7F8 turns the bow towards the player (0x822EFCE8, 0x822F0384,
// 0x822F0898: 5 degrees per frame); at 144 FPS it aimed 2.4 times as fast.
// Its appear check (0x822EE824) stays per frame: see the config.
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

// Amalaric Sniper, state 4 (hovering near the player): three decisions are
// rolled with the game's random numbers every frame -
//   0x822EF9D4  1 in 60   -> attack (state 9)
//   0x822EFA48  1 in 15   -> swoop at the player when level with him (state 7)
//   0x822EFAF8  "timer > random 0.5..2 s" -> shoot (state 8)
// so above 60 FPS it rolled more often per second and attacked far sooner
// (at 220 FPS the swoop came after ~0.07 s instead of ~0.25 s: "locked on").
// Fix: the rolls only count about 60 times a second. Each sniper keeps a
// tick accumulator (frame time x 60); on frames without a tick the roll is
// still made (same random sequence) but its result is turned into "no".
namespace {
struct SniperTick {
  double acc = 0.0;
  bool tick = true;
};
std::mutex g_sniper_mutex;
std::unordered_map<uint32_t, SniperTick> g_sniper_ticks;

bool SniperTicks(uint32_t obj) {
  std::lock_guard lock(g_sniper_mutex);
  auto it = g_sniper_ticks.find(obj);
  return it == g_sniper_ticks.end() || it->second.tick;
}
}  // namespace

// 0x822EE21C, start of the sniper's update (r31 = the sniper, f1 = frame time).
void HarmonySniperFrame(PPCRegister& r31, PPCRegister& f1) {
  std::lock_guard lock(g_sniper_mutex);
  if (g_sniper_ticks.size() > 256) g_sniper_ticks.clear();  // old snipers
  SniperTick& s = g_sniper_ticks[r31.u32];
  s.acc += f1.f64 * 60.0;
  s.tick = s.acc >= 0.999;  // 59.94 FPS frames count as a tick
  if (s.tick) s.acc = std::min(s.acc - 1.0, 1.0);
  if (s.acc < 0.0) s.acc = 0.0;
}

// After "random 0..N" (r3; 0 = do it): no tick -> 1.
void HarmonySniperRollInt(PPCRegister& r3, PPCRegister& r31) {
  if (!REXCVAR_GET(fix_frame_counters) || SniperTicks(r31.u32)) return;
  r3.u32 = 1;
}

// After "random 0.5..2" (f1; the timer must be above it): no tick -> huge.
void HarmonySniperRollFloat(PPCRegister& f1, PPCRegister& r31) {
  if (!REXCVAR_GET(fix_frame_counters) || SniperTicks(r31.u32)) return;
  f1.f64 = 1.0e30;
}
