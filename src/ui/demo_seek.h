#ifndef COD1RELOADED_DEMO_SEEK_H
#define COD1RELOADED_DEMO_SEEK_H

// demo_seek - play a demo from a given moment.
//
// A Quake 3 demo cannot jump: every snapshot is a delta of the one before. So the demo
// is played from the start and fast-forwarded: `timescale` (Com_ModifyMsec 0x43a310
// multiplies the frame time by it; with no local server a frame may cover up to 5 s)
// is driven every frame so that each frame covers about half of what is left, which
// lands on the target without overshooting, then drops back to 1. The sound is cut
// meanwhile (mss_volume, restored at the end, on abort and at exit; a marker file
// restores it at the next launch if the game died in between) and a curtain hides the
// fast-forward.
//
// Engine state read (CoDMP.exe 1.5, RE'd 2026-09-26):
//   clc.demoplaying      0x18ba6cc  set by CL_PlayDemo_f (0x40f810)
//   clc.demoName         0x18ba688  the `demo` command's argument
//   cl.snap.valid        0x148bba4  cl.snap = the last parsed snapshot (CL_ParseSnapshot
//   cl.snap.serverTime   0x148bbac  copies it at 0x417630); zeroed by a new gamestate

#include <cstdint>
#include <string>

namespace patches {

constexpr uintptr_t CODMP_CLC_DEMOPLAYING_VA = 0x018ba6cc;
constexpr uintptr_t CODMP_CLC_DEMONAME_VA    = 0x018ba688;
constexpr uintptr_t CODMP_CL_SNAP_VALID_VA   = 0x0148bba4;
constexpr uintptr_t CODMP_CL_SNAP_TIME_VA    = 0x0148bbac;

// Starts `demo <name>` (name without extension; dir = the demo's search-path folder,
// switched to with fs_game + vid_restart first when it is not the active one) and,
// when target_server_time > 0, fast-forwards to it. label = what the curtain says.
void demo_seek_play(const std::string& dir, const std::string& name_noext,
                    int target_server_time, int first_server_time, const std::string& label);

void demo_seek_frame();                 // every frame, main thread (SwapBuffers hook)
bool demo_seek_curtain_visible();
void demo_seek_draw_curtain(float w, float h);
void demo_seek_restore_now();           // exit path: timescale 1, sound back
void demo_seek_startup_recover();       // DLL start: a marker left by a crash mid-seek

}  // namespace patches

#endif
