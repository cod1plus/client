#include "gameplay/viewheight_fix.h"
#include "core/logger.h"

#include <cstdio>

namespace patches {

// COMPETITIVE RELEASE: HARDCODED (not read from cod1reloaded.ini). The first-person
// crouch view speed is the up/down mechanic - locked so it can't be tuned for an edge.
// 80 = the tuned value as of 2026-07-24 (vanilla 180). Rebuild to change.
ViewheightFixConfig g_viewheight_config = {
    // 150 = the 1.6.0 value (~133 ms), restored 2026-08-01. Paired with the VANILLA
    // 400 ms model stance blend (stance_fix off), this is the anti up-down behaviour
    // players validated at launch: a crouch-spammer's model moves slowly and stays
    // readable, so he is easy to track and to shoot. Speeding the model up to 250 ms
    // made it follow the spam closely and it read as flicker ("on voit le joueur
    // briller"). Change these two together or not at all.
    150.0f,
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
