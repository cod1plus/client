// stall_trace.cpp - see stall_trace.h
#include "performance/stall_trace.h"
#include "performance/gpu_sync.h"
#include "performance/smodel_fast.h"
#include "netcode/net_diag.h"
#include "video/window_patch.h"
#include "ui/gl_overlay.h"
#include "core/logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace patches {

namespace {

bool      g_init = false;
bool      g_on = true;
LARGE_INTEGER g_freq;
LONGLONG  g_prev = 0;                 // previous boundary
ULONG64   g_prev_cycles = 0;          // thread cycles at it
double    g_cycles_per_ms = 0;        // calibrated once: the thread running flat out
long      g_rx_prev = 0;
int       g_lines_min = 0;            // rate limit
DWORD     g_min_since = 0;
LONGLONG  g_focus_change = 0;         // last time iconic / foreground changed
bool      g_was_bg = false;

void calibrate() {
    // 3 ms of pure spinning on this thread: cycles per ms when it really runs
    LARGE_INTEGER t0, t1;
    ULONG64 c0 = 0, c1 = 0;
    QueryPerformanceCounter(&t0);
    QueryThreadCycleTime(GetCurrentThread(), &c0);
    const LONGLONG stop = t0.QuadPart + g_freq.QuadPart * 3 / 1000;
    volatile unsigned sink = 0;
    do { sink += 1; QueryPerformanceCounter(&t1); } while (t1.QuadPart < stop);
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / g_freq.QuadPart;
    g_cycles_per_ms = ms > 0 ? (c1 - c0) / ms : 0;
    logger::logf("stall_trace: on - a line per frame over 4x the cap period (thread %.0f Mcycles/ms when running)",
                 g_cycles_per_ms / 1e6 * 1000.0 / 1000.0);
}

}  // namespace

void stall_trace_frame(LONGLONG now, LONGLONG period) {
    if (!g_init) {
        g_init = true;
        const char* e = getenv("COD1RELOADED_STALL_TRACE");
        g_on = !(e && e[0] == '0');
        QueryPerformanceFrequency(&g_freq);
        if (g_on) calibrate();
        QueryThreadCycleTime(GetCurrentThread(), &g_prev_cycles);
        g_prev = now;
        g_rx_prev = net_diag_rx_total();
        return;
    }
    if (!g_on) return;
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    const LONGLONG dt = now - g_prev;
    const ULONG64 dc = cycles - g_prev_cycles;
    g_prev = now; g_prev_cycles = cycles;

    // window state, tracked every frame so a stall can say how long ago the focus moved
    HWND w = get_game_window();
    const bool iconic = w && IsIconic(w);
    const bool fg = w && GetForegroundWindow() == w;
    const bool bg = iconic || !fg;
    if (bg != g_was_bg) { g_was_bg = bg; g_focus_change = now; }

    // the per-frame context is consumed every frame (so a stall line reads this frame's)
    const double finish_ms = gpu_sync_last_us() / 1000.0;
    long sc_calls = 0; double sc_ms = 0;
    smodel_fast_poll_frame(&sc_calls, &sc_ms);
    const long rx_now = net_diag_rx_total();
    const long rx = rx_now - g_rx_prev;
    g_rx_prev = rx_now;

    if (period <= 0 || dt < period * 4) return;
    const double ms = dt * 1000.0 / g_freq.QuadPart;
    if (ms < 12.0) return;

    const DWORD tick = GetTickCount();
    if (tick - g_min_since > 60000) { g_min_since = tick; g_lines_min = 0; }
    if (++g_lines_min > 30) return;

    const double busy = g_cycles_per_ms > 0 ? dc / g_cycles_per_ms : -1;
    const double since_focus = g_focus_change ? (now - g_focus_change) * 1000.0 / g_freq.QuadPart : -1;
    char focus[64];
    if (iconic) snprintf(focus, sizeof(focus), "ICONIC");
    else if (!fg) snprintf(focus, sizeof(focus), "BACKGROUND");
    else if (since_focus >= 0 && since_focus < 3000) snprintf(focus, sizeof(focus), "focus back %.0f ms ago", since_focus);
    else snprintf(focus, sizeof(focus), "in front");
    logger::logf("stall: %.1f ms | thread ran ~%.1f ms of it%s | glFinish %.2f ms | scene %ld replacements %.2f ms | "
                 "%ld pq recus | %s%s",
                 ms, busy, busy >= 0 && busy < ms * 0.7 ? " (PREEMPTED / WAITING)" : "",
                 finish_ms, sc_calls, sc_ms, rx, focus, overlay_visible() ? " | overlay open" : "");
}

}  // namespace patches
