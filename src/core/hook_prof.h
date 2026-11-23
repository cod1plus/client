// hook_prof.h - how much of each frame the DLL's own hooks take, per hook.
//
// Every hook that runs on the game thread wraps its body in PROF_SCOPE(slot): two QPC reads
// (~30 ns each) and an add. The frame limiter polls the sums at every frame boundary
// (stall_trace prints them on a long frame), the session report prints the window's average
// and worst frame. The point: when a frame is long, know at once whether WE were in it.
#ifndef COD1RELOADED_HOOK_PROF_H
#define COD1RELOADED_HOOK_PROF_H

#include <windows.h>

namespace patches {

enum ProfSlot {
    PROF_BONE = 0,      // bone_probe: every skeleton the engine builds (+ hitbox_client, inspect idle compare)
    PROF_LEAN,          // apply_lean_adjust: every posed player
    PROF_INSPECT,       // inspect_apply: the view weapon, once a frame
    PROF_OVERLAY,       // gl_overlay's SwapBuffers hook before the real swap (menu, news card, polls)
    PROF_RCDRAW,        // render clock: the extra draws between two commands (engine time we asked for)
    PROF_COUNT
};

extern LONGLONG g_prof_ticks[PROF_COUNT];    // this frame, game thread only
extern long     g_prof_calls[PROF_COUNT];

struct ProfScope {
    int slot; LARGE_INTEGER t0;
    explicit ProfScope(int s) : slot(s) { QueryPerformanceCounter(&t0); }
    ~ProfScope() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); g_prof_ticks[slot] += t1.QuadPart - t0.QuadPart; g_prof_calls[slot]++; }
};
#define PROF_SCOPE(slot) ::patches::ProfScope prof_scope_##slot(::patches::slot)

// frame boundary: the frame's sums (ms per slot, calls per slot) and the window bookkeeping
struct ProfFrame { double ms[PROF_COUNT]; long calls[PROF_COUNT]; double total_ms; };
void prof_frame_poll(ProfFrame* out);                      // and zero the counters
const char* prof_slot_name(int slot);
// the window: average DLL ms per frame, the worst frame and which slot led it
void prof_window_stats(double* avg_ms, double* max_ms, int* max_slot, double* slot_avg_ms, bool reset);

}  // namespace patches

#endif
