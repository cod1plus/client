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
// view axes, default "14 -5 -5"), cod1x_inspect_speed (1 = 3.2 s / 6.2 s for the full one).
//
// `set cod1x_inspect 2` = the full one (6.2 s): right side with a chamber check (the bolt
// pulled and released; a bolt action lifts its handle first), then the magazine turned to the
// camera, pulled out cod1x_inspect_mag units (5), looked at and seated, then the left side.
// The weapon's own bones move: cg_weapons[w] (cgame 0x301aa4e0, 0x198 each) +0 = the view
// DObj (hands + weapon); syscall 0xaf (dobj, name) -> bone index or -1 finds `<gun>_clip` /
// `<gun>_bolt` / tag_flash; the render skeleton builder (bone_probe) hands every skeleton it
// builds to inspect_on_skeleton, which moves those bones in the matrices just built.
bool inspect_install(HMODULE cgame);   // every cgame load: the call site lives in cgame
void inspect_on_skeleton(const void* dobj);   // from bone_probe, after every skeleton build (one compare when idle)

}  // namespace patches

#endif
