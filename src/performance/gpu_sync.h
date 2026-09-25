#ifndef COD1RELOADED_GPU_SYNC_H
#define COD1RELOADED_GPU_SYNC_H
// One frame in flight, never two. The OpenGL driver is free to queue a frame or two of
// commands behind SwapBuffers when the CPU runs ahead of the GPU; the frame that holds
// this frame's mouse movement then reaches the screen one or two presents later than
// the game thinks. glFinish() right after the real SwapBuffers blocks until the GPU has
// presented, so the next input sample is taken against what is really on screen. The
// same idea as the engine's own `r_finish 1` (glFinish at the start of the backend), but
// after the presentation, and measured: the time spent waiting is written to the log
// once a minute, so a GPU-bound machine shows up as a number rather than a feeling.
// Free when the GPU idles (250 fps cap on anything modern); on a GPU-bound machine it
// trades a few fps for latency. `gpu_sync = off` in cod1reloaded.ini turns it off.
#include <windows.h>

namespace patches {

struct GpuSyncConfig {
    bool enable;
};

extern GpuSyncConfig g_gpu_sync_config;

void gpu_sync_start();   // DllMain: IAT-hook gdi32!SwapBuffers in CoDMP.exe

}  // namespace patches

#endif
