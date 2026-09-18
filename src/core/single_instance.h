#ifndef COD1RELOADED_SINGLE_INSTANCE_H
#define COD1RELOADED_SINGLE_INSTANCE_H

#include <windows.h>

// Two CoDMP.exe at once = shared config, shared logs, two clients fighting over
// the same net_port: "ca nique tout" (AMD alt-tab report, 2026-08-24).
//
// The engine itself can spawn a second copy: CoDMP.exe builds the command line
// `%s monkey %d %d "%s"` (verified at 0x467fc0-0x468084: OpenProcess on the
// parent PID, then CreateProcessA), the idTech3 "relaunch and wait for the old
// process to die" pattern. When the parent hangs - which is exactly what a lost
// GL context / display-mode change does on AMD after an alt-tab out of exclusive
// fullscreen - nobody ever exits and both stay alive. A player double-clicking
// the shortcut on a game that looks frozen produces the same end state.
//
// Guard, called as early as possible in DLL_PROCESS_ATTACH:
//   - a legit `monkey` relaunch is ALWAYS allowed through (and it actively
//     helps: it waits for the parent, then kills it if it is wedged);
//   - a plain duplicate launch is refused: the existing window is brought to
//     the front and this process exits before it touches a single file.
// The mutex is namespaced by the exe PATH, so a dev folder and the main install
// still run side by side.
namespace patches {

void single_instance_guard();

}  // namespace patches

#endif
