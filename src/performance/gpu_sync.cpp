// gpu_sync.cpp - see gpu_sync.h
//
// 10-23: a FENCE on the previous frame instead of glFinish on this one. glFinish = zero
// frames in flight: the game thread stood still until the GPU had fully finished the frame
// just queued, so every GPU hiccup (1.5-5 ms on a frame that averages 0.5 ms; 12-36 ms right
// after the window comes back) became a CPU-side stall of the same length - 54 of the 66
// long frames of enzo's 01:18 session were that wait. Now the frame just queued gets a sync
// object (GL_ARB_sync) and the thread waits for the PREVIOUS frame's: the GPU may be one
// frame behind, which absorbs a hiccup, and the queue still never grows past that one frame
// (a driver left alone queues two or three). While the GPU keeps up - the normal case - the
// previous frame is long done and the wait returns at once: the same latency as glFinish.
// COD1RELOADED_GPU_FINISH=1 forces glFinish (A/B); no GL_ARB_sync = glFinish as before.
#include "performance/gpu_sync.h"
#include "core/iat.h"
#include "core/logger.h"
#include "performance/pacing_stats.h"

#include <cstdlib>

namespace patches {

GpuSyncConfig g_gpu_sync_config = {
    /* mode */ GPU_SYNC_AUTO,
};

namespace {

typedef BOOL (WINAPI* SwapBuffers_t)(HDC);
typedef void (APIENTRY* glFinish_t)(void);
typedef void* GLsync_t;
typedef GLsync_t (APIENTRY* glFenceSync_t)(unsigned condition, unsigned flags);
typedef unsigned (APIENTRY* glClientWaitSync_t)(GLsync_t s, unsigned flags, unsigned long long timeout_ns);
typedef void (APIENTRY* glDeleteSync_t)(GLsync_t s);
typedef void* (WINAPI* wglGetProcAddress_t)(const char*);
typedef void* (WINAPI* wglGetCurrentContext_t)(void);
const unsigned GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117;
const unsigned GL_SYNC_FLUSH_COMMANDS_BIT    = 0x00000001;
const unsigned GL_WAIT_FAILED                = 0x911D;

SwapBuffers_t       g_orig_swap = nullptr;
glFinish_t          g_glFinish  = nullptr;
glFenceSync_t       g_fence = nullptr;
glClientWaitSync_t  g_wait = nullptr;
glDeleteSync_t      g_del = nullptr;
wglGetCurrentContext_t g_ctx = nullptr;
GLsync_t            g_prev_fence = nullptr;    // the previous frame's sync object
void*               g_prev_ctx = nullptr;      // the context it was made in (vid_restart = a new one)
bool          g_resolve_tried = false;
bool          g_hooked = false;
bool          g_auto_off = false;
bool          g_use_fence = false;

// the window's statistics: time spent waiting for the GPU (read by session_report.cpp)
LARGE_INTEGER g_freq = {};
LONGLONG g_sum_ticks = 0, g_max_ticks = 0;
LONGLONG g_last_ticks = 0;                 // the last frame's wait (stall_trace)
long     g_frames = 0;

// auto: the wait against the frame period, over 5 s windows
LONGLONG g_last_swap = 0;
LONGLONG g_auto_wait = 0, g_auto_period = 0, g_auto_since = 0;
int      g_bound_windows = 0;

// opengl32.dll is loaded by the engine at video init, well after DllMain: resolve at
// the first frame, once (a context is current: we are inside the engine's SwapBuffers)
void resolve_gl() {
    g_resolve_tried = true;
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) { logger::logf("gpu_sync: opengl32.dll not loaded - off"); return; }
    g_glFinish = (glFinish_t)GetProcAddress(gl, "glFinish");
    wglGetProcAddress_t gpa = (wglGetProcAddress_t)GetProcAddress(gl, "wglGetProcAddress");
    g_ctx = (wglGetCurrentContext_t)GetProcAddress(gl, "wglGetCurrentContext");
    const char* force = getenv("COD1RELOADED_GPU_FINISH");
    const bool forced = force && force[0] == '1';
    if (gpa && !forced) {
        g_fence = (glFenceSync_t)gpa("glFenceSync");
        g_wait  = (glClientWaitSync_t)gpa("glClientWaitSync");
        g_del   = (glDeleteSync_t)gpa("glDeleteSync");
        g_use_fence = g_fence && g_wait && g_del && g_ctx;
    }
    const char* mode = g_gpu_sync_config.mode == GPU_SYNC_AUTO ? "auto" : "on";
    if (g_use_fence)
        logger::logf("gpu_sync: fence on the previous frame after every SwapBuffers (one frame in flight, %s)", mode);
    else if (g_glFinish)
        logger::logf("gpu_sync: glFinish after every SwapBuffers (no frame in flight, %s%s)", mode,
                     forced ? ", forced" : "; no GL_ARB_sync");
    else
        logger::logf("gpu_sync: opengl32!glFinish not found - off");
}

// the wait itself: the previous frame's fence (this frame's is queued first), else glFinish
void gpu_wait() {
    if (g_use_fence) {
        void* ctx = g_ctx();
        if (g_prev_fence && ctx != g_prev_ctx) g_prev_fence = nullptr;   // new context: the old handle went with it
        GLsync_t mine = g_fence(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (g_prev_fence) {
            const unsigned r = g_wait(g_prev_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);   // 1 s cap
            g_del(g_prev_fence);
            g_prev_fence = nullptr;
            if (r == GL_WAIT_FAILED) {
                g_use_fence = false;
                if (mine) g_del(mine);
                logger::logf("gpu_sync: glClientWaitSync failed - glFinish from here on");
                if (g_glFinish) g_glFinish();
                return;
            }
        }
        if (!mine) {
            g_use_fence = false;
            logger::logf("gpu_sync: glFenceSync returned nothing - glFinish from here on");
            if (g_glFinish) g_glFinish();
            return;
        }
        g_prev_fence = mine;
        g_prev_ctx = ctx;
        return;
    }
    if (g_glFinish) g_glFinish();
}

BOOL WINAPI hk_swapbuffers(HDC dc) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const BOOL r = g_orig_swap ? g_orig_swap(dc) : FALSE;
    const bool wait = g_gpu_sync_config.mode != GPU_SYNC_OFF && !g_auto_off;
    if (wait && !g_resolve_tried) resolve_gl();
    if (!wait || (!g_glFinish && !g_use_fence)) {
        QueryPerformanceCounter(&t1);
        g_last_ticks = 0;
        pacing_note_present(t1.QuadPart);          // without the wait: when the swap was queued
        return r;
    }

    gpu_wait();
    QueryPerformanceCounter(&t1);
    pacing_note_present(t1.QuadPart);              // the frame is on its way to the screen

    const LONGLONG d = t1.QuadPart - t0.QuadPart;
    g_sum_ticks += d;
    g_last_ticks = d;
    if (d > g_max_ticks) g_max_ticks = d;
    ++g_frames;

    if (g_gpu_sync_config.mode == GPU_SYNC_AUTO && g_freq.QuadPart) {
        if (g_last_swap) {
            g_auto_wait   += d;
            g_auto_period += t0.QuadPart - g_last_swap;
        }
        g_last_swap = t0.QuadPart;
        if (!g_auto_since) g_auto_since = t0.QuadPart;
        if (t0.QuadPart - g_auto_since >= 5 * g_freq.QuadPart) {
            const bool bound = g_auto_period > 0 && g_auto_wait * 2 > g_auto_period;
            g_bound_windows = bound ? g_bound_windows + 1 : 0;
            if (g_bound_windows >= 2) {
                g_auto_off = true;
                logger::logf("gpu_sync: the GPU sets the frame rate (the wait = %.0f%% of the frame over 10 s) "
                             "- off for this session", 100.0 * (double)g_auto_wait / (double)g_auto_period);
            }
            g_auto_wait = 0; g_auto_period = 0; g_auto_since = t0.QuadPart;
        }
    }
    return r;
}

}  // namespace

