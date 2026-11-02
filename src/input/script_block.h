#ifndef COD1RELOADED_SCRIPT_BLOCK_H
#define COD1RELOADED_SCRIPT_BLOCK_H

namespace patches {

// exec and vstr out of the players' hands (competitive rule, 2026-11-03): a bind whose text runs
// either is refused - the key is left unbound, so the next config write drops the line - and a
// console line that runs either is not executed, both with a red line in the console. A bind that
// fires and moves (+attack / -attack with +forward, +moveup...: the scripted run-jump-throw) is
// refused the same way; the jump-throw on +gostand stays allowed. The
// engine's own execs (config_mp.cfg at start and on a mod switch, the menus' scripts, the command
// line) do not come through these doors and are left alone. Installed once at attach (CoDMP.exe
// 1.5); a different build is left untouched.
bool script_block_install();

}  // namespace patches

#endif
