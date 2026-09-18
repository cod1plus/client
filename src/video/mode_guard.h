#ifndef COD1RELOADED_MODE_GUARD_H
#define COD1RELOADED_MODE_GUARD_H

// The AMD alt-tab killer (meta, 2026-08-25: minimize -> two windows in the
// taskbar preview, one letterboxed corpse + one white, keyboard dead on return).
//
// Chain of events, reconstructed: leaving exclusive fullscreen parks the desktop
// mode; on restore the engine calls ChangeDisplaySettingsA again with the SAME
// stored DEVMODE - including a forced dmDisplayFrequency (r_displayrefresh) and
// r_colorbits. Recent AMD drivers reject some of those exact-match modes
// (DISP_CHANGE_BADMODE) that they accepted at launch, the engine treats the
// failure as fatal, pops its white log console (the "all white" window), and
// relaunches itself (`monkey`) - the second CoDMP. The community workaround
// "r_displayrefresh 0" works because it removes the field the driver chokes on.
//
// This guard applies that remedy automatically and only when needed: every
// ChangeDisplaySettingsA of the engine is logged (mode, fields, result), and a
// failed call is retried with the forced refresh stripped, then with the color
// depth stripped too. A retry that succeeds is reported to the engine as
// success - 144Hz players keep their refresh as long as their driver honours
// it, and nobody dies on the restore path.
namespace patches {

void mode_guard_start();

// True while the engine HOLDS an exclusive-fullscreen mode: set when a
// CDS_FULLSCREEN succeeds, cleared only when the ENGINE restores the desktop
// itself (vid_restart to windowed / quit). The OS auto-restoring the mode on
// minimize does NOT clear it - the game is still a fullscreen game while
// minimized. The borderless watcher keys off this: fighting an exclusive
// engine is the two-windows/dead-keyboard bug (meta 2026-08-25), and it must
// come back when the player switches to windowed via the menu mid-session.
bool mode_guard_exclusive_active();

// Restore-from-minimize repair. When a CDS_FULLSCREEN window minimizes, Windows
// restores the desktop mode automatically - but on restore it does NOT reliably
// re-apply the game's mode (meta's log 16:21:59: window back, desktop still
// 2560x1440, backbuffer 1280x1024 = tiny letterboxed game). reapply: if the
// current display mode differs from the last fullscreen mode the engine
// successfully set, set it again; returns true only when it actually switched.
// The remembered mode is dropped when the ENGINE itself restores the desktop
// (vid_restart/quit) - that is a deliberate exit from fullscreen, not the OS.
bool mode_guard_reapply_fullscreen();
bool mode_guard_fullscreen_size(int* w, int* h);

}  // namespace patches

#endif
