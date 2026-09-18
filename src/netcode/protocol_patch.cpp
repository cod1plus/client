#include "netcode/protocol_patch.h"
#include "core/logger.h"
#include "features/updater.h"        // COD1RELOADED_VERSION - the single source of truth
#include "features/settings_menu.h"  // CODMP_CVAR_FINDVAR_VA, CODMP_CBUF_EXECTEXT_VA
#include "netcode/competitive.h"     // CODMP_CVAR_SET_VA

#include <cstring>
#include <cstdio>

namespace patches {

int  g_protocol_version = 10;
char g_master_host[CODMP_MASTER_HOST_MAX + 1] = "87.106.7.52";
int  g_net_version = 16; // "1.6"

namespace {

int g_netmode = 0;   // 0 = 1.6 ecosystem, 1 = legacy 1.5

void poke(uintptr_t va, uint8_t v) {
    BYTE* p = (BYTE*)va;
    DWORD old = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    *p = v;
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
}

void poke_master_host(const char* host) {
    if (!host || !host[0]) return;
    char* dst = (char*)CODMP_MASTER_HOST_VA;
    DWORD old = 0;
    if (VirtualProtect(dst, CODMP_MASTER_HOST_MAX + 1, PAGE_READWRITE, &old)) {
        memset(dst, 0, CODMP_MASTER_HOST_MAX + 1);
        strncpy(dst, host, CODMP_MASTER_HOST_MAX);
        VirtualProtect(dst, CODMP_MASTER_HOST_MAX + 1, old, &old);
    }
    // The resolver caches the netadr on first use. Zero it or a switch would keep
    // querying the previous master's address with the new protocol.
    DWORD old2 = 0;
    BYTE* cache = (BYTE*)CODMP_MASTER_CACHE_VA;
    if (VirtualProtect(cache, 4, PAGE_READWRITE, &old2)) {
        *(uint32_t*)cache = 0;
        VirtualProtect(cache, 4, old2, &old2);
    }
}

// Everything that defines "which ecosystem am I talking to". Re-runnable at any time:
// the five protocol immediates, the master hostname, and the two server-browser knobs
// (what we ask the master for, and what the list refuses to display).
void poke_netmode(int proto, const char* master) {
    const uint8_t pv = (uint8_t)proto;
    poke(CODMP_PROTO_SERVER_CMP_VA,  pv);
    poke(CODMP_PROTO_CLIENT_EMIT_VA, pv);
    poke(CODMP_PROTO_GETINFO_VA,     pv);
    poke(CODMP_PROTO_BOT_VA,         pv);
    poke(CODMP_PROTO_REJECTMSG_VA,   pv);

    poke_master_host(master);

    char* fmt = (char*)CODMP_GETSERVERS_FMT_VA;
    if (memcmp(fmt, "getservers ", 11) == 0) {
        char req[CODMP_GETSERVERS_FMT_LEN + 1];
        const int n = snprintf(req, sizeof(req), "getservers %d", proto);
        if (n > 0 && (size_t)n < CODMP_GETSERVERS_FMT_LEN) {
            DWORD old = 0;
            if (VirtualProtect(fmt, CODMP_GETSERVERS_FMT_LEN, PAGE_READWRITE, &old)) {
                memset(fmt, 0, CODMP_GETSERVERS_FMT_LEN);
                memcpy(fmt, req, n);
                VirtualProtect(fmt, CODMP_GETSERVERS_FMT_LEN, old, &old);
            }
        } else {
            logger::logf("  protocol_patch: getservers req too long for proto %d, skip", proto);
        }
    }

    // The browser drops any server whose advertised protocol differs from this float.
    // Set it unconditionally: it has to move in BOTH directions now, so the old
    // "only patch when it still reads 6.0" guard would have made 1.6 -> 1.5 a no-op.
    float* pf = (float*)CODMP_BROWSER_PROTO_FLT_VA;
    if (*pf != (float)proto) {
        DWORD old = 0;
        if (VirtualProtect(pf, 4, PAGE_READWRITE, &old)) {
            *pf = (float)proto;
            VirtualProtect(pf, 4, old, &old);
        }
    }

    logger::logf("  protocol_patch: protocol %d, master \"%s\", browser filter %d",
                 proto, master ? master : "(unchanged)", proto);
}

}  // namespace

int netmode_current() { return g_netmode; }

bool apply_protocol_patch() {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (base != 0x400000) {
        logger::logf("  protocol_patch: CoDMP.exe base 0x%08x != 0x400000, abort", (unsigned)base);
        return false;
    }

    const uint8_t cur = *(const uint8_t*)CODMP_PROTO_SERVER_CMP_VA;
    const uint8_t pv  = (uint8_t)g_protocol_version;
    if (cur != 0x06 && cur != pv) {
        logger::logf("  protocol_patch: byte @0x%lx = 0x%02x (expected 0x06), abort",
                     (unsigned long)CODMP_PROTO_SERVER_CMP_VA, cur);
        return false;
    }

    g_netmode = 0;
    poke_netmode(g_protocol_version, g_master_host);
    return true;
}

// Follows cod1x_masterlist (0 = 1.6, 1 = legacy 1.5). Deliberately NOT archived: a
// competitive client must come back on the 1.6 list every launch, or a player who
// browsed 1.5 once would find the match servers gone and not know why.
void netmode_tick() {
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return;
    if (*(volatile int*)CODMP_CVAR_COUNT_VA <= 0) return;

    typedef void* (__cdecl *Cvar_Get_t)(const char*, const char*, int);
    typedef void* (__cdecl *Cvar_FindVar_t)(const char*);
    static const Cvar_Get_t     Cvar_Get     = (Cvar_Get_t)CODMP_CVAR_GET_VA;
    static const Cvar_FindVar_t Cvar_FindVar = (Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA;

    static bool registered = false;
    if (!registered) {
        Cvar_Get("cod1x_masterlist", "0", 0);
        Cvar_Get("cod1x_masterlist_label", "1.6 SERVERS", 0);
        registered = true;
    }

    void* cv = Cvar_FindVar("cod1x_masterlist");
    if (!cv) return;
    const int want = (*(int*)((char*)cv + 0x20) != 0) ? 1 : 0;   // cvar_t integer @0x20
    if (want == g_netmode) return;

    g_netmode = want;
    const int proto = (want == 1) ? PROTOCOL_LEGACY_15 : g_protocol_version;
    if (want == 1) {
        logger::logf("protocol_patch: switching to the LEGACY 1.5 list");
        poke_netmode(proto, CODMP_MASTER_HOST_LEGACY);
    } else {
        logger::logf("protocol_patch: switching back to the 1.6 list");
        poke_netmode(proto, g_master_host);
    }

    // Re-query here rather than from the menu button. The button runs in the same
    // frame it sets the cvar, so a `uiScript RefreshServers` on it would go out with
    // the OLD protocol and master still poked in - the very race this avoids.
    typedef void (__cdecl *Cbuf_ExecuteText_t)(int, const char*);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "globalservers 0 %d\n", proto);
    ((Cbuf_ExecuteText_t)CODMP_CBUF_EXECTEXT_VA)(2 /* EXEC_APPEND */, cmd);

    // Label for the menu button, so one item can show which list is active.
    typedef void* (__cdecl *Cvar_Set_t)(const char*, const char*);
    ((Cvar_Set_t)CODMP_CVAR_SET_VA)("cod1x_masterlist_label",
                                    want == 1 ? "1.5 SERVERS" : "1.6 SERVERS");
}

// Turn "1.6.2" into 10602 - a single integer the server can compare with <.
// Two digits per component, so 1.6.2 < 1.6.10 < 1.7.0 all order correctly.
static int build_number_from(const char* v) {
    int part[3] = {0, 0, 0};
    int i = 0;
    for (const char* p = v; *p && i < 3; ) {
        while (*p >= '0' && *p <= '9') { part[i] = part[i] * 10 + (*p - '0'); ++p; }
        if (*p == '.') { ++p; ++i; } else break;
    }
    return part[0] * 10000 + part[1] * 100 + part[2];
}

// Two USERINFO cvars, both ROM so a player cannot fake them from his console:
//   cod1reloaded = net version (16 = "1.6"). Coarse: "is this a 1.6 client at all".
//                  Never changes between releases.
//   cod1x_build  = THIS build, e.g. 10602 for 1.6.2. What lets a server require an
//                  up-to-date client instead of merely a patched one - without it a
//                  player left on an old release connects exactly like a current one.
void register_client_version_cvar() {
    static bool done = false;
    if (done) return;
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return;

    typedef void* (__cdecl *Cvar_Get_t)(const char*, const char*, int);
    Cvar_Get_t Cvar_Get = (Cvar_Get_t)CODMP_CVAR_GET_VA;

    char ver[16];
    snprintf(ver, sizeof(ver), "%d", g_net_version);
    Cvar_Get("cod1reloaded", ver, CVAR_USERINFO | CVAR_ROM);

    char build[16];
    snprintf(build, sizeof(build), "%d", build_number_from(COD1RELOADED_VERSION));
    Cvar_Get("cod1x_build", build, CVAR_USERINFO | CVAR_ROM);

    done = true;
    logger::logf("  version_gate: client userinfo cod1reloaded=%s cod1x_build=%s (%s)",
                 ver, build, COD1RELOADED_VERSION);
}

}  // namespace patches
