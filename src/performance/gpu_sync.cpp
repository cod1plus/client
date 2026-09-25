// gpu_sync.cpp - see gpu_sync.h
#include "performance/gpu_sync.h"
#include "core/iat.h"
#include "core/logger.h"

namespace patches {

GpuSyncConfig g_gpu_sync_config = {
    /* enable */ true,
};

namespace {

typedef BOOL (WINAPI* SwapBuffers_t)(HDC);
typedef void (APIENTRY* glFinish_t)(void);
SwapBuffers_t g_orig_swap = nullptr;
glFinish_t    g_glFinish  = nullptr;
bool          g_resolve_tried = false;

// the minute's statistics: time spent in glFinish
LARGE_INTEGER g_freq = {};
LONGLONG g_sum_ticks = 0, g_max_ticks = 0;
long     g_frames = 0;
DWORD    g_last_report = 0;

// opengl32.dll is loaded by the engine at video init, well after DllMain: resolve at
// the first frame, once
void resolve_glfinish() {
    g_resolve_tried = true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (gl) g_glFinish = (glFinish_t)GetProcAddress(gl, "glFinish");
    logger::logf(g_glFinish ? "gpu_sync: glFinish after every SwapBuffers (one frame in flight)"
                            : "gpu_sync: opengl32!glFinish not found - off");
}

BOOL WINAPI hk_swapbuffers(HDC dc) {
    const BOOL r = g_orig_swap ? g_orig_swap(dc) : FALSE;
    if (!g_gpu_sync_config.enable) return r;
    if (!g_resolve_tried) resolve_glfinish();
    if (!g_glFinish) return r;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    g_glFinish();
    QueryPerformanceCounter(&t1);

    const LONGLONG d = t1.QuadPart - t0.QuadPart;
    g_sum_ticks += d;
    if (d > g_max_ticks) g_max_ticks = d;
    ++g_frames;
    const DWORD now = GetTickCount();
    if (!g_last_report) g_last_report = now;
    if (now - g_last_report >= 60000 && g_frames > 0) {
        if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
        const double us = 1000000.0 / (double)g_freq.QuadPart;
        // a large average means the GPU, not the CPU, sets the frame rate - the wait is
        // then time the frame would have spent queued anyway, now spent in the open
        logger::logf("gpu_sync: glFinish avg %.0f us, max %.0f us over %ld frames",
                     (double)g_sum_ticks / g_frames * us, (double)g_max_ticks * us, g_frames);
        g_sum_ticks = 0; g_max_ticks = 0; g_frames = 0;
        g_last_report = now;
    }
    return r;
}

}  // namespace

void gpu_sync_start() {
    if (!g_gpu_sync_config.enable) {
        logger::logf("gpu_sync: disabled in config");
        return;
    }
    void* orig = iat_hook("gdi32.dll", "SwapBuffers", (void*)hk_swapbuffers);
    if (!orig) {
        logger::logf("gpu_sync: SwapBuffers import not found - off");
        return;
    }
    g_orig_swap = (SwapBuffers_t)orig;
    logger::logf("gpu_sync: SwapBuffers hooked (glFinish resolved at the first frame)");
}

}  // namespace patches
