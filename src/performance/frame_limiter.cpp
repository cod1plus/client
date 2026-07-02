// QPC frame limiter: engine ms-math caps maxfps=250 at ~240-248. Keep
// com_maxfps=250 (PB bans >250) but enforce a real 250 by patching only the
// call at 0x0043a4f5 inside the spin-loop. Original fcn.00438a70 untouched.

#include "performance/frame_limiter.h"
#include "core/logger.h"
#include "netcode/version_patch.h" // CODMP_PREFERRED_BASE

#include <cstdio>
#include <immintrin.h>   // _mm_pause — not pulled in transitively by older mingw <windows.h>

namespace patches {

FrameLimiterConfig g_frame_limiter_config = {
    /* enable           */ true,
    /* deadline_bias_us */ 0,
};

namespace {

LARGE_INTEGER g_qpc_freq = {0};
LONGLONG      g_last_frame_qpc = 0;
bool          g_applied = false;

int read_com_maxfps_dvar() {
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) return 0;
    uintptr_t base = (uintptr_t)exe;
    void** slot = (void**)(base + (CODMP_COM_MAXFPS_DVAR_SLOT_VA - CODMP_PREFERRED_BASE));
    void* dvar = *slot;
    if (!dvar) return 0;
    return *(int*)((char*)dvar + CODMP_DVAR_INTEGER_OFFSET);
}

}  // namespace

typedef int (__cdecl *sys_ms_fn)();
static sys_ms_fn g_orig_sys_ms = nullptr; // unused; kept for revisiting orig-call approach

// __cdecl no-arg, returns ms in eax. replaces fcn.00438a70 call in spin-loop.
extern "C" int __cdecl frame_wait_replacement() {
    if (g_qpc_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&g_qpc_freq);
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    int maxfps = read_com_maxfps_dvar();

    if (maxfps > 0 && maxfps <= 10000 && g_last_frame_qpc > 0) {
        const LONGLONG ticks_per_frame = g_qpc_freq.QuadPart / maxfps;
        const LONGLONG bias_ticks = (LONGLONG)g_frame_limiter_config.deadline_bias_us
                                    * g_qpc_freq.QuadPart / 1000000LL;
        const LONGLONG deadline = g_last_frame_qpc + ticks_per_frame + bias_ticks;

        while (now.QuadPart < deadline) {
            // sleep with slack, spin the last <1.5ms
            const LONGLONG remaining_us =
                ((deadline - now.QuadPart) * 1000000LL) / g_qpc_freq.QuadPart;
            if (remaining_us > 1500) {
                Sleep(1);
            } else {
                _mm_pause();
            }
            QueryPerformanceCounter(&now);
        }
    }
    g_last_frame_qpc = now.QuadPart;

    // ms from QPC. engine stores at [0x01912ad0], compares vs [0x008eda90]; consistent
    // call-to-call so the diff exits the loop. (physics breakage was server sv_fps, not this)
    return (int)((now.QuadPart * 1000) / g_qpc_freq.QuadPart);
}

bool apply_frame_limiter_patch() {
    if (g_applied) return true;
    if (!g_frame_limiter_config.enable) {
        logger::logf("frame_limiter: disabled in config, skipping");
        return true;
    }

    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) {
        logger::logf("frame_limiter: GetModuleHandleA(NULL) returned null");
        return false;
    }

    const uintptr_t exe_base = (uintptr_t)exe;
    const uintptr_t opcode_addr  = exe_base + CODMP_FRAME_LIMIT_CALL_OPCODE_RVA;
    const uintptr_t operand_addr = exe_base + CODMP_FRAME_LIMIT_CALL_OPERAND_RVA;

    // expect CALL rel32
    const uint8_t opcode = *(const uint8_t*)opcode_addr;
    if (opcode != 0xE8) {
        logger::logf("frame_limiter: opcode inattendu (0x%02x, attendu 0xE8)", opcode);
        return false;
    }

    // verify current target == fcn.00438a70
    const int32_t current_offset = *(const int32_t*)operand_addr;
    const uintptr_t current_target = (uintptr_t)((intptr_t)opcode_addr + 5 + current_offset);
    const uintptr_t expected_target = exe_base + (CODMP_FRAME_LIMIT_ORIGINAL_TARGET - CODMP_PREFERRED_BASE);
    if (current_target != expected_target) {
        logger::logf(
            "frame_limiter: cible inattendue 0x%08x (attendu 0x%08x) - patch annule",
            (unsigned)current_target, (unsigned)expected_target);
        return false;
    }

    g_orig_sys_ms = (sys_ms_fn)expected_target;

    const uintptr_t hook = (uintptr_t)&frame_wait_replacement;
    const int32_t new_offset = (int32_t)((intptr_t)hook - (intptr_t)(opcode_addr + 5));

    DWORD old_protect = 0;
    if (!VirtualProtect((void*)operand_addr, 4, PAGE_READWRITE, &old_protect)) {
        logger::logf("frame_limiter: VirtualProtect a echoue");
        return false;
    }
    *(int32_t*)operand_addr = new_offset;
    VirtualProtect((void*)operand_addr, 4, old_protect, &old_protect);

    g_applied = true;
    logger::logf(
        "frame_limiter: call at CoDMP+0x%lx redirige -> our wait (0x%08x)",
        (unsigned long)CODMP_FRAME_LIMIT_CALL_OPCODE_RVA, (unsigned)hook);
    return true;
}

}  // namespace patches
