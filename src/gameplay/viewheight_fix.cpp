#include "gameplay/viewheight_fix.h"
#include "core/logger.h"

#include <cstdio>

namespace patches {

ViewheightFixConfig g_viewheight_config = {
    VIEWHEIGHT_LERP_SPEED_FALLBACK,
};

namespace {

bool patch_float(uintptr_t address, float value) {
    DWORD old_protect = 0;
    if (!VirtualProtect((void*)address, sizeof(float), PAGE_READWRITE, &old_protect))
        return false;
    *(float*)address = value;
    VirtualProtect((void*)address, sizeof(float), old_protect, &old_protect);
    return true;
}

}  // namespace

bool apply_viewheight_fix(HMODULE gamex86_module) {
    if (!gamex86_module) return false;

    const uintptr_t base   = (uintptr_t)gamex86_module;
    const uintptr_t target = base + VIEWHEIGHT_LERP_SPEED_RVA;

    const float current = *(const float*)target;
    if (current != VIEWHEIGHT_LERP_SPEED_DEFAULT) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "cod1reloaded: viewheight lerp speed sanity check failed.\n"
            "Expected %.1f at gamex86+0x%08x, got %.4f.\n"
            "Aborting patch.",
            VIEWHEIGHT_LERP_SPEED_DEFAULT, (unsigned)VIEWHEIGHT_LERP_SPEED_RVA, current);
        MessageBoxA(NULL, msg, "cod1reloaded", MB_OK | MB_ICONWARNING);
        return false;
    }

    return patch_float(target, g_viewheight_config.viewheight_lerp_speed);
}

}  // namespace patches
