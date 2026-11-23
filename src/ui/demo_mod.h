#ifndef COD1RELOADED_DEMO_MOD_H
#define COD1RELOADED_DEMO_MOD_H
// demo_mod - `demo <name>` plays in the mod the demo needs, whatever started it (the game's
// own demo menu, the console, the 1.6X Demos tab).
//
// The engine never switches mod for a demo: CL_SystemInfoChanged (0x4176f0) skips the whole
// systeminfo while clc.demoplaying, fs_game included. A demo recorded on a rPAM server
// started from Main therefore ends on "Couldn't load maps/mp/mp_hanoi.bsp": the map pack is
// in __rPAMv115b5. CL_PlayDemo_f (0x40f810, registered at 0x413bc7) is entered first here:
// when the demo's first map is not reachable in the active mod, the demo's own fs_game
// (its recorded systeminfo) - or else the mod folder whose pk3s hold that map - is switched
// to first (demo_play_in). Otherwise the engine's command runs untouched.
//
// The engine also skips the pure pk3 list for a demo: every pk3 of the install was searched,
// also those the recorded server never allowed (rPAM's own "LEANFIX" playeranim.script over the
// server's -> players animated wrongly). The recorded server's sv_paks is now applied for the
// demo, as when connected, when every pak it used is here with the same checksum; removed when
// playback ends (demo_pure_release).

#include <string>

namespace patches {

void demo_mod_install();      // DLL start (main thread, before the first frame)

// Plays demos/<name> of game folder <dir> ("Main" or a mod). When <dir> is not the active
// mod, fs_game is force-set (the console's `seta fs_game` is refused: write protected) and
// vid_restart restarts the file system before `demo <name>`. Main thread.
void demo_play_in(const std::string& dir, const std::string& name);

// Runs <cmds> (console lines, "\n" ended) in game folder <dir> ("Main" or a mod), the same
// way: a different folder is switched to first. rescan = restart the file system even when
// <dir> is already the active mod, for a pk3 placed while the game runs (the Workshop's PLAY
// right after a download). Main thread.
void play_in_mod(const std::string& dir, const std::string& cmds, bool rescan);

// Playback over: the recorded server's pure list (if one was applied) is removed, so the menu
// and the next game search every pk3 again. Main thread.
void demo_pure_release();

}  // namespace patches

#endif
