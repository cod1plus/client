// Server-side client version gate. The connect protocol is already bumped to 10
// (protocol_patch) so only cod1reloaded clients reach ClientConnect; this enforces
// a MINIMUM client version via the "cod1reloaded" userinfo cvar.

#include "netcode/version_gate.h"
#include "netcode/protocol_patch.h"  // g_net_version
#include "netcode/version_patch.h"   // g_short_version_buffer
#include "core/logger.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <stdint.h>

namespace patches {

VersionGateConfig g_version_gate_config;

namespace {

// game_mp_x86.dll RVAs (imagebase 0x20000000), RE'd.
constexpr uintptr_t RVA_CLIENTCONNECT = 0x1b130; // char* ClientConnect(int,int) cdecl
constexpr uintptr_t RVA_CC_CALLSITE   = 0x2741a; // E8 rel32 -> ClientConnect (vmMain cmd 4)
constexpr uintptr_t RVA_SYSCALL_PTR   = 0x6d684; // *(void**) engine syscall fn ptr
constexpr int       G_GET_USERINFO    = 0x21;

uintptr_t g_game_base = 0;
char* (__cdecl *g_clientconnect_orig)(int, int) = nullptr;
char g_reject_msg[160] = "This server requires a newer cod1reloaded.";

// userinfo = "\key\val\key\val..."; return atoi of key's value, 0 if absent.
int info_value_int(const char* info, const char* key) {
    const size_t klen = strlen(key);
    const char* p = info;
    while (*p == '\\') {
        const char* k = p + 1;
        const char* ke = strchr(k, '\\');
        if (!ke) break;
        const char* v = ke + 1;
        const char* ve = strchr(v, '\\');
        const size_t vlen = ve ? (size_t)(ve - v) : strlen(v);
        if ((size_t)(ke - k) == klen && strncmp(k, key, klen) == 0) {
            char buf[32];
            const size_t n = vlen < 31 ? vlen : 31;
            memcpy(buf, v, n);
            buf[n] = '\0';
            return atoi(buf);
        }
        if (!ve) break;
        p = ve;
    }
    return 0;
}

char* __cdecl clientconnect_wrapper(int clientNum, int firstTime) {
    typedef int (__cdecl *syscall_t)(int, ...);
    syscall_t G_syscall = *(syscall_t*)(g_game_base + RVA_SYSCALL_PTR);

    char userinfo[1024];
    userinfo[0] = '\0';
    if (G_syscall)
        G_syscall(G_GET_USERINFO, clientNum, (int)(uintptr_t)userinfo, (int)sizeof(userinfo));

    const int cver = info_value_int(userinfo, "cod1reloaded");
    const bool unversioned = (cver == 0);
    if (cver < g_net_version && !(unversioned && g_version_gate_config.allow_unversioned)) {
        logger::logf("version_gate: reject client %d (cod1reloaded=%d < %d)",
                     clientNum, cver, g_net_version);
        return g_reject_msg;
    }
    return g_clientconnect_orig ? g_clientconnect_orig(clientNum, firstTime) : nullptr;
}

bool install_callsite_redirect(uintptr_t base) {
    uint8_t* call = (uint8_t*)(base + RVA_CC_CALLSITE);
    if (*call != 0xE8) {
        logger::logf("version_gate: callsite @+0x%lx not E8 (0x%02x), abort",
                     (unsigned long)RVA_CC_CALLSITE, *call);
        return false;
    }
    const int32_t cur_rel = *(int32_t*)(call + 1);
    const uintptr_t cur_target = (uintptr_t)(call + 5) + cur_rel;
    if (cur_target != base + RVA_CLIENTCONNECT) {
        logger::logf("version_gate: callsite -> 0x%08x != ClientConnect 0x%08x, abort",
                     (unsigned)cur_target, (unsigned)(base + RVA_CLIENTCONNECT));
        return false;
    }
    g_clientconnect_orig = (char* (__cdecl*)(int, int))(base + RVA_CLIENTCONNECT);
    const int32_t new_rel = (int32_t)((uintptr_t)&clientconnect_wrapper - (uintptr_t)(call + 5));
    DWORD old = 0;
    if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t*)(call + 1) = new_rel;
    VirtualProtect(call + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    return true;
}

DWORD WINAPI watch(LPVOID) {
    HMODULE gm = nullptr;
    for (;;) { gm = GetModuleHandleA("game_mp_x86.dll"); if (gm) break; Sleep(20); }
    g_game_base = (uintptr_t)gm;

    snprintf(g_reject_msg, sizeof(g_reject_msg),
             "Update cod1reloaded to %s+ to join this server.", g_short_version_buffer);

    if (install_callsite_redirect(g_game_base))
        logger::logf("version_gate: ClientConnect gate installed (min cod1reloaded=%d)", g_net_version);
    return 0;
}

}  // namespace

void version_gate_start() {
    if (!g_version_gate_config.enable) {
        logger::logf("version_gate: disabled");
        return;
    }
    CreateThread(nullptr, 0, watch, nullptr, 0, nullptr);
}

}  // namespace patches
