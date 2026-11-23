// QPC frame limiter: engine ms-math caps maxfps=250 at ~240-248. Keep
// com_maxfps=250 (PB bans >250) but enforce a real 250 by patching only the
// call at 0x0043a4f5 inside the spin-loop. Original fcn.00438a70 untouched.
//
// Com_Frame (Q3 verbatim) is
//     do { com_frameTime = Com_EventLoop();  msec = com_frameTime - lastTime; }
//     while (msec < minMsec);                // minMsec = 1000 / com_maxfps, integer
//     ...  lastTime = com_frameTime;
// The patched call lands here. Three rules for what we hand back:
//
// 1. THE ENGINE'S EPOCH (Sys_Milliseconds: ms since launch), never another one.
//    Until 2026-09-25 this returned QPC milliseconds since BOOT. com_frameTime is not
//    just the loop's yardstick: CL_KeyState (0x40b0b0) does `msec += com_frameTime -
//    downtime` and IN_KeyUp (0x40b020) `msec += uptime - downtime`, where downtime /
//    uptime are the key events' Sys_Milliseconds stamps ("+forward <key> <time>",
//    CL_KeyEvent 0x40ee08). With two epochs a press counted as a full frame (huge,
//    clamped to 1) and a release lost its fraction (negative, clamped to 0): keyboard
//    movement quantised to whole frames since 1.6.3. Same epoch = the fractions are
//    back, and the int cannot wrap after 24.8 days of Windows uptime either.
//
// 2. REAL time, always. Returning the theoretical deadline instead was tried (it makes
//    the ms deltas perfectly uniform) and REVERTED: this value is the clock the engine
//    uses for everything time-based - snapshot interpolation, animation blending, stance
//    transitions. A synthetic clock drifts from the network timeline by a few ms, the
//    interpolation window slides, and remote player models flicker ("on voit le joueur
//    briller", reported live on crouch spam). The deadline is what we WAIT for; it must
//    never be what we REPORT.
//
// 3. A CONTINUOUS clock (QPC), calibrated onto the engine's epoch - not timeGetTime
//    itself. Sys_Milliseconds moves in clock-interrupt steps of 0.9766 ms, so over an
//    exact 4 ms period its whole-ms delta reads 3 every dozen frames; Com_Frame then
//    calls again and the frame stretches. Measured in tools/test_frame_limiter.cpp:
//    returning timeGetTime gave 238 fps for com_maxfps 250. Floors of a continuous
//    clock stepping by exactly 4.000 ms differ by exactly 4. The calibration is checked
//    now and then against Sys_Milliseconds and re-done if the two clocks ever drift
//    apart (they should not: both count the same interrupt time, one at 100 ns, one in
//    ms); the key stamps are on the stepped clock, so they can disagree with ours by a
//    tick, never more - the same tick the stamps are quantised to anyway.
//
// LATE INPUT SAMPLING. The engine reads the mouse in IN_Frame, BEFORE Com_Frame and so
// before this wait: every count moved during the wait is only seen next frame. And the
// Windows message pump (Sys_SendKeyEvents 0x468a40: keys, mouse buttons, wheel) runs in
// CL_Frame AFTER CL_SendCmd (0x412915 vs 0x412901) - a click arriving during frame N is
// pumped after N's command is built, dispatched in N+1, in the command of N+1 at best.
// So once the wait is over we pump the queue and sample the mouse again: the second
// Com_EventLoop (0x43a6cd) hands the events to CL_MouseEvent (which ADDS to cl.mouseDx,
// 0x40bbf6) and to the +binds, Cbuf_Execute (0x43a6d2) runs them, and CL_Frame builds
// this frame's command with everything that happened up to a few microseconds ago.
// One frame less latency for clicks and keys, the wait's length less for the mouse.
// IN_MouseMove is self-contained (GetCursorPos - centre, SetCursorPos, Sys_QueEvent);
// it is called under IN_Frame's own gate (mouse initialised and active). Vanilla 1.5
// has the same ordering, so this is one place the mod beats it.
//
// After a late wake the delta can still read one short (the previous frame crossed a
// millisecond boundary late, this one did not). That second call used to wait for the
// NEXT deadline: a doubled frame (8 ms at 250 fps) as a reward for waking 0.3 ms late.
// Now it is recognised (lastTime still holds the previous frame: Com_Frame only updates
// it after the loop) and finishes the millisecond instead, re-anchoring the cadence.

#include "performance/frame_limiter.h"
#include "performance/pacing_stats.h"
#include "performance/stall_trace.h"
#include "performance/render_clock.h"
#include "performance/process_priority.h"
#include "performance/bench.h"
#include "performance/gpu_sync.h"
#include "core/logger.h"
#include "netcode/version_patch.h" // CODMP_PREFERRED_BASE

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <immintrin.h>   // _mm_pause - not pulled in transitively by older mingw <windows.h>
#include <timeapi.h>     // timeGetTime (winmm)

