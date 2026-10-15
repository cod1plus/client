#ifndef COD1RELOADED_PACING_STATS_H
#define COD1RELOADED_PACING_STATS_H
// How REGULAR the frames are, not just how many: the intervals between frame starts (the
// limiter's wake-ups, which set the game time each frame shows) and between presents (the
// end of SwapBuffers, plus glFinish when gpu_sync waits - what a VRR screen displays at).
// A G-Sync / FreeSync screen shows every frame the moment it is presented, so it shows
// every irregularity too: the spread of these intervals is the smoothness, the average is
// only the frame rate. session_report.cpp prints both every 60 s.
#include <windows.h>

namespace patches {

struct PacingStats {
    long   n;           // intervals in the window
    double mean_ms;
    double sd_ms;       // standard deviation: the jitter
    double p99_ms;      // 99th percentile
    double max_ms;
    long   over;        // intervals more than 25 % over the mean
};

void pacing_note_frame(LONGLONG qpc);     // the limiter released a frame (frame_limiter.cpp)
void pacing_note_present(LONGLONG qpc);   // SwapBuffers (+ glFinish) returned (gpu_sync.cpp)
void pacing_stats(PacingStats* frames, PacingStats* presents, bool reset);

}  // namespace patches

#endif
