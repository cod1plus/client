#ifndef COD1RELOADED_SESSION_REPORT_H
#define COD1RELOADED_SESSION_REPORT_H
// One line a minute, and one at the end, that says how the game actually ran:
//     bilan 60 s: 250.0 fps (15000 frames, 0 en retard, 0 longues, max 4.3 ms) | GPU 0.5 ms
//     (max 1.4) | 1920x1080 @ 320 Hz exclusif | com_maxfps 250 | rinput on
// A player who says "it lags" pastes that line; the numbers come from the frame limiter
// (cadence), gpu_sync (GPU wait), mode_guard (display mode) and the engine's cvars.
namespace patches {

void session_report_tick();                 // watcher thread, cheap: writes every 60 s
void session_report_final(const char* why); // once: window closing / DLL detach

}  // namespace patches

#endif
