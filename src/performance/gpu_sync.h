#ifndef COD1RELOADED_GPU_SYNC_H
#define COD1RELOADED_GPU_SYNC_H
// One frame in flight, never two. The OpenGL driver is free to queue a frame or two of
// commands behind SwapBuffers when the CPU runs ahead of the GPU; the frame that holds
// this frame's mouse movement then reaches the screen one or two presents later than
// the game thinks. glFinish() right after the real SwapBuffers blocks until the GPU has
// presented, so the next input sample is taken against what is really on screen. The
// same idea as the engine's own `r_finish 1` (glFinish at the start of the backend), but
// after the presentation, and measured: the wait goes into the session report.
//
// Free when the GPU idles (250 fps cap on anything modern). On a machine where the GPU
// sets the frame rate the wait IS the frame, and blocking on it costs fps: in `auto`
// the wait is compared with the frame period, and when it fills more than half of it
// for ten seconds the sync switches itself off for the session (logged).
#include <windows.h>

namespace patches {

enum GpuSyncMode { GPU_SYNC_OFF = 0, GPU_SYNC_ON = 1, GPU_SYNC_AUTO = 2 };

struct GpuSyncConfig {
    int mode;      // GpuSyncMode; ini gpu_sync = off / on / auto
};

extern GpuSyncConfig g_gpu_sync_config;

void gpu_sync_start();   // DllMain: IAT-hook gdi32!SwapBuffers in CoDMP.exe
// time spent in glFinish over the current window, microseconds (session_report.cpp)
void gpu_sync_stats(double* avg_us, double* max_us, long* frames, bool reset);
// "on", "off", or "auto-off" (switched itself off: the GPU sets the fps)
const char* gpu_sync_state();

}  // namespace patches

#endif