namespace patches {

FrameLimiterConfig g_frame_limiter_config = {
    /* enable           */ true,
    /* deadline_bias_us */ 0,
    /* late_input       */ true,
};

namespace {

LARGE_INTEGER g_qpc_freq = {};
LONGLONG      g_next_deadline = 0;   // theoretical, NOT the last wake time
LONGLONG      g_step_frac = 0;       // running remainder of freq/maxfps
int           g_last_maxfps = 0;
int           g_last_ret = INT_MIN;  // what the previous call returned (re-loop detection)
bool          g_have_ret = false;
LONGLONG      g_epoch = 0;           // the QPC count at which the engine's clock read 0
bool          g_epoch_set = false;
unsigned      g_calls = 0;
bool          g_applied = false;
// statistics of the current window (session report)
LONGLONG      g_prev_end = 0;        // QPC at the previous frame's return
LONGLONG      g_st_max_ticks = 0;
long          g_st_frames = 0, g_st_late = 0, g_st_long = 0;
volatile long g_rc_late = 0, g_rc_frames = 0;   // since the render clock's last poll
long          g_st_total = 0;        // frames since the start, never reset

int read_com_maxfps_dvar() {
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) return 0;
    uintptr_t base = (uintptr_t)exe;
    void** slot = (void**)(base + (CODMP_COM_MAXFPS_DVAR_SLOT_VA - CODMP_PREFERRED_BASE));
    void* dvar = *slot;
    if (!dvar) return 0;
    return *(int*)((char*)dvar + CODMP_DVAR_INTEGER_OFFSET);
}

// Sys_Milliseconds(), exactly as the engine inlines it (the init is idempotent; by the
// time Com_Frame reaches our call it has run its own copy a few instructions earlier).
int engine_sys_milliseconds() {
    volatile int*   initialized = (volatile int*)CODMP_SYS_TIMEINIT_VA;
    volatile DWORD* base        = (volatile DWORD*)CODMP_SYS_TIMEBASE_VA;
    if (!*initialized) { *base = timeGetTime(); *initialized = 1; }
    return (int)(timeGetTime() - *base);
}

typedef void (__cdecl* EngineVoidFn_t)();

// the wait is over: the keys and clicks that queued up meanwhile, then the mouse
void engine_late_input() {
    ((EngineVoidFn_t)CODMP_SYS_SENDKEYEVENTS_VA)();
    if (*(volatile int*)CODMP_MOUSE_INITIALIZED_VA && *(volatile int*)CODMP_MOUSE_ACTIVE_VA)
        ((EngineVoidFn_t)CODMP_IN_MOUSEMOVE_VA)();
}

const FrameLimiterEngine g_engine = {
    read_com_maxfps_dvar,
    engine_sys_milliseconds,
    (int*)CODMP_COM_LASTTIME_VA,
    engine_late_input,
};

// bookkeeping at every return to Com_Frame, then the late input sampling
void end_of_wait(const FrameLimiterEngine& e, LONGLONG now, bool reloop, LONGLONG step_ticks) {
    bench_frame_end(now);                    // the next frame's work starts now (bench.cpp)
    if (reloop) {
        ++g_st_late;
        InterlockedIncrement(&g_rc_late);
    } else {
        ++g_st_frames;
        ++g_st_total;
        InterlockedIncrement(&g_rc_frames);
        if (g_prev_end) {
            const LONGLONG period = now - g_prev_end;
            if (period > g_st_max_ticks) g_st_max_ticks = period;
            if (step_ticks > 0 && period > step_ticks + step_ticks / 2) ++g_st_long;
        }
        g_prev_end = now;
        pacing_note_frame(now);
        stall_trace_frame(now, step_ticks);   // one line per long frame, with its make-up
    }
    if (g_frame_limiter_config.late_input && e.late_input) e.late_input();
}

// our clock: QPC on the engine's epoch (rule 3)
inline int clock_ms(LONGLONG qpc) {
    return (int)((qpc - g_epoch) * 1000 / g_qpc_freq.QuadPart);
}

void calibrate(LONGLONG qpc, const FrameLimiterEngine& e) {
    const LONGLONG cand = qpc - (LONGLONG)e.engine_ms() * g_qpc_freq.QuadPart / 1000;
    if (!g_epoch_set) { g_epoch = cand; g_epoch_set = true; return; }
    // the stepped clock alone accounts for +-1 ms between two samples; 3 is drift
    const LONGLONG diff_ms = (cand - g_epoch) * 1000 / g_qpc_freq.QuadPart;
    if (diff_ms >= 3 || diff_ms <= -3) {
        g_epoch = cand;
        static int s_logged = 0;
        if (s_logged++ < 5)
            logger::logf("frame_limiter: clock re-synced to Sys_Milliseconds (%lld ms apart)", diff_ms);
    }
}

inline void spin_once(LONGLONG remaining_ticks) {
    const LONGLONG remaining_us = remaining_ticks * 1000000LL / g_qpc_freq.QuadPart;
    // Sleep(1) can overshoot well past a millisecond even at 1ms timer
    // resolution; at 250fps the whole frame is 4ms, so only sleep while
    // there is real slack and spin the tail.
    if (remaining_us > 2500) Sleep(1);
    else                     _mm_pause();
}

// HIGH-RESOLUTION WAIT (2026-10-16). Sleep(1) wakes on the next clock interrupt at best and
// often one or two later, which is why the coarse sleep above stops 2.5 ms before the
// deadline and spins the rest: up to 60 % of every 4 ms frame burnt in a spin, and a
// preempted spin is a late frame. A waitable timer created with
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+) wakes within a fraction of a
// millisecond of its due time, so the coarse wait can go to HR_SPIN_US before the deadline
// and only that tail is spun - the deadline itself is still met by the spin, to the QPC
// tick. Resolved at run time (older Windows / older headers): without it, the Sleep(1) path
// above, unchanged.
// 1.6.17: OFF by default again - the Sleep(1) wait is part of the 1.6.12 timing players asked back
// (see gpu_sync.cpp). On with the rest of the 1.6.13 timing (cod1x_gpu_fence 1, Ctrl+M > Display,
// live) or alone with COD1RELOADED_LIMITER_HR=1.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
constexpr LONGLONG HR_SPIN_US = 1000;
typedef HANDLE (WINAPI* CreateWaitableTimerExW_t)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
HANDLE g_hr_timer = NULL;
bool   g_hr_tried = false;

