#include "netcode/competitive.h"
#include "netcode/protocol_patch.h"   // CODMP_CVAR_GET_VA
#include "core/logger.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace patches {

CompetitiveConfig g_competitive_config = {
    /* enable        */ false,
    /* snaps         */ 40,
    /* cl_maxpackets */ 125,
    /* rate          */ 25000,
    /* lock_cvars    */ true,    // cod2x parity: player cannot change them
    /* maxfps_min    */ 125,     // cod2x: com_maxFps limits 125..250
    /* maxfps_max    */ 250,
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
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        logger::logf("  competitive: CoDMP.exe base != 0x400000, abort caps");
        return false;
    }

    // NOTE: do NOT call any engine function (Cvar_Get/Set/FindVar) here - this runs from
    // DllMain (DLL_PROCESS_ATTACH), BEFORE Com_Init/Cvar_Init. Calling the engine here
    // crashes DLL init (0xc0000142). sv_competitive is pre-registered later, from
    // competitive_force_cvars(), which only runs once the cvar system is up. Byte-patches
    // below are safe at attach (they poke already-mapped static code in CoDMP.exe).

    // Lift the caps unconditionally: this only RAISES the ceiling (snaps 30->40,
    // cl_maxpackets 100->125). It forces nothing by itself - the client still runs its
    // configured value until a server spec locks it. Same as cod2x's cgame_patch.
    const uint8_t snaps_new = 0x28; // 40 -- patch BOTH the compare and the assignment
    bool ok = true;
    ok &= poke_verify(CODMP_SNAPS_CAP_CMP_VA, CODMP_SNAPS_CAP_OLD, snaps_new, "snaps cap cmp");
    ok &= poke_verify(CODMP_SNAPS_CAP_MOV_VA, CODMP_SNAPS_CAP_OLD, snaps_new, "snaps cap mov");
    ok &= poke_verify(CODMP_MAXPACKETS_CAP_VA, CODMP_MAXPACKETS_OLD, 0x7d, "cl_maxpackets cap");

    logger::logf("  competitive: caps %s (snaps->40, cl_maxpackets->125)",
                 ok ? "applied" : "PARTIAL");
    return ok;
}

namespace {

typedef void* (__cdecl* Cvar_Set_t)(const char*, const char*);
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
const Cvar_Set_t     cv_set  = (Cvar_Set_t)CODMP_CVAR_SET_VA;
const Cvar_FindVar_t cv_find = (Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA2;
const Cvar_Get_t     cv_get  = (Cvar_Get_t)CODMP_CVAR_GET_VA;

// Force `value` then ROM-lock: the engine refuses any later player set. Our own
// Cvar_Set passes force=1 and bypasses the ROM check, so we keep write access.
void lock_exact(const char* name, int value) {
    if (value <= 0) return;
    void* cv = cv_find(name);
    if (!cv) return;
    int* integer = (int*)((char*)cv + COMP_CVAR_INT);
    if (*integer != value) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", value);
        cv_set(name, buf);                      // force=1
    }
    if (g_competitive_config.lock_cvars)
        *(uint32_t*)((char*)cv + COMP_CVAR_FLAGS) |= COMP_CVAR_ROM;
}

// Ranged cvar (cod2x narrows com_maxFps to 125..250 and lets the player pick inside).
// CoD1 has no limits field, so we clamp back instead; polled fast enough that an
// out-of-range value never survives a frame. No ROM here - the range must stay usable.
void clamp_range(const char* name, int lo, int hi) {
    if (lo <= 0 || hi < lo) return;
    void* cv = cv_find(name);
    if (!cv) return;
    const int v = *(int*)((char*)cv + COMP_CVAR_INT);
    if (v >= lo && v <= hi) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v < lo ? lo : hi);
    cv_set(name, buf);
}

// Track which cvars WE locked, so we can release them when the server spec changes
// or clears. Bounded; the server file will never list many cvars.
constexpr int MAX_LOCKED = 24;
char g_locked_names[MAX_LOCKED][32] = {};
int  g_locked_count = 0;

// Drop the ROM bit we added, on every cvar we locked, then forget them.
void unlock_all() {
    for (int i = 0; i < g_locked_count; ++i) {
        void* cv = cv_find(g_locked_names[i]);
        if (cv) *(uint32_t*)((char*)cv + COMP_CVAR_FLAGS) &= ~COMP_CVAR_ROM;
    }
    g_locked_count = 0;
}

void remember_locked(const char* name) {
    for (int i = 0; i < g_locked_count; ++i)
        if (!strcmp(g_locked_names[i], name)) return;
    if (g_locked_count < MAX_LOCKED) {
        strncpy(g_locked_names[g_locked_count], name, 31);
        g_locked_names[g_locked_count][31] = 0;
        ++g_locked_count;
    }
}

