// cheat_scan.cpp - cvar-name cheat detection (the useful half of PunkBuster).
// See cheat_scan.h for the design and its honest limits.

#include "netcode/cheat_scan.h"
#include "netcode/protocol_patch.h"   // CODMP_CVAR_GET_VA, CODMP_CVAR_COUNT_VA, CVAR_USERINFO
#include "netcode/competitive.h"      // CODMP_CVAR_FINDVAR_VA2, COMP_CVAR_ROM
#include "core/logger.h"

#include <cstdio>
#include <cstring>

namespace patches {

namespace {

typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
const Cvar_Get_t     cs_get  = (Cvar_Get_t)CODMP_CVAR_GET_VA;
const Cvar_FindVar_t cs_find = (Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA2;

// ---------------------------------------------------------------------------
// BLOCKLIST - cvar names registered by public CoD1/CoD-engine cheats.
//
// CURATION RULE (important): only names that CANNOT plausibly be a legitimate
// player alias. PunkBuster's own list contains generic words - fov, gun, con,
// team, key, help, mode, safe, quiet, names, weapons, predict, box, tree, sky,
// fire, shoot, melee, aim, wall, radar, recoil, angles, alias ... A player who
// writes `seta fov "vstr fov1"` in his own config would be flagged by those.
// Kicking an innocent player mid-match is worse than missing one cheater, so
// every ambiguous single word is DELIBERATELY EXCLUDED here. What remains are
// prefixed / compound names that only a cheat creates.
// ---------------------------------------------------------------------------
const char* const kCheatCvars[] = {
    // OGC (one of the most common CoD1 cheats)
    "ogc_aim", "ogc_bot", "ogc_fov", "ogc_glow", "ogc_mode", "ogc_names",
    "ogc_trans", "ogc_wall", "ogc_weapons",
    // "w_" cheat family (wallhack/aimbot menus)
    "w_aim", "w_aimbot", "w_antirecoil", "w_autobash", "w_autolean", "w_barhack",
    "w_bot", "w_chams", "w_clearscope", "w_cross", "w_crosshair", "w_fog",
    "w_fovx", "w_fovy", "w_modelcolor", "w_models", "w_nofog", "w_pbss",
    "w_recoil", "w_ringhack", "w_scope", "w_skycolor", "w_skyhack", "w_stealth",
    "w_vehicleesp", "w_wall", "w_wallhack", "w_walls", "w_whitewalls",
    // hax_ / mom_ / orgy_ / cu_ / ic_ / sjs_ families
    "hax_aim", "hax_aimbot", "hax_distance", "hax_radar", "hax_shoot",
    "hax_stats", "hax_wallhack",
    "mom_aimbot", "mom_radar", "mom_stats", "mom_wallhack",
    "orgy_aim_fov", "orgy_aim_pingpredict", "orgy_defaultSens",
    "orgy_radar_range", "orgy_serverkill", "orgy_wallhack",
    "cu_aim_key", "cu_aimvecs", "cu_wallhack",
    "ic_aim_key", "ic_radar_width",
    "sjs_aim", "sjs_shoot", "sjs_wall",
    // cod_ cheat family (note: NOT a real CoD1 prefix - the game uses cg_/cl_/com_)
    "cod_aimbot", "cod_autoshoot", "cod_fov", "cod_names", "cod_nofog",
    "cod_predict", "cod_radar", "cod_shoot", "cod_wallhack", "cod_weapons",
    // bot_ cheat entries (bot_enable/bot_minplayers are legit and excluded)
    "bot_aim", "bot_distance", "bot_radar", "bot_wallhack",
    // underscore-prefixed cheat knobs
    "_aim_key", "_aimfov", "_aimkey", "_aimpredict", "_aimshoot", "_aimvecz",
    "_autoshoot", "_killsounds", "_radar", "_wall", "_wallhack",
    "_491ebf653aed411e",
    // explicit cheat words (unambiguous on their own)
    "wallhack", "aimbot", "autoaim", "ignorewalls", "esp_all", "esp_names",
    "esp_off", "esp_weapons", "b_wallhack", "pesp", "radarconf",
    // renamed engine debug cvars used by cheat builds
    "bs_showCullXModels", "bs_showsurfcounts",
    "z_showCullXModels",  "z_showsurfcounts",
};
constexpr int kCheatCvarCount = (int)(sizeof(kCheatCvars) / sizeof(kCheatCvars[0]));

int  g_hits = 0;
bool g_registered = false;
char g_verdict[32] = "0";

}  // namespace

int cheat_scan_hits() { return g_hits; }

void cheat_scan_tick() {
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return;
    // engine cvar system must exist (same guard the watcher uses for the other modules)
    if (*(volatile int*)CODMP_CVAR_COUNT_VA <= 0) return;

    if (!g_registered) {
        g_registered = true;
        // userinfo so the server sees it at connect (same channel as the version gate).
        // ROM so the player cannot fake a clean verdict from his own console.
        cs_get("cod1x_ac", "0", CVAR_USERINFO | COMP_CVAR_ROM);
    }

    // Re-scan periodically: a cheat can be loaded AFTER connecting.
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (last != 0 && (now - last) < 5000) return;
    last = now;

    int hits = 0;
    const char* first = nullptr;
    for (int i = 0; i < kCheatCvarCount; ++i) {
        if (cs_find(kCheatCvars[i])) {
            ++hits;
            if (!first) first = kCheatCvars[i];
        }
    }

    if (hits != g_hits) {
        g_hits = hits;
        snprintf(g_verdict, sizeof(g_verdict), "%d", hits);
        // Publish via Cvar_Get: the cvar is ROM, so a plain Cvar_Set from us would be
        // fine (force=1) but Cvar_Get with the new default also refreshes userinfo.
        typedef void* (__cdecl* Cvar_Set_t)(const char*, const char*);
        ((Cvar_Set_t)CODMP_CVAR_SET_VA)("cod1x_ac", g_verdict);

        if (hits > 0)
            logger::logf("cheat_scan: %d blocklisted cvar(s) present (first: %s)",
                         hits, first ? first : "?");
        else
            logger::logf("cheat_scan: clean");
    }
}

}  // namespace patches
