#ifndef COD1RELOADED_AVATAR_OVERLAY_H
#define COD1RELOADED_AVATAR_OVERLAY_H

#include <windows.h>

namespace patches {

struct AvatarOverlayConfig {
    bool  enable;
    char  test_url[512];
    int   x;
    int   y;
    int   width;
    int   height;
};

extern AvatarOverlayConfig g_avatar_overlay_config;

// bakes pk3 before engine scans paks; call from DllMain. blocks; cached pk3 = no-op.
void avatar_overlay_prepare_pk3_blocking();

// installs engine_2d hooks; call once cgame_mp_x86.dll is loaded.
void avatar_overlay_show_test();

void avatar_overlay_shutdown();

}  // namespace patches

#endif
