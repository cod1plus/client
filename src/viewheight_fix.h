#ifndef COD1RELOADED_VIEWHEIGHT_FIX_H
#define COD1RELOADED_VIEWHEIGHT_FIX_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// gamex86.dll preferred load base
constexpr uintptr_t GAMEX86_PREFERRED_BASE = 0x20000000;

// Adresse (a la base preferee) du float de vitesse de lerp de viewheight dans
// PM_UpdateViewHeight. Lu via `fmul dword [0x2009a1b0]` a gamex86+0x5557b.
// Valeur par defaut : 180.0f (units/sec).
constexpr uintptr_t VIEWHEIGHT_LERP_SPEED_RVA     = 0x0009a1b0;
constexpr float     VIEWHEIGHT_LERP_SPEED_DEFAULT  = 180.0f;
// 100.0 = sync exact avec l'animation 200ms, 150.0 = compromis (~133ms).
constexpr float     VIEWHEIGHT_LERP_SPEED_FALLBACK = 150.0f;

struct ViewheightFixConfig {
    float viewheight_lerp_speed;
};

extern ViewheightFixConfig g_viewheight_config;

// Patche le float de vitesse dans gamex86.dll.
// Retourne false si le sanity-check echoue (mauvaise version).
bool apply_viewheight_fix(HMODULE gamex86_module);

}  // namespace patches

#endif
