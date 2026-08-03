// A cg_drawFPS that stops shivering.
//
// The counter is not wrong, it is under-resolved. CG_DrawFPS averages 32 frame times
// sampled from an INTEGER millisecond clock and prints 32000/sum. At a genuinely
// locked 250 fps the true sum is 128 ms, but each sample rounds to 3, 4 or 5, so the
// sum lands on 127..129 and the readout alternates 252 / 250 / 248. Widening the
// window is not an option: the 32-term sum is unrolled in the compiled code.
//
// So instead of changing how the engine computes, we change what it computes FROM.
// The frame time is measured here at QPC resolution, smoothed over about a second,
// and the 32 samples are rewritten to add up to exactly the number we want shown. The engine then draws its own string, in its own font and position - only
// the input is ours. The value stays honest: it is the real average, just not
// re-quantised to the millisecond 250 times a second.

#include "features/fps_display.h"
#include "core/logger.h"

#include <cstdio>
#include <cstring>

namespace patches {

FpsDisplayConfig g_fps_display_config;

extern "C" { void* g_cg_drawfps_original = nullptr; }

namespace {

HMODULE g_cgame = nullptr;
bool    g_installed_for = false;

LARGE_INTEGER g_qpc_freq = {};
LONGLONG      g_prev_qpc = 0;
// Averaged as a FRAME TIME, not as a rate. Averaging 1/dt is biased upward - the mean
// of the reciprocals is not the reciprocal of the mean - and a simulation of a locked
// 250 fps with realistic jitter read 253-256 that way, and 329 for a true 333.
double        g_avg_dt   = 0.0;
int           g_shown    = 0;      // exactly what the engine will print
int           g_total    = 0;      // the sum handed to it to make that happen

double g_held = 0.0;               // how long the average has been outside the band

// Tuned by simulating jittery frame pacing at 60/125/144/250/333 fps: this is the
// shortest settling time that gives ZERO changes at all of them while still landing on
// the exact figure. Loosening any of the three brings the flicker back.
constexpr double kWindow = 1.0;    // seconds of memory in the average
constexpr double kDwell  = 0.6;    // the average must stay out of band this long
constexpr double kHystK  = 1.0;    // band width, in units of the display's own step

// The engine can only print 32000/total for an integer total, so the printable values
// are not evenly spaced: 250 and 251 are adjacent, but above 300 the gaps widen fast
// (333 then 329) and past 900 they are enormous. A fixed hysteresis is therefore too
// tight at high frame rates and pointlessly wide at low ones - it has to scale with
// the local step, which is fps^2/32000.
double display_step(double fps) { return fps * fps / 32000.0; }

// The total whose PRINTED result is closest to fps. Rounding 32000/fps instead lands
// on a neighbour surprisingly often: at a true 250 it settled on 248 and stayed there.
int best_total(double fps) {
    int t0 = (int)(32000.0 / fps + 0.5);
    if (t0 < FPS_SAMPLES) t0 = FPS_SAMPLES;
    if (t0 > 32000) t0 = 32000;
    int best = t0;
    double best_err = 1e9;
    for (int t = t0 - 1; t <= t0 + 1; ++t) {
        if (t < FPS_SAMPLES || t > 32000) continue;
        const double err = (32000 / t) - fps;
        const double abs_err = err < 0 ? -err : err;
        if (abs_err < best_err) { best_err = abs_err; best = t; }
    }
    return best;
}

}  // namespace

// Runs immediately before CG_DrawFPS reads the sample array.
extern "C" void cg_drawfps_pre_run() {
    if (!g_cgame) return;

    // cg_drawFPS 2 shows min/max: that mode is meant to expose jitter, so it is left
    // strictly alone.
    const int mode = *(const int*)((uintptr_t)g_cgame + CGAME_CG_DRAWFPS_CVAR_RVA + 12);
    if (mode != 1) return;

    if (g_qpc_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&g_qpc_freq);
        if (g_qpc_freq.QuadPart == 0) return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG prev = g_prev_qpc;
    g_prev_qpc = now.QuadPart;
    if (prev == 0) return;                       // first frame: nothing to measure yet

    const double dt = (double)(now.QuadPart - prev) / (double)g_qpc_freq.QuadPart;
    if (dt <= 0.0 || dt > 1.0) return;           // alt-tab, load, breakpoint: ignore

    // Memory expressed per-frame, so the smoothing does not itself depend on the
    // frame rate.
    const double alpha = dt / (kWindow + dt);
    g_avg_dt = (g_avg_dt == 0.0) ? dt : g_avg_dt + alpha * (dt - g_avg_dt);
    const double fps = 1.0 / g_avg_dt;

    // Adopt a new figure only once the average has stayed out of band for a while. A
    // band alone is not enough: the average wanders across it now and then, and one
    // crossing was enough to make the counter tick over every few seconds.
    const double band = display_step(fps) * kHystK;
    const double h = band < 1.0 ? 1.0 : band;
    if (g_shown == 0) {
        g_held = kDwell;
    } else if (fps > g_shown + h || fps < g_shown - h) {
        g_held += dt;
    } else {
        g_held = 0.0;
    }
    if (g_held >= kDwell) {
        g_held  = 0.0;
        g_total = best_total(fps);
        g_shown = 32000 / g_total;   // compare against what is really printed
    }
    if (g_total <= 0) return;

    int* samples = (int*)((uintptr_t)g_cgame + CGAME_FPS_SAMPLES_RVA);
    const int base = g_total / FPS_SAMPLES;
    const int rem  = g_total - base * FPS_SAMPLES;
    for (int i = 0; i < FPS_SAMPLES; ++i) samples[i] = base + (i < rem ? 1 : 0);

    // The engine only starts printing once it has collected a full window; make sure
    // that condition is met so the counter appears immediately rather than after 32
    // frames of a freshly loaded map.
    int* index = (int*)((uintptr_t)g_cgame + CGAME_FPS_INDEX_RVA);
    if (*index < FPS_SAMPLES) *index = FPS_SAMPLES;
}

// 5 stolen bytes = sub esp,0x28 / push esi / push edi, replayed here before rejoining
// the original at +5. The float argument is untouched on the stack.
extern "C" __attribute__((naked))
void cg_drawfps_trampoline() {
    asm(
        "pushal\n\t"
        "call _cg_drawfps_pre_run\n\t"
        "popal\n\t"
        "subl $0x28, %esp\n\t"
        "push %esi\n\t"
        "push %edi\n\t"
        "jmp *_g_cg_drawfps_original\n\t"
    );
}

void fps_display_install(HMODULE cgame) {
    if (!cgame) return;
    g_cgame = cgame;
    if (!g_fps_display_config.enable) return;

    BYTE* fn = (BYTE*)((uintptr_t)cgame + CGAME_CG_DRAWFPS_RVA);

    // Expect the exact prologue; anything else means a different cgame build and the
    // patch is skipped rather than applied blind.
    static const BYTE kExpect[CGAME_CG_DRAWFPS_STEAL] = { 0x83, 0xec, 0x28, 0x56, 0x57 };
    if (memcmp(fn, kExpect, sizeof(kExpect)) != 0) {
        if (fn[0] == 0xe9) return;                       // already hooked (map change)
        logger::logf("fps_display: prologue CG_DrawFPS inattendu, patch ignore");
        return;
    }

    g_cg_drawfps_original = (void*)(fn + CGAME_CG_DRAWFPS_STEAL);
    g_prev_qpc = 0;
    g_avg_dt   = 0.0;
    g_held     = 0.0;
    g_total    = 0;
    g_shown    = 0;

    const intptr_t rel = (intptr_t)&cg_drawfps_trampoline - (intptr_t)(fn + 5);
    DWORD old = 0;
    if (!VirtualProtect(fn, CGAME_CG_DRAWFPS_STEAL, PAGE_EXECUTE_READWRITE, &old)) return;
    fn[0] = 0xe9;
    memcpy(fn + 1, &rel, 4);
    VirtualProtect(fn, CGAME_CG_DRAWFPS_STEAL, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, CGAME_CG_DRAWFPS_STEAL);

    if (!g_installed_for) {
        g_installed_for = true;
        logger::logf("fps_display: CG_DrawFPS hooke -> compteur lisse (cg_drawFPS 1)");
    }
}

}  // namespace patches
