#ifndef COD1RELOADED_CHEAT_SCAN_H
#define COD1RELOADED_CHEAT_SCAN_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// Cvar-based cheat detection - the useful half of PunkBuster, without PB.
//
// PunkBuster did two things through pbsv.cfg:
//   1. cvar RANGE checks (com_maxfps IN 40 250, snaps IN 20 40, cg_fov IN 80 80, ...)
//      -> we already do this, better: competitive.cpp CLAMPS/ROM-locks them from the
//         server's competitive.cfg instead of kicking, so a match never dies over a
//         wrong fps value.
//   2. a BLOCKLIST of cvar NAMES created by public cheats (wallhack, aimbot, esp,
//      ogc_*, hax_*, w_aim, cod_wallhack, ...). If the client had that cvar at all,
//      PB kicked. That half is what this module restores.
//
// How: every public CoD1 cheat registers its config cvars in the engine's own cvar
// table. We simply ask the engine (Cvar_FindVar) whether any blocklisted name exists.
// No memory scanning, no signatures - just "did a cheat register its knobs here".
// The result is published in the userinfo cvar "cod1x_ac" (USERINFO|ROM), which the
// server reads in its ClientConnect gate (same plumbing as the version gate).
//
// HONEST LIMITS - this is a cvar checker, not a kernel anti-cheat:
//   * catches the ordinary cheater running a public cheat build (what PB caught too);
//   * does NOT catch a recompiled client that lies about the result. The closed
//     ecosystem (protocol 10 + version gate) is what raises that bar, not this module.
// Never claim it is unbypassable.
//
// Fail-open by design: if anything is off (engine not ready, name list empty), we
// report "clean" rather than risk kicking an innocent player.

// Poll: registers cod1x_ac once the cvar system is up, then re-scans periodically and
// publishes the verdict. Cheap (a hash lookup per name, throttled). Safe to call every
// watcher tick; does nothing until the engine cvar system exists.
void cheat_scan_tick();

// Number of blocklisted names currently flagged as present (0 = clean). For logging.
int cheat_scan_hits();

}  // namespace patches

#endif
