#ifndef COD1RELOADED_FRAME_LIMITER_H
#define COD1RELOADED_FRAME_LIMITER_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// CoDMP.exe RVAs, preferred base 0x00400000. spin-loop fcn.0043a3d0:
//   0043a4f5 call fcn.00438a70 ; 0043a517 jl 0043a4f5 (spin while elapsed<target)
// we repoint only the call operand at frame_wait_replacement; 00438a70 untouched (called elsewhere).
constexpr uintptr_t CODMP_FRAME_LIMIT_CALL_OPCODE_RVA   = 0x0003a4f5; // 'E8'
constexpr uintptr_t CODMP_FRAME_LIMIT_CALL_OPERAND_RVA  = 0x0003a4f6; // rel32
constexpr uintptr_t CODMP_FRAME_LIMIT_ORIGINAL_TARGET   = 0x00438a70;

// com_maxfps dvar ptr slot (mov [data.01912acc],eax after Cvar_Get); integer at +0x20
constexpr uintptr_t CODMP_COM_MAXFPS_DVAR_SLOT_VA       = 0x01912acc;
constexpr uintptr_t CODMP_DVAR_INTEGER_OFFSET           = 0x20;

struct FrameLimiterConfig {
    bool enable;
    int  deadline_bias_us; // +us added to deadline; -500 -> effective 250.5 if running under
};

extern FrameLimiterConfig g_frame_limiter_config;

bool apply_frame_limiter_patch();

}  // namespace patches

#endif
