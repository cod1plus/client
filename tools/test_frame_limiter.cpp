// test_frame_limiter.cpp - runs the REAL wait (performance/frame_limiter.cpp) inside a copy
// of Com_Frame's spin loop, against a fake engine clock, and checks the two rules the wait
// has to obey (see frame_limiter.cpp): it reports the ENGINE's clock, and a frame is never
// doubled when the engine's whole-ms delta reads one short after a late wake. The wait that
// shipped from 1.6.3 to 1.6.8 is reproduced here as `old_wait` for the before / after.
//
//   g++ -std=c++17 -O2 -I src tools/test_frame_limiter.cpp src/performance/frame_limiter.cpp
//       src/core/logger.cpp -lwinmm -o build/test_frame_limiter && build/test_frame_limiter
#include "performance/frame_limiter.h"

#include <windows.h>
#include <timeapi.h>
#include <immintrin.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace patches;

// ---- the fake engine ----
static int   g_maxfps = 250;
static DWORD g_base = 0;                       // sys_timeBase
static int   g_last_time = 0;                  // Com_Frame's lastTime
static int fake_maxfps() { return g_maxfps; }
static int fake_engine_ms() { return (int)(timeGetTime() - g_base); }
static const FrameLimiterEngine g_fake = { fake_maxfps, fake_engine_ms, &g_last_time };

static int new_wait() { return frame_wait(g_fake); }

// ---- the wait of 1.6.3 .. 1.6.8, verbatim in behaviour: QPC ms since boot, and a second
// ---- call in the same Com_Frame loop waits for the NEXT deadline
static LARGE_INTEGER o_freq = {0};
static LONGLONG o_next = 0, o_frac = 0;
static int o_last_maxfps = 0;
static int old_wait() {
    if (!o_freq.QuadPart) QueryPerformanceFrequency(&o_freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int maxfps = g_maxfps;
    if (maxfps > 0 && maxfps <= 10000) {
        const LONGLONG step_int = o_freq.QuadPart / maxfps, step_rem = o_freq.QuadPart % maxfps;
        if (maxfps != o_last_maxfps || o_next == 0) { o_last_maxfps = maxfps; o_frac = 0; o_next = now.QuadPart + step_int; }
        const LONGLONG deadline = o_next;
        while (now.QuadPart < deadline) {
            const LONGLONG rem_us = (deadline - now.QuadPart) * 1000000LL / o_freq.QuadPart;
            if (rem_us > 2500) Sleep(1); else _mm_pause();
            QueryPerformanceCounter(&now);
        }
        LONGLONG step = step_int;
        o_frac += step_rem;
        if (o_frac >= maxfps) { o_frac -= maxfps; step += 1; }
        o_next += step;
        if (now.QuadPart > o_next + 4 * step_int) { o_next = now.QuadPart + step_int; o_frac = 0; }
    } else {
        o_next = 0;
    }
    return (int)((now.QuadPart * 1000) / o_freq.QuadPart);
}

static void old_reset() { o_next = 0; o_frac = 0; o_last_maxfps = 0; }

// ---- the harness ----
static LARGE_INTEGER g_freq;
static double qpc_ms() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / g_freq.QuadPart; }
static void busy_ms(double ms) { const double end = qpc_ms() + ms; while (qpc_ms() < end) _mm_pause(); }

struct Stats {
    int frames = 0, reloops = 0, doubled = 0, short_frames = 0;
    double period_sum = 0, period_max = 0;
    long epoch_err_max = 0;            // |returned - Sys_Milliseconds| at the return
};

// Com_Frame, the part that matters:
//     do { com_frameTime = WAIT(); if (lastTime > com_frameTime) lastTime = com_frameTime;
//          msec = com_frameTime - lastTime; } while (msec < minMsec);
//     lastTime = com_frameTime;  ... the frame's work ...
static Stats run(int (*wait)(), int maxfps, int frames, double stall_ms, int stall_every) {
    Stats s;
    g_maxfps = maxfps;
    const int min_msec = 1000 / maxfps;
    const double nominal = 1000.0 / maxfps;
    g_last_time = fake_engine_ms();
    double prev_end = 0;
    for (int f = 0; f < frames; ++f) {
        int t = 0, msec = 0, calls = 0;
        do {
            t = wait();
            const long err = labs((long)t - (long)fake_engine_ms());
            if (err > s.epoch_err_max) s.epoch_err_max = err;
            if (g_last_time > t) g_last_time = t;
            msec = t - g_last_time;
            ++calls;
        } while (msec < min_msec);
        g_last_time = t;
        const double end = qpc_ms();
        if (f >= 5) {                                       // let the cadence settle
            const double period = end - prev_end;
            ++s.frames;
            s.period_sum += period;
            if (period > s.period_max) s.period_max = period;
            if (period > 1.6 * nominal) ++s.doubled;
            if (period < 0.5 * nominal) ++s.short_frames;
            if (calls > 1) ++s.reloops;
        }
        prev_end = end;
        // the frame's work: a stall a little longer than the period, so the next wait is
        // entered late and the engine's whole-ms delta may read one short
        if (stall_every && f % stall_every == stall_every - 1) busy_ms(stall_ms);
    }
    return s;
}

