#ifndef COD1RELOADED_FRAME_LIMITER_H
#define COD1RELOADED_FRAME_LIMITER_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// CoDMP.exe RVAs, preferred base 0x00400000. spin-loop fcn.0043a3d0 (Com_Frame):
//   0043a4f5 call fcn.00438a70 ; 0043a517 jl 0043a4f5 (spin while elapsed<target)
// we repoint only the call operand at frame_wait_replacement; 00438a70 untouched (called elsewhere).
constexpr uintptr_t CODMP_FRAME_LIMIT_CALL_OPCODE_RVA   = 0x0003a4f5; // 'E8'
constexpr uintptr_t CODMP_FRAME_LIMIT_CALL_OPERAND_RVA  = 0x0003a4f6; // rel32
constexpr uintptr_t CODMP_FRAME_LIMIT_ORIGINAL_TARGET   = 0x00438a70;

// com_maxfps dvar ptr slot (mov [data.01912acc],eax after Cvar_Get); integer at +0x20
constexpr uintptr_t CODMP_COM_MAXFPS_DVAR_SLOT_VA       = 0x01912acc;
constexpr uintptr_t CODMP_DVAR_INTEGER_OFFSET           = 0x20;

// The engine's clock. Sys_Milliseconds() is inlined at every call site (~50 of them) as
//     if (!sys_timeInitialized) { sys_timeBase = timeGetTime(); sys_timeInitialized = 1; }
//     return timeGetTime() - sys_timeBase;
// ms since the game started. Everything time-stamped in the engine is on this clock: the
// key events (+forward <key> <time>), the mouse events, the packets, cls.realtime.
constexpr uintptr_t CODMP_SYS_TIMEBASE_VA               = 0x01999d40;
constexpr uintptr_t CODMP_SYS_TIMEINIT_VA               = 0x01460254;
// Com_Frame's lastTime: com_frameTime of the previous frame (0043a523 mov [8eda90], eax)
constexpr uintptr_t CODMP_COM_LASTTIME_VA               = 0x008eda90;

struct FrameLimiterConfig {
    bool enable;
    int  deadline_bias_us; // +us added to deadline; -500 -> effective 250.5 if running under
};

extern FrameLimiterConfig g_frame_limiter_config;

// What the wait reads from the engine, behind pointers so tools/test_frame_limiter.cpp
// can run the very same wait against a fake engine and a fake Com_Frame loop.
struct FrameLimiterEngine {
    int  (*maxfps)();          // com_maxfps->integer (<= 0: no cap)
    int  (*engine_ms)();       // Sys_Milliseconds()
    int* last_time;            // Com_Frame's lastTime
};

// One call of Com_Frame's spin loop: waits for the frame's deadline, returns the value
// Com_Frame stores in com_frameTime (see frame_limiter.cpp for the two rules it obeys).
int frame_wait(const FrameLimiterEngine& e);
void frame_limiter_reset();    // forget the cadence (tests)

bool apply_frame_limiter_patch();

}  // namespace patches

#endif
