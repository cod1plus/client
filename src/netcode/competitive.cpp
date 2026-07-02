#include "netcode/competitive.h"
#include "core/logger.h"

#include <cstdio>

namespace patches {

CompetitiveConfig g_competitive_config = {
    /* enable        */ false,
    /* snaps         */ 40,
    /* cl_maxpackets */ 125,
    /* rate          */ 25000,
};

namespace {

// verify the current byte, then patch it (executable code -> flush icache)
bool poke_verify(uintptr_t va, uint8_t expect, uint8_t val, const char* tag) {
    BYTE* p = (BYTE*)va;
    if (*p != expect) {
        logger::logf("  competitive: %s @0x%08x is 0x%02x (expected 0x%02x) -> skip",
                     tag, (unsigned)va, *p, expect);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    *p = val;
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
    logger::logf("  competitive: %s @0x%08x 0x%02x -> 0x%02x", tag, (unsigned)va, expect, val);
    return true;
}

}  // namespace

bool apply_competitive_caps() {
    if (!g_competitive_config.enable) return true;

    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        logger::logf("  competitive: CoDMP.exe base != 0x400000, abort caps");
        return false;
    }

    const uint8_t snaps_new = 0x28; // 40 -- patch BOTH the compare and the assignment
    bool ok = true;
    ok &= poke_verify(CODMP_SNAPS_CAP_CMP_VA, CODMP_SNAPS_CAP_OLD, snaps_new, "snaps cap cmp");
    ok &= poke_verify(CODMP_SNAPS_CAP_MOV_VA, CODMP_SNAPS_CAP_OLD, snaps_new, "snaps cap mov");
    ok &= poke_verify(CODMP_MAXPACKETS_CAP_VA, CODMP_MAXPACKETS_OLD, 0x7d, "cl_maxpackets cap");

    logger::logf("  competitive: caps %s (snaps->40, cl_maxpackets->125)", ok ? "applied" : "PARTIAL");
    return ok;
}

void competitive_force_cvars() {
    if (!g_competitive_config.enable) return;
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return;

    // Re-lock at ~0.5 Hz: overrides a late config.cfg exec and stops a player lowering
    // snaps mid-match. Cvar_Set is a no-op when the value is unchanged (no userinfo spam).
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (last != 0 && (now - last) < 2000) return;
    last = now;

    typedef void* (__cdecl *Cvar_Set_t)(const char*, const char*);
    Cvar_Set_t Cvar_Set = (Cvar_Set_t)CODMP_CVAR_SET_VA;

    char buf[16];
    if (g_competitive_config.snaps > 0) {
        snprintf(buf, sizeof(buf), "%d", g_competitive_config.snaps);
        Cvar_Set("snaps", buf);
    }
    if (g_competitive_config.cl_maxpackets > 0) {
        snprintf(buf, sizeof(buf), "%d", g_competitive_config.cl_maxpackets);
        Cvar_Set("cl_maxpackets", buf);
    }
    if (g_competitive_config.rate > 0) {
        snprintf(buf, sizeof(buf), "%d", g_competitive_config.rate);
        Cvar_Set("rate", buf);
    }

    static bool logged = false;
    if (!logged) {
        logged = true;
        logger::logf("  competitive: forced snaps=%d cl_maxpackets=%d rate=%d",
                     g_competitive_config.snaps, g_competitive_config.cl_maxpackets,
                     g_competitive_config.rate);
    }
}

}  // namespace patches
