#ifndef COD1RELOADED_ANIM_CLAMP_H
#define COD1RELOADED_ANIM_CLAMP_H

#include <windows.h>

namespace patches {

// Stop the "Player animation index out of range (N): M" kick to the main menu.
// cgame's BG_GetAnimationForIndex Com_Error's when the requested index >= the
// runtime animation count. On a heavily-modded server the client can register a
// slightly different animation count than the server, so it receives an index it
// never registered and the player is dropped. This patches the check to CLAMP the
// index to 0 (return a valid default animation) instead of erroring — no kick.
// Mirror of the server-side clamp in cod1plus.so (anim_clamp.c).
//
// Idempotent, verify-then-write; a different cgame build is left untouched.
// Re-applied on every cgame (re)load via apply_to_cgame.
bool apply_anim_clamp(HMODULE cgame_module);

}  // namespace patches

#endif
