// stall_trace.h - one log line per long frame, with what the frame was made of.
//
// The bilan says how many frames were long in a minute; this says WHY each one was, as it
// happens: how much of the frame the game thread actually ran (QueryThreadCycleTime - the
// rest it was preempted or waiting), the glFinish wait of that frame (GPU), the scene
// overflow work (smodel_fast), the packets that arrived, and whether the window was
// iconic / in the background / under the Ctrl+M overlay. Rate-limited to 30 lines a
// minute; "stall:" lines in cod1reloaded.log. COD1RELOADED_STALL_TRACE=0 turns it off.
#ifndef COD1RELOADED_STALL_TRACE_H
#define COD1RELOADED_STALL_TRACE_H

#include <windows.h>

namespace patches {

// at every frame boundary of the limiter (end_of_wait, not a reloop): the frame that just
// ended ran from the previous call to `now`; `period_ticks` = one frame at the cap
void stall_trace_frame(LONGLONG now_qpc, LONGLONG period_ticks);

}  // namespace patches

#endif
