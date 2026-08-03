#ifndef COD1RELOADED_FPS_DISPLAY_H
#define COD1RELOADED_FPS_DISPLAY_H

#include <windows.h>
#include <stdint.h>

namespace patches {

struct FpsDisplayConfig {
    // Steady cg_drawFPS 1. cg_drawFPS 2 (min/max) is deliberately left untouched:
    // that mode exists to SHOW jitter, so smoothing it would defeat its purpose.
    bool enable = true;
};

extern FpsDisplayConfig g_fps_display_config;

// cgame_mp_x86.dll, RE'd 2026-08-03.
//   CG_DrawFPS @0x30015ae0, float(float y). Averages the last 32 frame times, taken
//   from trap Milliseconds (syscall 6), and prints 32000 / sum. Integer milliseconds
//   mean the sum wobbles by a millisecond or so even on a perfectly paced 250 fps,
//   and 32000/127..129 = 252/250/248 - which is the flicker.
//   The window cannot simply be widened: the 32-term sum is unrolled in the code.
//     samples[32]  RVA 0x00095ad0
//     index        RVA 0x00095b54
//     prev_ms      RVA 0x00095b58
//   cg_drawFPS vmCvar RVA 0x001dc600, .integer at +12.
constexpr uintptr_t CGAME_CG_DRAWFPS_RVA      = 0x00015ae0;
constexpr uintptr_t CGAME_FPS_SAMPLES_RVA     = 0x00095ad0;
constexpr uintptr_t CGAME_FPS_INDEX_RVA       = 0x00095b54;
constexpr uintptr_t CGAME_CG_DRAWFPS_CVAR_RVA = 0x001dc600;
constexpr int       FPS_SAMPLES               = 32;

// Prologue stolen by the 5-byte jmp: sub esp,0x28 / push esi / push edi = exactly 5.
constexpr size_t    CGAME_CG_DRAWFPS_STEAL    = 5;

void fps_display_install(HMODULE cgame);   // idempotent, re-applied on map change

}  // namespace patches

#endif
