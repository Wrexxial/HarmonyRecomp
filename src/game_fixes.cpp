// harmonyrecomp - runtime fixes for the recompiled game code.

#include "game_fixes.h"

#include <atomic>
#include <cstdint>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xmemory.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

namespace harmony {
namespace {

// _purecall of the game's CRT (sub_82738630 in version 1.04, sub_82754DC8 with
// title update 2): calls the purecall handler and then abort(). It is only
// reachable through vtables.
//
// In online co-op the network library dispatches finished asynchronous
// operations from a list (sub_82789358) on its own thread. Rarely an
// operation's object is already being destroyed on the game thread at that
// point, its vtable is the abstract base, and the completion call lands here -
// which ended a co-op round after ~15 minutes with an abort. The object is
// going away anyway, so the completion is dropped instead.
constexpr uint32_t kPureCallCandidates[] = {0x82738630, 0x82754DC8};

// The candidate that holds _purecall in the loaded executable: prologue
// mflr r12 / stw r12,-8(r1) / stwu r1,-96(r1), then li r3,25 (abort reason).
uint32_t FindPureCall(rex::Runtime* runtime) {
  auto* memory = runtime->memory();
  for (uint32_t address : kPureCallCandidates) {
    auto* code = memory ? memory->TranslateVirtual<const uint8_t*>(address) : nullptr;
    if (code && rex::memory::load_and_swap<uint32_t>(code) == 0x7D8802A6 &&
        rex::memory::load_and_swap<uint32_t>(code + 4) == 0x9181FFF8 &&
        rex::memory::load_and_swap<uint32_t>(code + 8) == 0x9421FFA0 &&
        rex::memory::load_and_swap<uint32_t>(code + 36) == 0x38600019) {
      return address;
    }
  }
  return 0;
}

void PureCallGuard(PPCContext& ctx, uint8_t* base) {
  (void)base;
  static std::atomic<int> count{0};
  int n = ++count;
  if (n <= 20 || n % 1000 == 0) {
    REXLOG_WARN("[fix] pure virtual call from {:08X} (object {:08X}) ignored ({} so far)",
                static_cast<uint32_t>(ctx.lr), ctx.r3.u32, n);
  }
}

}  // namespace

void InstallGameFixes(rex::Runtime* runtime) {
  auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  if (!dispatcher) return;
  const uint32_t pure_call = FindPureCall(runtime);
  if (!pure_call || !dispatcher->SetFunction(pure_call, &PureCallGuard)) {
    REXLOG_WARN("[fix] could not install the _purecall guard");
  }
}

void RaiseTimerResolution() {
#if defined(_WIN32)
  timeBeginPeriod(1);
#endif
}

}  // namespace harmony
