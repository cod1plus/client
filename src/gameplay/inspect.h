#ifndef COD1RELOADED_INSPECT_H
#define COD1RELOADED_INSPECT_H

#include <windows.h>

namespace patches {

// Weapon inspect (TEST). `set cod1x_inspect 1` - bind a key to it - turns the first-person
// weapon in front of the camera for ~3 s: right side (bolt / ejection port), then the left
// side and the underside, then back. Firing, aiming, reloading or switching weapon cuts it.
// Purely local: only where the view model is drawn changes, nothing reaches the server.
//
// cgame_mp_x86.dll 0x30038b60 = CG_AddViewWeapon(ps): builds the hand refEntity on its stack
// (Q3 layout: axis at +0x1c, origin at +0x44, renderfx 0xc = first person | depth hack) and
// hands it to 0x30038660 (ecx = that refEntity; stack: ps, &cg.predictedPlayerEntity, ps),
// which copies it into the weapon model and submits both. Its only call site, 0x30038f41,
// is repointed at a stub that turns the refEntity first. (The memory note of 2026-07-20 had
// 0x300177f0 as the view weapon: that one draws the crosshair.)
// Tuning, live: cod1x_inspect_pivot "f l u" (the point turned around, from the hand origin,
// view axes, default "14 -5 -5"), cod1x_inspect_speed (1 = 3.2 s).
bool inspect_install(HMODULE cgame);   // every cgame load: the call site lives in cgame

}  // namespace patches

#endif
