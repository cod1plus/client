#ifndef COD1RELOADED_GAMMA_FIX_H
#define COD1RELOADED_GAMMA_FIX_H

// Per-monitor hardware gamma (cod2x window.cpp port) — fixes the dual-screen
// light/graphics bugs: the engine applies its SetDeviceGammaRamp to a window DC,
// which on multi-monitor setups lands on the wrong display (game washed out /
// too dark, desktop gamma leaking to the second screen, ramp lost on alt-tab).
//
// Mechanism: IAT-hook gdi32!SetDeviceGammaRamp in CoDMP.exe. The engine's ramp
// (computed from r_gamma + r_overBrightBits — kept intact) is captured and applied
// to the monitor the game window is REALLY on (CreateDC by device name), with the
// original desktop ramp saved. A watcher tick re-applies on focus/monitor changes
// and restores the desktop ramp when the game loses focus or exits.

namespace patches {

struct GammaFixConfig {
    bool enable = true;   // ini: gamma_fix_enable
};

extern GammaFixConfig g_gamma_fix_config;

void gamma_fix_start();      // DllMain: install the IAT hook
void gamma_fix_tick();       // watcher thread: focus / monitor transition handling
void gamma_fix_shutdown();   // DllMain DETACH: restore the desktop ramp

}  // namespace patches

#endif