// A large average means the GPU, not the CPU, sets the frame rate - the wait is then
// time the frame would have spent queued anyway, now spent in the open.
void gpu_sync_stats(double* avg_us, double* max_us, long* frames, bool reset) {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    const double us = g_freq.QuadPart ? 1000000.0 / (double)g_freq.QuadPart : 0.0;
    if (avg_us) *avg_us = g_frames ? (double)g_sum_ticks / g_frames * us : 0.0;
    if (max_us) *max_us = (double)g_max_ticks * us;
    if (frames) *frames = g_frames;
    if (reset) { g_sum_ticks = 0; g_max_ticks = 0; g_frames = 0; }
}

const char* gpu_sync_state() {
    if (!g_hooked || g_gpu_sync_config.mode == GPU_SYNC_OFF) return "off";
    if (g_auto_off) return "auto-off";
    return g_use_fence ? "fence" : "on";
}

void gpu_sync_start() {
    if (g_gpu_sync_config.mode == GPU_SYNC_OFF) {
        logger::logf("gpu_sync: disabled in config");
        return;
    }
    void* orig = iat_hook("gdi32.dll", "SwapBuffers", (void*)hk_swapbuffers);
    if (!orig) {
        logger::logf("gpu_sync: SwapBuffers import not found - off");
        return;
    }
    g_orig_swap = (SwapBuffers_t)orig;
    g_hooked = true;
    logger::logf("gpu_sync: SwapBuffers hooked (%s; GL resolved at the first frame)",
                 g_gpu_sync_config.mode == GPU_SYNC_AUTO ? "auto" : "on");
}

double gpu_sync_last_us() {
    return g_freq.QuadPart ? (double)g_last_ticks * 1e6 / (double)g_freq.QuadPart : 0.0;
}

}  // namespace patches
