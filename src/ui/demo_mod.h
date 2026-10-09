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
// to with `seta fs_game <mod>; vid_restart; demo <name>`, the engine's own mod idiom, the
// same the Demos tab used. Otherwise the engine's command runs untouched.

namespace patches {

void demo_mod_install();      // DLL start (main thread, before the first frame)

}  // namespace patches

#endif
