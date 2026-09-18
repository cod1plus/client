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

// The .ini says `fullscreen = on` (or off) but Main/config_mp.cfg carries a
// `seta r_fullscreen` that disagrees: the config wins in the engine (archived cvar
// beats the default we redirect), so the value is rewritten IN PLACE before the engine
// reads the file. Only when the key is spelled out in the .ini; a config without the
// cvar is left alone (the redirected default already covers it).
void enforce_ini_fullscreen();

// refresh_rate = max but the configured resolution is not listed at the display's
// maximum Hz (custom resolutions carry only the rates the driver created them with):
// rewrite config_mp.cfg to the resolution that has the maximum, and emulate a 4:3
// stretch with view_mode = stretched. DllMain, after enforce_ini_fullscreen().
void enforce_max_hz_native();

}  // namespace patches

#endif