// the high-resolution wait wanted for this frame (the timer itself is created the first time)
bool hr_wanted() {
    static int s_env = -1;
    if (s_env < 0) {
        const char* e = getenv("COD1RELOADED_LIMITER_HR");
        s_env = (e && *e == '1') ? 1 : 0;
    }
    return s_env || gpu_sync_timing_1613();
}

void hr_timer_init() {
    if (g_hr_tried) return;
    g_hr_tried = true;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    CreateWaitableTimerExW_t create = k32 ? (CreateWaitableTimerExW_t)GetProcAddress(k32, "CreateWaitableTimerExW") : nullptr;
    if (create) g_hr_timer = create(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    logger::logf(g_hr_timer ? "frame_limiter: high-resolution waitable timer (spin the last %lld us only)"
                            : "frame_limiter: no high-resolution timer on this Windows - Sleep(1) wait%s",
                 g_hr_timer ? HR_SPIN_US : 0LL);
}

// wait until the QPC deadline; `now` is updated
void wait_until(LONGLONG deadline, LARGE_INTEGER& now) {
    const bool hr = hr_wanted();
    static int s_hr_logged = -1;
    if ((int)hr != s_hr_logged) {
        s_hr_logged = hr;
        logger::logf(hr ? "frame_limiter: high-resolution wait (the 1.6.13 timing)" : "frame_limiter: Sleep(1) wait (the 1.6.12 timing)");
    }
    if (hr) hr_timer_init();
    while (now.QuadPart < deadline) {
        const LONGLONG remaining = deadline - now.QuadPart;
        if (hr && g_hr_timer) {
            const LONGLONG remaining_us = remaining * 1000000LL / g_qpc_freq.QuadPart;
            if (remaining_us > HR_SPIN_US + 100) {
                LARGE_INTEGER due;
                due.QuadPart = -(remaining_us - HR_SPIN_US) * 10;     // relative, 100 ns units
                if (SetWaitableTimer(g_hr_timer, &due, 0, NULL, NULL, FALSE))
                    WaitForSingleObject(g_hr_timer, INFINITE);
                else
                    spin_once(remaining);
            } else {
                _mm_pause();
            }
        } else {
            spin_once(remaining);
        }
        QueryPerformanceCounter(&now);
    }
}

}  // namespace

void frame_limiter_stats(FrameLimiterStats* out, bool reset) {
    if (out) {
        if (g_qpc_freq.QuadPart == 0) QueryPerformanceFrequency(&g_qpc_freq);
        out->frames = g_st_frames;
        out->late = g_st_late;
        out->long_frames = g_st_long;
        out->max_frame_ms = g_qpc_freq.QuadPart ? (double)g_st_max_ticks * 1000.0 / (double)g_qpc_freq.QuadPart : 0.0;
    }
    if (reset) { g_st_frames = 0; g_st_late = 0; g_st_long = 0; g_st_max_ticks = 0; }
}

