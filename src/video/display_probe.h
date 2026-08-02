#ifndef COD1RELOADED_DISPLAY_PROBE_H
#define COD1RELOADED_DISPLAY_PROBE_H

#include <windows.h>

namespace patches {

// Q3-lineage r_mode table (CoD1 default r_mode 3 = 640x480). false = mode out of table.
bool resolution_for_r_mode(int mode, int* w, int* h);

// The resolution the engine will create its backbuffer at, read from the player's
// Main/config_mp.cfg. DllMain runs before Com_Init, so no cvar exists yet and the
// config file is the only source available this early. false = the config says
// nothing about it (fresh install), in which case we must not assume anything.
bool probe_config_resolution(int* w, int* h);

// Current desktop mode of the primary display.
bool probe_desktop_resolution(int* w, int* h);

// Turns windowed/borderless back off for a player whose game resolution the desktop
// cannot present as-is. Call from DllMain before apply_fullscreen_patch() and
// start_window_watcher().
void display_mode_guard();

}  // namespace patches

#endif
