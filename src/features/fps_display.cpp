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
#include "performance/frame_limiter.h"   // com_maxfps: the rate WE impose
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

constexpr double kWindow   = 1.0;   // seconds of memory in the average
constexpr double kDwell    = 0.6;   // the average must stay out of band this long
constexpr double kDeadZone = 0.02;  // 2% band, so noise never moves the figure
constexpr double kSnap     = 0.02;  // how close to the cap counts as "at the cap"

// The rate is not an unknown to be estimated: the mod's own frame limiter imposes it.
// Reading com_maxfps turns the hard half of the problem into a lookup.
int capped_fps() {
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return 0;
    void** slot = (void**)CODMP_COM_MAXFPS_DVAR_SLOT_VA;
    if (!slot || !*slot) return 0;
    const int v = *(int*)((char*)*slot + CODMP_DVAR_INTEGER_OFFSET);
    return (v > 0 && v <= 1000) ? v : 0;
}

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

    // A hitch is not the cruising rate. Left in the average, the occasional 10 ms
    // frame drags the MEAN FRAME TIME up and so the figure down - which is why the
    // counter sat on 248 at a locked 250 rather than on the value it was holding.
    if (g_avg_dt > 0.0 && dt > 2.0 * g_avg_dt) return;

    // Memory expressed per-frame, so the smoothing does not itself depend on the
    // frame rate.
    const double alpha = dt / (kWindow + dt);
    g_avg_dt = (g_avg_dt == 0.0) ? dt : g_avg_dt + alpha * (dt - g_avg_dt);
    const double fps = 1.0 / g_avg_dt;

    // The decisive simplification. Estimating the rate to the nearest unit and hoping
    // it holds still was the wrong problem: the rate is IMPOSED by this mod's own
    // limiter. Within 2% of com_maxfps the figure is the cap, exactly and immovably -
    // that residual is measurement quantisation, not frames being missed. Below that,
    // the smoothed measurement is shown, because a real drop must be visible.
    const int cap = capped_fps();
    double target = fps;
    if (cap > 0 && fps > cap * (1.0 - kSnap) && fps < cap * (1.0 + kSnap))
        target = cap;

    const double h = target * kDeadZone < 1.0 ? 1.0 : target * kDeadZone;
    if (g_shown == 0) {
        g_held = kDwell;
    } else if (target > g_shown + h || target < g_shown - h) {
        g_held += dt;
    } else {
        g_held = 0.0;
    }
    if (g_held >= kDwell) {
        g_held  = 0.0;
        g_total = best_total(target);
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