void frame_limiter_late_poll(long* late, long* frames) {
    *late = InterlockedExchange(&g_rc_late, 0);
    *frames = InterlockedExchange(&g_rc_frames, 0);
}

long frame_limiter_total_frames() { return g_st_total; }

void frame_limiter_reset() {
    g_prev_end = 0;
    frame_limiter_stats(nullptr, true);
    g_next_deadline = 0;
    g_step_frac = 0;
    g_last_maxfps = 0;
    g_last_ret = INT_MIN;
    g_have_ret = false;
    g_epoch_set = false;
    g_calls = 0;
}

int frame_wait(const FrameLimiterEngine& e) {
    if (g_qpc_freq.QuadPart == 0) QueryPerformanceFrequency(&g_qpc_freq);
    process_priority_mmcss_join();       // the game thread: joins / leaves MMCSS as cod1x_mmcss asks (cheap when unchanged)

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    bench_frame_begin(now.QuadPart);         // the frame's work ends here (bench.cpp: budget line, camera benchmark)
    if (!g_epoch_set || (++g_calls & 1023) == 0) calibrate(now.QuadPart, e);

    const int maxfps = e.maxfps();
    if (!(maxfps > 0 && maxfps <= 10000)) {
        g_next_deadline = 0;
        const int ret = clock_ms(now.QuadPart);
        g_last_ret = ret; g_have_ret = true;
        end_of_wait(e, now.QuadPart, false, 0);
        return ret;
    }

    // EXACT frame step: freq/maxfps rarely divides evenly, and truncating loses a
    // fraction of a tick every frame - visible as a permanent 1-2 fps shortfall.
    // Carry the remainder so the average is exactly maxfps.
    const LONGLONG step_int = g_qpc_freq.QuadPart / maxfps;
    const LONGLONG step_rem = g_qpc_freq.QuadPart % maxfps;
    const LONGLONG bias_ticks = (LONGLONG)g_frame_limiter_config.deadline_bias_us
                                * g_qpc_freq.QuadPart / 1000000LL;
    const int min_msec = 1000 / maxfps;               // Com_Frame's own integer division

    // Second call within the same Com_Frame loop (see the header): the engine read our
    // previous value against lastTime and found the whole-ms delta short. Do not spend
    // a period on it - finish the millisecond, re-anchor the cadence there.
    const bool reloop = g_have_ret && e.last_time && *e.last_time != g_last_ret;
    if (reloop) {
        const int last = *e.last_time;
        const LONGLONG give_up = now.QuadPart + step_int;   // never more than one period
        while (clock_ms(now.QuadPart) - last < min_msec && now.QuadPart < give_up) {
            _mm_pause();
            QueryPerformanceCounter(&now);
        }
        g_next_deadline = now.QuadPart + step_int + bias_ticks;
        g_step_frac = 0;
        const int ret = clock_ms(now.QuadPart);
        g_last_ret = ret;
        end_of_wait(e, now.QuadPart, true, step_int);
        return ret;
    }

    if (maxfps != g_last_maxfps || g_next_deadline == 0) {
        g_last_maxfps   = maxfps;
        g_step_frac     = 0;
        g_next_deadline = now.QuadPart + step_int + bias_ticks;
    }

    const LONGLONG deadline = g_next_deadline;
    // render clock (cod1x_render_clock): a screen frame due before this command frame is drawn
    // at its own instant, in the middle of the wait
    LONGLONG slot;
    while (render_clock_slot_before(deadline, now.QuadPart, &slot)) {
        wait_until(slot, now);
        render_clock_draw_between(now.QuadPart);
        QueryPerformanceCounter(&now);
    }
    wait_until(deadline, now);

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
    }

    const int ret = clock_ms(now.QuadPart);
    g_last_ret = ret; g_have_ret = true;
    end_of_wait(e, now.QuadPart, false, step_int);
    return ret;
}

// __cdecl no-arg, returns ms in eax. replaces fcn.00438a70 call in spin-loop.
extern "C" int __cdecl frame_wait_replacement() {
    return frame_wait(g_engine);
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
    if (exe_base != CODMP_PREFERRED_BASE) {
        // the engine clock, lastTime and the dvar slot are absolute addresses
        logger::logf("frame_limiter: CoDMP.exe not at 0x%08x (0x%08x) - patch annule",
                     (unsigned)CODMP_PREFERRED_BASE, (unsigned)exe_base);
        return false;
    }
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
        "frame_limiter: call at CoDMP+0x%lx redirige -> our wait (0x%08x), engine epoch, late input %s",
        (unsigned long)CODMP_FRAME_LIMIT_CALL_OPCODE_RVA, (unsigned)hook,
        g_frame_limiter_config.late_input ? "on" : "off");
    return true;
}

}  // namespace patches
