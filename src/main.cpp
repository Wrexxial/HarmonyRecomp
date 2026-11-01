// harmonyrecomp - ReXGlue Recompiled Project

#include "generated/default/harmonyrecomp_init.h"

#include "harmonyrecomp_app.h"

REX_DEFINE_APP(harmonyrecomp, HarmonyrecompApp::Create)

#ifdef _WIN32
// Laptops with a second, faster GPU (NVIDIA Optimus, AMD switchable graphics):
// these exports make the drivers run the game on the fast GPU by default
// instead of the integrated one (which gave ~30 FPS, Discord report).
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
