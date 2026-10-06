#ifndef COD1RELOADED_ANIM_CLAMP_H
#define COD1RELOADED_ANIM_CLAMP_H

#include <windows.h>

namespace patches {

// Stop the drop to the main menu on an out-of-range player animation index
// ("Player animation index out of range (%i): %i", "BG_GetAnimationForIndex: index out of
// bounds", ...): the index is one the client's animation table does not have - a modded server
// registering a different count, or a demo recorded on one and watched from the main menu.
// The five cgame checks fall back to animation 0 (a valid default pose) instead of erroring,
// without touching any register their callers rely on (see anim_clamp.cpp: the 2026-07
// version did, and was reverted). Mirror of the server-side clamp in cod1plus.so.
//
// Idempotent, verify-then-write, all five or none; a different cgame build is left untouched.
// Re-applied on every cgame (re)load via apply_to_cgame.
bool apply_anim_clamp(HMODULE cgame_module);

}  // namespace patches

#endif
