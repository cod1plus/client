// ruleset.cpp - embedded PunkBuster-style ruleset enforcement. See ruleset.h.

#include "netcode/ruleset.h"
#include "netcode/ruleset_eval.h"
#include "netcode/ruleset_table.h"
#include "netcode/ruleset_parse.h"
#include "netcode/ruleset_fetch.h"
#include "netcode/protocol_patch.h"   // CODMP_CVAR_GET_VA, CODMP_CVAR_COUNT_VA, CVAR_USERINFO
#include "netcode/competitive.h"      // CODMP_CVAR_SET_VA, CODMP_CVAR_FINDVAR_VA2, cvar_t offsets, competitive_spec_has
#include "core/logger.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace patches {

namespace {

typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
typedef void* (__cdecl* Cvar_Set_t)(const char*, const char*);
const Cvar_Get_t     rs_get  = (Cvar_Get_t)CODMP_CVAR_GET_VA;
const Cvar_FindVar_t rs_find = (Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA2;
const Cvar_Set_t     rs_set  = (Cvar_Set_t)CODMP_CVAR_SET_VA;

// cvar_t (plain Q3 layout, see competitive.h): name+0 string+4 resetString+8 latched+0xc flags+0x10
constexpr int CV_STRING  = 0x04;
constexpr int CV_RESET   = 0x08;
constexpr int CV_LATCHED = 0x0c;
constexpr int CV_FLAGS   = 0x10;
constexpr uint32_t CV_LATCH = 0x20;
constexpr uint32_t CV_ROM   = 0x40;
constexpr uint32_t CV_CHEAT = 0x200;

// NOTE: `wait` is deliberately LEFT ALONE (enzo, 2026-09-18). cod2x swallows it to kill
// scripted bursts, and 1.6.6 did the same for a while by rewriting the imm32 of Com_Init's
// "push Cmd_Wait_f" (@0x42ad9d). But CoD1 nade binds are built on it - switch to the
// grenade, cook, throw, switch back, with `wait` between the steps - so blocking it breaks
// a legitimate, universal technique. The ruleset governs cvar VALUES, not the console.

bool g_active = false;
bool g_registered = false;
char g_report[96] = "";

// per-rule memory: did we already fix this rule once (so a second miss = persistent)?
// sized to the list being enforced, reset when the store swaps it
std::vector<uint8_t>  g_fixed;
std::vector<uint16_t> g_persist;
std::vector<uint8_t>  g_engine_owned;   // the engine resets it to default after our set: hands off
const RuleList*       g_list = nullptr;
// cvars we ROM-locked (release them when the ruleset goes inactive). Copies, not
// pointers: the rule names live in a RuleList the store may delete on a swap.
constexpr int MAX_LOCKED = 512;
constexpr int LOCKED_NAME = 48;
char g_locked[MAX_LOCKED][LOCKED_NAME];
int  g_locked_count = 0;

const char* cv_str(void* cv, int off) {
    const char* s = *(const char**)((char*)cv + off);
    return s ? s : "";
}

int cv_int(const char* name, int def) {
    void* cv = rs_find(name);
    if (!cv) return def;
    return *(int*)((char*)cv + COMP_CVAR_INT);
}

void lock_rom(void* cv, const char* name) {
    uint32_t* flags = (uint32_t*)((char*)cv + CV_FLAGS);
    if (*flags & CV_ROM) return;
    *flags |= CV_ROM;
    if (g_locked_count < MAX_LOCKED) {
        snprintf(g_locked[g_locked_count], LOCKED_NAME, "%s", name);
        ++g_locked_count;
    }
}

void unlock_all() {
    for (int i = 0; i < g_locked_count; ++i) {
        void* cv = rs_find(g_locked[i]);
        if (cv) *(uint32_t*)((char*)cv + CV_FLAGS) &= ~CV_ROM;
    }
    g_locked_count = 0;
}

// release one lock (the server's competitive.cfg just took that cvar over, live)
void unlock_one(void* cv, const char* name) {
    for (int i = 0; i < g_locked_count; ++i) {
        if (_stricmp(g_locked[i], name)) continue;
        *(uint32_t*)((char*)cv + CV_FLAGS) &= ~CV_ROM;
        memcpy(g_locked[i], g_locked[--g_locked_count], LOCKED_NAME);
        return;
    }
}

// Cvars the MOD governs itself, with its own published policy - the downloaded list must
// not fight it. cg_fov: the CoDBase list says "IN 80" (exactly 80), while 1.6X allows
// 80..95 and clamps it live in settings_menu_tick. Without this the ruleset pushed cg_fov
// back to 80 four times a second on any server publishing a competitive.cfg, while /devmap
// (ruleset inactive) let 95 through - enzo, 2026-09-18.
// Precedence stays: the server's competitive.cfg > this list > the downloaded ruleset. A
// server that really wants 80 only has to name cg_fov in its competitive.cfg.
bool mod_owns(const char* name) {
    static const char* const kOwned[] = { "cg_fov" };
    for (size_t i = 0; i < sizeof(kOwned) / sizeof(kOwned[0]); ++i)
        if (!_stricmp(name, kOwned[i])) return true;
    return false;
}

void publish(const char* report) {
    if (!strcmp(report, g_report)) return;
    snprintf(g_report, sizeof(g_report), "%s", report);
    rs_set("cod1x_rs", g_report);        // force=1: bypasses our own ROM bit
    logger::logf("ruleset: report -> \"%s\"", g_report);
}

}  // namespace

bool        ruleset_active() { return g_active; }
const char* ruleset_id()     { return RULESET_ID; }

void ruleset_tick() {
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return;
    if (*(volatile int*)CODMP_CVAR_COUNT_VA <= 0) return;

    if (!g_registered) {
        g_registered = true;
        rs_get("sv_competitive_ruleset", "", 0);                       // systeminfo lands here
        rs_get("cod1x_rs", "", CVAR_USERINFO | CV_ROM);               // our verdict, read by the server
        ruleset_store_init();                                         // embedded + cache + start-up refresh
    }

    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (last != 0 && (now - last) < 250) return;
    last = now;

    // the id comes from the CURRENT server's systeminfo, never from the mirrored cvar
    // (which keeps the previous server's value - see systeminfo_value in competitive.cpp)
    char want[64];
    if (!systeminfo_value("sv_competitive_ruleset", want, sizeof(want))) want[0] = 0;
    std::string base;
    int want_version = 0;
    ruleset_split_id(want, base, want_version);
    const bool ingame = cv_int("cl_ingame", 0) != 0;
    ruleset_store_tick();                                             // periodic GitHub refresh

    // A server whose cod1plus.so predates sv_competitive_ruleset names nothing, but it
    // still pushes its competitive.cfg as sv_competitive - and a server that enforces a
    // competitive.cfg is a competitive server: enforce the default list there too. So
    // this needs no server update; an updated server can still pick another id, a
    // version, or 0.
    bool defaulted = false;
    if (base.empty() && ingame) {
        char spec[8];
        if (systeminfo_value("sv_competitive", spec, sizeof(spec)) && spec[0]) {
            base = RULESET_ID;
            defaulted = true;
        }
    }

    // best list we hold for that id (lock held while non-null); also triggers the download
    const RuleList* rl = (ingame && !base.empty()) ? ruleset_store_acquire(base.c_str(), want_version) : nullptr;
    const bool active = rl != nullptr;

    if (!active) {
        if (g_active) {
            const int released = g_locked_count;
            unlock_all();
            g_fixed.clear();
            g_persist.clear();
            g_engine_owned.clear();
            g_list = nullptr;
            logger::logf("ruleset: inactive (%s) -> %d lock(s) released",
                         !ingame ? "not in game" : (!want[0] ? "server publishes no ruleset" : "ruleset id not available yet"),
                         released);
            g_active = false;
        }
        if (want[0] && ingame) {
            static char last_unknown[64] = "";
            if (strcmp(last_unknown, want)) {
                snprintf(last_unknown, sizeof(last_unknown), "%s", want);
                logger::logf("ruleset: server asks for \"%s\", no such list here yet (embedded %s v%d) - downloading",
                             want, RULESET_ID, RULESET_VERSION);
            }
            char rep[96];
            snprintf(rep, sizeof(rep), "%s:missing", want);
            publish(rep);
        } else {
            publish("");
        }
        return;
    }

    // the list was swapped (download landed) -> start the per-rule memory over
    if (g_list != rl || g_fixed.size() != rl->rules.size()) {
        if (g_list) unlock_all();
        g_list = rl;
        g_fixed.assign(rl->rules.size(), 0);
        g_persist.assign(rl->rules.size(), 0);
        g_engine_owned.assign(rl->rules.size(), 0);
        logger::logf("ruleset: %s - enforcing %s v%d (%s, %d rules) on this server%s%s",
                     g_active ? "list updated" : "active", rl->id.c_str(), rl->version, rl->source.c_str(),
                     (int)rl->rules.size(),
                     (want_version > rl->version) ? " - server asks a newer version, download pending" : "",
                     defaulted ? " (server names no ruleset but pushes a competitive.cfg: default list)" : "");
        g_active = true;
    }

    int persistent = 0, probes = 0, fixed_now = 0;
    const char* first_v[2] = { nullptr, nullptr };
    const char* first_p = nullptr;
    char fixbuf[128];

    for (size_t i = 0; i < rl->rules.size(); ++i) {
        const RuleDef& r = rl->rules[i];
        if (r.mode == RM_PROBE) {
            if (rs_find(r.cvar)) {
                ++probes;
                if (!first_p) first_p = r.cvar;
            }
            continue;
        }
        void* cv = rs_find(r.cvar);
        if (!cv) continue;                       // not on this client build: nothing to hold
        if (competitive_spec_has(r.cvar)) {      // the server's competitive.cfg wins, always:
            unlock_one(cv, r.cvar);              // drop our lock so its range stays usable
            g_fixed[i] = 0;
            continue;
        }
        if (mod_owns(r.cvar)) {                  // 1.6X policy beats the list (see mod_owns)
            unlock_one(cv, r.cvar);
            g_fixed[i] = 0;
            continue;
        }

        const uint32_t flags = *(uint32_t*)((char*)cv + CV_FLAGS);
        const bool cheat = r.cheat || (flags & CV_CHEAT);   // the engine resets those itself
        const char* value = cv_str(cv, CV_STRING);
        bool ok = rule_ok(r, value);
        if (!ok && (flags & CV_LATCH)) {
            const char* latched = cv_str(cv, CV_LATCHED);
            if (latched[0] && rule_ok(r, latched)) ok = true;   // takes effect at vid_restart
        }
        if (ok) {
            if (r.mode == RM_EXACT && !cheat) lock_rom(cv, r.cvar);
            continue;
        }
        if (g_engine_owned[i]) continue;         // see below: the engine keeps resetting it
        const char* def = cv_str(cv, CV_RESET);
        if (g_fixed[i] && !strcmp(value, def)) {
            // We set it, and it is back at its ENGINE DEFAULT: that is the engine's own
            // validator (the renderer resets r_shownormals / r_xdebug to "" and prints a
            // usage line on any value it cannot parse - fighting it spams the console) or
            // sv_cheats putting a CHEAT cvar back. A default value is not a cheat: leave
            // that cvar to the engine for the rest of the session, no violation counted.
            g_engine_owned[i] = 1;
            unlock_one(cv, r.cvar);
            logger::logf("ruleset: %s comes back to its default \"%s\" after every set - left to the engine",
                         r.cvar, def);
            continue;
        }
        if (!rule_fix(r, value, def, fixbuf, sizeof(fixbuf))) continue;
        rs_set(r.cvar, fixbuf);                  // force=1
        ++fixed_now;
        // PERSISTENT = an EXACT rule we ROM-locked found changed again to a non-default
        // value: the console cannot do that (ROM), the engine does not (default case
        // above) - only something bypassing the lock. Ranges are never counted: a
        // player retyping a value in the console is clamped, not accused.
        if (g_fixed[i] && r.mode == RM_EXACT && !cheat) {
            if (g_persist[i] < 65535) ++g_persist[i];
            ++persistent;
            if (!first_v[0]) first_v[0] = r.cvar;
            else if (!first_v[1] && first_v[0] != r.cvar) first_v[1] = r.cvar;
        }
        g_fixed[i] = 1;
        if (r.mode == RM_EXACT && !cheat) lock_rom(cv, r.cvar);
    }

    static int logged_fixes = 0;
    if (fixed_now && logged_fixes < 5) {
        ++logged_fixes;
        logger::logf("ruleset: %d cvar(s) brought back into rule this pass", fixed_now);
    }

    char idv[80];
    snprintf(idv, sizeof(idv), "%s@%d", rl->id.c_str(), rl->version);
    char rep[96];
    if (persistent) {
        snprintf(rep, sizeof(rep), "%s:v%d:%s%s%s", idv, persistent,
                 first_v[0] ? first_v[0] : "", first_v[1] ? "," : "", first_v[1] ? first_v[1] : "");
    } else if (probes) {
        snprintf(rep, sizeof(rep), "%s:p%d:%s", idv, probes, first_p ? first_p : "");
    } else {
        snprintf(rep, sizeof(rep), "%s:ok", idv);
    }
    ruleset_store_release();
    publish(rep);
}

}  // namespace patches
