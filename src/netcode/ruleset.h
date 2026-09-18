#ifndef COD1RELOADED_RULESET_H
#define COD1RELOADED_RULESET_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// PunkBuster-style ruleset enforcement (the cod2x way of carrying rules: in the client,
// the server only names the set). The list comes from the store (ruleset_fetch.h):
// downloaded from the cod1plus/rulesets repo at start-up and on request, cached on
// disk, with tools/gen_ruleset.py's compiled table (ruleset_table.h, built from
// rulesets/<id>.pb) as the offline fallback.
//
// SERVER SIDE: cod1plus.so publishes the systeminfo cvar `sv_competitive_ruleset` with
// "<id>" or "<id>@<version>" (file ruleset.txt next to competitive.cfg, re-read live;
// default "codbase-2023-05"). A client behind that version re-downloads in game.
//
// CLIENT SIDE, while the id matches ours and we are in game:
//   * every value rule of the table is checked ~4x/s on the real cvar table and FIXED
//     (PB could only kick; we can set): exact -> value + ROM lock (player cannot change
//     it), range -> clamp, OUT -> engine default, INCLUDE/EXCLUDE -> engine default;
//   * a cvar named in the server's pushed competitive.cfg spec (competitive.cpp) is left
//     to that spec: the server's values override the embedded ones (com_maxfps,
//     cl_maxpackets, rate, cg_fov ...);
//   * CVAR_LATCH cvars count as satisfied when their LATCHED value satisfies the rule
//     (the string only changes at vid_restart);
//   * RM_PROBE rules (names the CoD1 client never registers - cheat config knobs, other
//     games' cvars) are counted when such a cvar EXISTS; report only, never fixed;
// The verdict goes to the server in the USERINFO cvar `cod1x_rs` (ROM), where <id> is
// "<base>@<version enforced>":
//     "<id>:ok"                       every rule satisfied at the last pass
//     "<id>:v<n>:<cvar>[,<cvar>]"     n PERSISTENT violations = a cvar we fixed that
//                                     was found out of rule again (a client that fights
//                                     the enforcement: modified build, external tool)
//     "<id>:p<n>:<name>"              n probe hits (informative, see cheat_scan.cpp for
//                                     the curated list that is allowed to kick)
// Off a ruleset server (or during a demo, when sv_competitive_ruleset is empty) every
// ROM bit we added is released and the report is cleared.
//
// HONEST LIMITS: same as cheat_scan - a recompiled client can lie. The version gate is
// what raises that bar. Kicks happen on the server (cheat_gate.c), never here.

void ruleset_tick();    // watcher thread, once the cvar system exists

bool        ruleset_active();  // true while enforcing (a list for the server's id, in game)
const char* ruleset_id();      // the EMBEDDED id (fallback)

}  // namespace patches

#endif