static int g_failed = 0;
static void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "OK" : "FAIL", what);
    if (!ok) ++g_failed;
}

int main() {
    QueryPerformanceFrequency(&g_freq);
    timeBeginPeriod(1);                                   // what fps_cap_init does in the game
    // Windows 11 ignores a background / windowless process's timer resolution request
    // (this test has no window): opt out like process_priority.cpp does for the game.
    {
        struct { ULONG Version, ControlMask, StateMask; } st = { 1, 0x1 | 0x4, 0 };
        typedef BOOL (WINAPI *SetProcessInformation_t)(HANDLE, int, LPVOID, DWORD);
        SetProcessInformation_t spi = (SetProcessInformation_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
        if (spi) spi(GetCurrentProcess(), 4, &st, sizeof(st));
        typedef LONG (WINAPI *NtQueryTimerResolution_t)(ULONG*, ULONG*, ULONG*);
        NtQueryTimerResolution_t q = (NtQueryTimerResolution_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution");
        ULONG mn = 0, mx = 0, cur = 0;
        if (q) { q(&mn, &mx, &cur); printf("timer resolution: %.4f ms (min %.4f, max %.4f)\n", cur / 10000.0, mn / 10000.0, mx / 10000.0); }
    }
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    g_base = timeGetTime() - 12345;                       // a game "launched" 12.3 s ago

    struct Case { int maxfps; double stall; int every; const char* name; };
    const Case cases[] = {
        { 250, 0.0, 0,  "250 fps, no stall" },
        { 250, 4.4, 20, "250 fps, a 4.4 ms frame every 20" },
        { 250, 4.8, 20, "250 fps, a 4.8 ms frame every 20" },
        { 125, 0.0, 0,  "125 fps, no stall" },
        { 125, 8.6, 20, "125 fps, an 8.6 ms frame every 20" },
        { 333, 3.3, 20, "333 fps, a 3.3 ms frame every 20 (odd step)" },
    };
    const int N = 1500;
    printf("%-44s %-5s %9s %9s %8s %8s %8s\n", "case", "wait", "avg ms", "max ms", "reloops", "doubled", "epoch");
    for (const Case& c : cases) {
        old_reset();
        const Stats o = run(old_wait, c.maxfps, N, c.stall, c.every);
        frame_limiter_reset();
        const Stats n = run(new_wait, c.maxfps, N, c.stall, c.every);
        const double nominal = 1000.0 / c.maxfps;
        printf("%-44s %-5s %9.4f %9.3f %8d %8d %8ld\n", c.name, "old", o.period_sum / o.frames, o.period_max, o.reloops, o.doubled, o.epoch_err_max);
        printf("%-44s %-5s %9.4f %9.3f %8d %8d %8ld\n", "", "new", n.period_sum / n.frames, n.period_max, n.reloops, n.doubled, n.epoch_err_max);
        char what[160];
        // Sys_Milliseconds moves in 0.9766 ms steps (the clock interrupt), the wait's clock
        // is continuous: they can disagree by a tick, never more
        snprintf(what, sizeof(what), "%s: new reports the engine clock (max error %ld ms <= 2)", c.name, n.epoch_err_max);
        check(n.epoch_err_max <= 2, what);
        snprintf(what, sizeof(what), "%s: new never doubles a frame (%d)", c.name, n.doubled);
        check(n.doubled == 0, what);
        snprintf(what, sizeof(what), "%s: new average period %.4f ms within 1%% of %.4f", c.name, n.period_sum / n.frames, nominal);
        check(fabs(n.period_sum / n.frames - nominal) < nominal * 0.01, what);
        if (!c.every) {
            snprintf(what, sizeof(what), "%s: new never re-loops on an on-time frame (%d)", c.name, n.reloops);
            check(n.reloops == 0, what);
        }
    }

    // no cap: the wait returns at once, still on the engine clock
    frame_limiter_reset();
    g_maxfps = 0;
    const double t0 = qpc_ms();
    const int t = new_wait();
    check(qpc_ms() - t0 < 1.0 && labs((long)t - (long)fake_engine_ms()) <= 2, "com_maxfps 0: returns at once, engine clock");

    // a cap change mid-run re-anchors the cadence
    frame_limiter_reset();
    run(new_wait, 250, 200, 0, 0);
    const Stats n2 = run(new_wait, 125, 400, 0, 0);
    char what[160];
    snprintf(what, sizeof(what), "250 -> 125 mid-run: average %.4f ms, %d doubled", n2.period_sum / n2.frames, n2.doubled);
    check(fabs(n2.period_sum / n2.frames - 8.0) < 0.08 && n2.doubled == 0, what);

    timeEndPeriod(1);
    printf("%s\n", g_failed ? "FAILED" : "ALL OK");
    return g_failed ? 1 : 0;
}