// Read the server-published spec string. The server (.so) reads competitive.cfg and
// sets the cvar "sv_competitive" with CVAR_SYSTEMINFO, so the engine mirrors it into
// our cvar table (the same channel sv_pure uses). Format, space separated:
//     name=value        -> lock the cvar to exactly <value> (ROM: player can't change)
//     name=min:max      -> clamp the cvar into [min,max] (player picks inside)
// Empty / cvar absent  -> not in competitive mode. We pre-register it (init) so it
// always exists locally and systeminfo just updates its value.
// The engine refuses a cvar value over MAX_CVAR_VALUE_STRING (256) - it aborts the
// SERVER on the next map change - so a long spec is split across sv_competitive,
// sv_competitive2, ... Read them all back and join them with a space.
constexpr int SPEC_PARTS = 4;
constexpr int SPEC_JOINED_MAX = 1024;

const char* read_spec() {
    static char joined[SPEC_JOINED_MAX];
    joined[0] = 0;
    size_t len = 0;
    for (int i = 0; i < SPEC_PARTS; ++i) {
        char nm[32];
        if (i == 0) snprintf(nm, sizeof(nm), "sv_competitive");
        else        snprintf(nm, sizeof(nm), "sv_competitive%d", i + 1);
        void* cv = cv_find(nm);
        if (!cv) continue;
        const char* s = *(const char**)((char*)cv + COMP_CVAR_STRING);
        if (!s || !s[0]) continue;
        const size_t n = strlen(s);
        if (len + n + 2 >= sizeof(joined)) break;
        if (len) joined[len++] = ' ';
        memcpy(joined + len, s, n);
        len += n;
        joined[len] = 0;
    }
    return joined;
}

// Apply one "name=..." token. Returns nothing; only touches cvars that exist locally
// (a server typo is a harmless no-op, never creates a junk locked cvar).
void apply_token(const char* name, const char* rhs) {
    if (!cv_find(name)) return;
    const char* colon = strchr(rhs, ':');
    if (colon) {                                  // range min:max -> clamp
        clamp_range(name, atoi(rhs), atoi(colon + 1));
    } else {                                      // exact -> lock
        lock_exact(name, atoi(rhs));
        if (g_competitive_config.lock_cvars) remember_locked(name);
    }
}

}  // namespace

void competitive_force_cvars() {
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return;

    // Pre-register sv_competitive here (NOT in apply_competitive_caps, which runs at DLL
    // attach before the cvar system exists). This is only reached from the watcher once
    // CODMP_CVAR_COUNT_VA > 0, so the engine is ready. Ensures the cvar exists locally so
    // the server's systeminfo just updates its value.
    static bool s_registered = false;
    if (!s_registered) {
        s_registered = true;
        for (int i = 0; i < SPEC_PARTS; ++i) {
            char nm[32];
            if (i == 0) snprintf(nm, sizeof(nm), "sv_competitive");
            else        snprintf(nm, sizeof(nm), "sv_competitive%d", i + 1);
            cv_get(nm, "", 0);
        }
    }

    static char  s_applied[SPEC_JOINED_MAX] = "";   // last spec we acted on (detect changes)
    static DWORD s_last = 0;
    const  DWORD now = GetTickCount();

    const char* spec = read_spec();

    // spec cleared (left the server / admin turned it off) -> release our locks once
    if (!spec[0]) {
        if (g_locked_count || s_applied[0]) {
            unlock_all();
            s_applied[0] = 0;
            logger::logf("  competitive: no server spec -> cvars unlocked");
        }
        return;
    }

    // Re-apply fast so a value set from the console is pulled back within a tick
    // (ROM already refuses exact-locked ones; this also re-clamps ranges).
    if (s_last != 0 && (now - s_last) < 100) return;
    s_last = now;

    const bool changed = strncmp(spec, s_applied, sizeof(s_applied)) != 0;
    if (changed) {
        unlock_all();                              // release old set before applying new
        strncpy(s_applied, spec, sizeof(s_applied) - 1);
        s_applied[sizeof(s_applied) - 1] = 0;
    }

    // parse "name=rhs" tokens separated by spaces
    char buf[SPEC_JOINED_MAX];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char* tok = strtok(buf, " \t"); tok; tok = strtok(nullptr, " \t")) {
        char* eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        apply_token(tok, eq + 1);
    }

    if (changed)
        logger::logf("  competitive: applied server spec \"%s\"", spec);
}

}  // namespace patches
