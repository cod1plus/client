#ifndef COD1RELOADED_AVATAR_OVERLAY_H
#define COD1RELOADED_AVATAR_OVERLAY_H

#include <windows.h>

namespace patches {

// Configuration de l'overlay HUD (barre scoreboard : avatars + timer +
// scores, rendue par-dessus le HUD du jeu via engine_2d).
struct AvatarOverlayConfig {
    bool  enable;             // interrupteur principal
    char  test_url[512];      // URL to download and display
    int   x;                  // overlay position X (screen coords)
    int   y;                  // overlay position Y
    int   width;              // overlay size W
    int   height;             // overlay size H
};

extern AvatarOverlayConfig g_avatar_overlay_config;

// PHASE 1 : download the avatar PNG, convert to TGA, package into a
// .pk3 BEFORE the engine starts scanning paks. Must be called from
// DllMain (after config loaded). Blocks the calling thread for the
// download duration (typically 100-300ms). If the pk3 already exists
// from a previous launch, this is a no-op (cached).
void avatar_overlay_prepare_pk3_blocking();

// PHASE 2 : install the engine_2d hooks and per-frame draw callback.
// Called from the cgame patch watcher (when cgame_mp_x86.dll loads).
void avatar_overlay_show_test();

// Cleanup at DLL_PROCESS_DETACH.
void avatar_overlay_shutdown();

}  // namespace patches

#endif
