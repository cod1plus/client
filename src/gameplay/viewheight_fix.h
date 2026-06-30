#ifndef COD1RELOADED_VIEWHEIGHT_FIX_H
#define COD1RELOADED_VIEWHEIGHT_FIX_H

#include <windows.h>
#include <stdint.h>

namespace patches {

constexpr uintptr_t GAMEX86_PREFERRED_BASE = 0x20000000;

// PM_UpdateViewHeight lerp speed; fmul dword [0x2009a1b0] @ gamex86+0x5557b
constexpr uintptr_t VIEWHEIGHT_LERP_SPEED_RVA     = 0x0009a1b0;
constexpr float     VIEWHEIGHT_LERP_SPEED_DEFAULT  = 180.0f;
constexpr float     VIEWHEIGHT_LERP_SPEED_FALLBACK = 150.0f;  // 100=sync w/ 200ms anim, 150=~133ms

struct ViewheightFixConfig {
    float viewheight_lerp_speed;
};

extern ViewheightFixConfig g_viewheight_config;

// false if sanity check fails (wrong version)
bool apply_viewheight_fix(HMODULE gamex86_module);

}  // namespace patches

#endif
