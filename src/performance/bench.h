// bench.h - where a frame's CPU time goes, and a reproducible camera benchmark.
//
// Always on (cheap): the client's renderer table (re, copied by CL_InitRef 0x412fb6 from GetRefAPI's 0xcfb558 to
// 0x148baa0, 0x39 dwords) gets timed wrappers on RenderScene (+0x64, the front end: culling, surface lists) and
// EndFrame (+0x8c: the back end draws everything, then SwapBuffers), and the qgl pointer of glDrawElements
// (0x199c628, its single call site 0x4cdce5) a counting / timing wrapper (the driver's own CPU time per draw).
// With the limiter's frame boundaries (frame_wait), one line every 10 s:
//   frame budget: work avg / p99 / max = scene + backend (draw calls: n, indices, driver ms) + rest (game, cgame,
//   collision, sound), and how many frames went over the 250 fps budget (4.0 ms).
//
// cod1x_bench 1|2|3 (not archived): once a world scene is drawn, the camera is taken from cod1x_bench_views.txt in
// the game folder (lines "x y z yaw pitch [settle measure]") - the refdef handed to RE_RenderScene gets the view
// origin and axis, whatever the player does (team menu, spectator, dead) - each view settled then measured, a line
// per view and a summary, a CSV next to the log (cod1x_bench_<hhmmss>.csv); 2 = `quit` at the end.
// 3 = the same + `screenshotJPEG bench_<hhmmss>_vNN` at each view (Main/screenshots), for before/after pictures,
// menu (UI_REFRESH, 0x418a72) and HUD (cg_draw2D) hidden while it runs.
#ifndef COD1RELOADED_BENCH_H
#define COD1RELOADED_BENCH_H

namespace patches {

void bench_frame_begin(long long qpc);   // frame_wait entry: the frame's work ends here (main thread)
void bench_frame_end(long long qpc);     // the limiter released the next frame: its work starts (main thread)

}  // namespace patches

#endif
