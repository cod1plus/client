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
LONGLONG      g_next_deadline = 0;   // theoretical, NOT the last wake time
LONGLONG      g_deadline_met  = 0;   // the deadline this frame satisfied
LONGLONG      g_step_frac = 0;       // running remainder of freq/maxfps
int           g_last_maxfps = 0;
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

    if (maxfps > 0 && maxfps <= 10000) {
        // EXACT frame step: freq/maxfps rarely divides evenly, and truncating loses a
        // fraction of a tick every frame - visible as a permanent 1-2 fps shortfall.
        // Carry the remainder so the average is exactly maxfps.
        const LONGLONG step_int = g_qpc_freq.QuadPart / maxfps;
        const LONGLONG step_rem = g_qpc_freq.QuadPart % maxfps;
        const LONGLONG bias_ticks = (LONGLONG)g_frame_limiter_config.deadline_bias_us
                                    * g_qpc_freq.QuadPart / 1000000LL;

        if (maxfps != g_last_maxfps || g_next_deadline == 0) {
            g_last_maxfps   = maxfps;
            g_step_frac     = 0;
            g_next_deadline = now.QuadPart + step_int + bias_ticks;
        }

        g_deadline_met = g_next_deadline;   // the tick we hand back as "now"
        const LONGLONG deadline = g_next_deadline;
        while (now.QuadPart < deadline) {
            const LONGLONG remaining_us =
                ((deadline - now.QuadPart) * 1000000LL) / g_qpc_freq.QuadPart;
            // Sleep(1) can overshoot well past a millisecond even at 1ms timer
            // resolution; at 250fps the whole frame is 4ms, so only sleep while
            // there is real slack and spin the tail.
            if (remaining_us > 2500) {
                Sleep(1);
            } else {
                _mm_pause();
            }
            QueryPerformanceCounter(&now);
        }

        // ANCHOR ON THE DEADLINE, not on the wake time: waking late is normal (sleep
        // granularity, scheduling) and re-anchoring on it made every frame start its
        // budget late, so the rate drifted permanently below the target - the
        // "248 instead of 250" flicker. Advancing the theoretical clock instead makes
        // a late frame steal from the next one and the average stay exact.
        LONGLONG step = step_int;
        g_step_frac += step_rem;
        if (g_step_frac >= maxfps) { g_step_frac -= maxfps; step += 1; }
        g_next_deadline += step;

        // Hitch / alt-tab / map load: if we fell more than a few frames behind, do not
        // sprint to catch up - restart the cadence from now.
        if (now.QuadPart > g_next_deadline + 4 * step_int) {
            g_next_deadline = now.QuadPart + step_int + bias_ticks;
            g_step_frac = 0;
            g_deadline_met = now.QuadPart;   // resync: fall back to real time
        }
    } else {
        g_next_deadline = 0;
        g_deadline_met  = 0;
    }

    // Hand back the THEORETICAL tick we just waited for, not the actual wake time.
    // The engine derives frame time - and everything paced by it - from the delta
    // between successive returns of this function, in whole milliseconds. Real wake
    // times wander by a few microseconds, and truncating them to ms turns that into a
    // 3/4/5 ms jitter: the frame counter reads a locked 250 while the pacing visibly
    // shimmers. The deadline clock advances by exactly one frame step every frame, so
    // the deltas are uniform. It tracks real time (it is built from it and resyncs
    // after any hitch), so engine timing stays honest.
    const LONGLONG t = (g_deadline_met > 0) ? g_deadline_met : now.QuadPart;
    return (int)((t * 1000) / g_qpc_freq.QuadPart);
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
