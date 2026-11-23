#ifndef COD1RELOADED_USE_PICKUP_H
#define COD1RELOADED_USE_PICKUP_H

#include <windows.h>

namespace patches {

// A weapon lying on the ground is no longer predicted as picked up when the player walks over
// it: no pickup sound and the weapon stays drawn. Client half of cod1plus.so's use_pickup (the
// server gives a weapon's ammo on the use key only): without it the cgame keeps predicting the
// walk-over ammo pickup the server no longer does. Health packs and the other items keep their
// prediction. On a server that still gives the ammo on contact, the pickup arrives with the
// server's own event (as with cg_predictItems 0, for weapons only).
//
// cod1x_use_pickup 1 (default) / 0 = the game's prediction, live. Re-installed on every cgame
// load; a different cgame build is left untouched.
bool use_pickup_install(HMODULE cgame);
void use_pickup_tick();

}  // namespace patches

#endif
