#include "netcode/competitive.h"
#include "netcode/ruleset_eval.h"    // rule_ok / rule_fix: the one place value semantics live
#include "netcode/protocol_patch.h"   // CODMP_CVAR_GET_VA
#include "core/logger.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace patches {

// Live systeminfo lookup. CL_SystemInfoChanged (CoDMP.exe 0x4176f0, RE 2026-09-16)
// applies the server's systeminfo by walking its "\key\value" pairs and calling
// Cvar_Set2(force) on EACH KEY PRESENT - a key the new server does not publish keeps
// whatever the PREVIOUS server set. So "the cvar is non-empty" never meant "this
// server publishes it": a player leaving a competitive server for a fun server kept the
// spec, and would keep the ruleset id. The engine's own truth is the CS_SYSTEMINFO
// configstring, which the gamestate parser rewrites for every server / map:
//   cl.gameState.stringOffsets[1] @0x148dcc0  (int, into stringData)
//   cl.gameState.stringData       @0x148fcbc  (MAX_GAMESTATE_CHARS = 16000)
// Offset 0 = the shared empty string (cleared at disconnect) -> nothing published.
constexpr uintptr_t CODMP_GS_SYSTEMINFO_OFF_VA = 0x0148dcc0;
constexpr uintptr_t CODMP_GS_STRINGDATA_VA     = 0x0148fcbc;
constexpr int       CODMP_MAX_GAMESTATE_CHARS  = 16000;

bool systeminfo_value(const char* key, char* out, int outsz) {
    if (outsz <= 0) return false;
    out[0] = 0;
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return false;
    const int off = *(volatile int*)CODMP_GS_SYSTEMINFO_OFF_VA;
    if (off <= 0 || off >= CODMP_MAX_GAMESTATE_CHARS) return false;
    const char* s   = (const char*)CODMP_GS_STRINGDATA_VA + off;
    const char* end = (const char*)CODMP_GS_STRINGDATA_VA + CODMP_MAX_GAMESTATE_CHARS;
    const int klen = (int)strlen(key);
    // "\key\value\key\value..." - keys compare case-insensitively (Info_ValueForKey)
    while (s < end && *s == '\\') {
        const char* k = s + 1;
        const char* kend = k;
        while (kend < end && *kend && *kend != '\\') ++kend;
        if (kend >= end || !*kend) return false;
        const char* v = kend + 1;
        const char* vend = v;
        while (vend < end && *vend && *vend != '\\') ++vend;
        if ((kend - k) == klen && !_strnicmp(k, key, klen)) {
            int n = (int)(vend - v);
            if (n > outsz - 1) n = outsz - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return true;
        }
        if (vend >= end || !*vend) return false;
        s = vend;
    }
    return false;
}

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
// The value is compared and written as the STRING the server sent: 0, negatives and
// decimals (m_yaw 0.022, cl_timenudge -20) are all legal. (Until 2026-09-16 this took an
// int and returned on value <= 0, so every "x 0" rule of competitive.cfg - r_fullbright,
// cl_nodelta, s_show, pmove_fixed ... - and m_yaw 0.022 were silently never applied.)
void lock_exact(const char* name, const char* value) {
    void* cv = cv_find(name);
    if (!cv) return;
    RuleDef r = { name, RM_EXACT, 0, 0.f, 0.f, "" };
    const char* cur = *(const char**)((char*)cv + COMP_CVAR_STRING);
    bool same;
    if (sscanf(value, "%f", &r.a) == 1) {
        r.b = r.a;
        same = rule_ok(r, cur);                 // numeric compare, tolerance 1e-4
    } else {
        same = cur && !strcmp(cur, value);      // non-numeric: literal
    }
    if (!same) cv_set(name, value);             // force=1
    if (g_competitive_config.lock_cvars)
        *(uint32_t*)((char*)cv + COMP_CVAR_FLAGS) |= COMP_CVAR_ROM;
}

// Ranged cvar (cod2x narrows com_maxFps to 125..250 and lets the player pick inside).
// CoD1 has no limits field, so we clamp back instead; polled fast enough that an
// out-of-range value never survives a frame. No ROM here - the range must stay usable.
// Bounds are floats: "cl_timenudge -20 0" and "r_picmip 0 3" are legal ranges.
void clamp_range(const char* name, const char* lo_s, const char* hi_s) {
    RuleDef r = { name, RM_RANGE, 0, 0.f, 0.f, "" };
    if (sscanf(lo_s, "%f", &r.a) != 1 || sscanf(hi_s, "%f", &r.b) != 1 || r.b < r.a) return;
    void* cv = cv_find(name);
    if (!cv) return;
    const char* cur = *(const char**)((char*)cv + COMP_CVAR_STRING);
    if (rule_ok(r, cur)) return;
    char buf[32];
    if (rule_fix(r, cur, "", buf, sizeof(buf))) cv_set(name, buf);
}

// Track which cvars WE locked, so we can release them when the server spec changes
// or clears. Bounded; the server file will never list many cvars.
// 96: one competitive.cfg fills 4 x 220 chars = ~45 rules; the old 24 overflowed once
// the "x 0" rules started applying (a cvar past the table stayed ROM after leaving).
constexpr int MAX_LOCKED = 96;
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
        char nm[32], s[256];
        if (i == 0) snprintf(nm, sizeof(nm), "sv_competitive");
        else        snprintf(nm, sizeof(nm), "sv_competitive%d", i + 1);
        // from the live systeminfo, NOT the cvar: the cvar keeps the previous
        // server's value when the current one publishes nothing (see systeminfo_value)
        if (!systeminfo_value(nm, s, sizeof(s)) || !s[0]) continue;
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
        char lo[32];
        snprintf(lo, sizeof(lo), "%.*s", (int)(colon - rhs), rhs);
        clamp_range(name, lo, colon + 1);
    } else {                                      // exact -> lock
        lock_exact(name, rhs);
        if (g_competitive_config.lock_cvars) remember_locked(name);
    }
}

// Names present in the spec we are enforcing (the embedded ruleset defers to them).
constexpr int MAX_SPEC_NAMES = 96;
char g_spec_names[MAX_SPEC_NAMES][32];
int  g_spec_name_count = 0;

void note_spec_name(const char* name) {
    if (g_spec_name_count >= MAX_SPEC_NAMES) return;
    strncpy(g_spec_names[g_spec_name_count], name, 31);
    g_spec_names[g_spec_name_count][31] = 0;
    ++g_spec_name_count;
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
            g_spec_name_count = 0;
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
        g_spec_name_count = 0;
    }

    // parse "name=rhs" tokens separated by spaces
    char buf[SPEC_JOINED_MAX];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char* tok = strtok(buf, " \t"); tok; tok = strtok(nullptr, " \t")) {
        char* eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        if (changed) note_spec_name(tok);
        apply_token(tok, eq + 1);
    }

    if (changed)
        logger::logf("  competitive: applied server spec \"%s\"", spec);
}

bool competitive_spec_has(const char* name) {
    for (int i = 0; i < g_spec_name_count; ++i)
        if (!_stricmp(g_spec_names[i], name)) return true;
    return false;
}

}  // namespace patches
