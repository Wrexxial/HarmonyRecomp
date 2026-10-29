// harmonyrecomp - runtime fixes for the recompiled game code.
#pragma once

namespace rex {
class Runtime;
}

namespace harmony {

// Replaces entries in the indirect-call table (call after the main module is
// registered, e.g. from OnPostSetup).
void InstallGameFixes(rex::Runtime* runtime);

// 1 ms Windows timer resolution for this process. The game's 60 Hz clock
// (guest vblank) is a thread that sleeps 1 ms at a time; with Windows' default
// 15.6 ms those vblanks came in bursts and the game often ran at 30 FPS with
// stutter (GitHub issue #11; RivaTuner hid it by raising it system-wide).
void RaiseTimerResolution();

}  // namespace harmony
