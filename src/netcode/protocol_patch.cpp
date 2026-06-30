#include "netcode/protocol_patch.h"
#include "core/logger.h"

#include <cstring>
#include <cstdio>

namespace patches {

int  g_protocol_version = 10;
char g_master_host[CODMP_MASTER_HOST_MAX + 1] = "87.106.7.52";
int  g_net_version = 16; // "1.6"

namespace {
void poke(uintptr_t va, uint8_t v) {
    BYTE* p = (BYTE*)va;
    DWORD old = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    *p = v;
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
}
}  // namespace

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

    poke(CODMP_PROTO_SERVER_CMP_VA,  pv);
    poke(CODMP_PROTO_CLIENT_EMIT_VA, pv);
    poke(CODMP_PROTO_GETINFO_VA,     pv);
    poke(CODMP_PROTO_BOT_VA,         pv);
    poke(CODMP_PROTO_REJECTMSG_VA,   pv);
    logger::logf("  protocol_patch: protocol -> %d", g_protocol_version);

    if (g_master_host[0]) {
        char* dst = (char*)CODMP_MASTER_HOST_VA;
        DWORD old = 0;
        if (VirtualProtect(dst, CODMP_MASTER_HOST_MAX + 1, PAGE_READWRITE, &old)) {
            memset(dst, 0, CODMP_MASTER_HOST_MAX + 1);
            strncpy(dst, g_master_host, CODMP_MASTER_HOST_MAX);
            VirtualProtect(dst, CODMP_MASTER_HOST_MAX + 1, old, &old);
        }
        // resolver caches netadr once; zero it so our host is resolved (it's 0 at startup anyway)
        DWORD old2 = 0;
        BYTE* cache = (BYTE*)CODMP_MASTER_CACHE_VA;
        if (VirtualProtect(cache, 4, PAGE_READWRITE, &old2)) {
            *(uint32_t*)cache = 0;
            VirtualProtect(cache, 4, old2, &old2);
        }
        logger::logf("  protocol_patch: master -> \"%s\"", g_master_host);
    }

    // Server browser: ask the master for our protocol, and stop the list from
    // filtering out servers that advertise it.
    {
        char* fmt = (char*)CODMP_GETSERVERS_FMT_VA;
        if (memcmp(fmt, "getservers ", 11) == 0) {
            char req[CODMP_GETSERVERS_FMT_LEN + 1];
            int n = snprintf(req, sizeof(req), "getservers %d", g_protocol_version);
            if (n > 0 && (size_t)n < CODMP_GETSERVERS_FMT_LEN) {
                DWORD old = 0;
                if (VirtualProtect(fmt, CODMP_GETSERVERS_FMT_LEN, PAGE_READWRITE, &old)) {
                    memset(fmt, 0, CODMP_GETSERVERS_FMT_LEN);
                    memcpy(fmt, req, n);
                    VirtualProtect(fmt, CODMP_GETSERVERS_FMT_LEN, old, &old);
                    logger::logf("  protocol_patch: browser request -> \"%s\"", req);
                }
            } else {
                logger::logf("  protocol_patch: getservers req too long for proto %d, skip",
                             g_protocol_version);
            }
        }

        float* pf = (float*)CODMP_BROWSER_PROTO_FLT_VA;
        const float want = (float)g_protocol_version;
        if (*pf == 6.0f && want != 6.0f) {
            DWORD old = 0;
            if (VirtualProtect(pf, 4, PAGE_READWRITE, &old)) {
                *pf = want;
                VirtualProtect(pf, 4, old, &old);
                logger::logf("  protocol_patch: browser proto filter 6 -> %d", g_protocol_version);
            }
        } else if (*pf != want) {
            logger::logf("  protocol_patch: browser float @0x%lx unexpected, skip",
                         (unsigned long)CODMP_BROWSER_PROTO_FLT_VA);
        }
    }
    return true;
}

// USERINFO cvar "cod1reloaded" = our net version; engine ships it in the connect
// userinfo so the server can verify the client patch (see ClientConnect gate).
void register_client_version_cvar() {
    static bool done = false;
    if (done) return;
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return;

    typedef void* (__cdecl *Cvar_Get_t)(const char*, const char*, int);
    Cvar_Get_t Cvar_Get = (Cvar_Get_t)CODMP_CVAR_GET_VA;

    char ver[16];
    snprintf(ver, sizeof(ver), "%d", g_net_version);
    Cvar_Get("cod1reloaded", ver, CVAR_USERINFO | CVAR_ROM);

    done = true;
    logger::logf("  version_gate: client userinfo cvar cod1reloaded=%s", ver);
}

}  // namespace patches
