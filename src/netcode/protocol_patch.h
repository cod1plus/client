#ifndef COD1RELOADED_PROTOCOL_PATCH_H
#define COD1RELOADED_PROTOCOL_PATCH_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// CoDMP.exe (base 0x400000). Network protocol is the imm byte 0x06 at these VAs.
// 6 -> 10 hard-separates us from vanilla CoD1 (proto 6).
constexpr uintptr_t CODMP_PROTO_SERVER_CMP_VA  = 0x4573f8; // SVC_DirectConnect: cmp $6 (gatekeeper)
constexpr uintptr_t CODMP_PROTO_CLIENT_EMIT_VA = 0x45d96e; // client connect userinfo protocol=6
constexpr uintptr_t CODMP_PROTO_GETINFO_VA     = 0x45eb57; // getinfo/infoResponse advertise
constexpr uintptr_t CODMP_PROTO_BOT_VA         = 0x411148; // bot userinfo
constexpr uintptr_t CODMP_PROTO_REJECTMSG_VA   = 0x457423; // "(should be 6)" cosmetic

// Master hostname string (operative copy, refs in resolver/heartbeat/getservers).
// Original "codmaster.activision.com" = 24 chars; overwrite in place.
constexpr uintptr_t CODMP_MASTER_HOST_VA  = 0x5a5568;
constexpr uintptr_t CODMP_MASTER_CACHE_VA = 0x93b110; // cached netadr; zero to force re-resolve
constexpr size_t    CODMP_MASTER_HOST_MAX = 24;

// Server browser. "getservers %s" (14-byte field) = the master query the Internet
// browser sends; CL_ServerInfoPacket filters the list by a client protocol float
// (default 6.0) and drops servers whose advertised protocol differs.
constexpr uintptr_t CODMP_GETSERVERS_FMT_VA    = 0x5a5558;
constexpr size_t    CODMP_GETSERVERS_FMT_LEN   = 14;     // "getservers %s\0"
constexpr uintptr_t CODMP_BROWSER_PROTO_FLT_VA = 0x5a88b4;

extern int  g_protocol_version;                 // ini: protocol_version (default 10)
extern char g_master_host[CODMP_MASTER_HOST_MAX + 1]; // ini: master_server
extern int  g_net_version;                      // ini: net_version (16 = "1.6"); client cvar + server min

// CoD1 Cvar API (CoDMP.exe @0x400000), RE'd:
constexpr uintptr_t CODMP_CVAR_GET_VA   = 0x0043b880; // cvar_t* Cvar_Get(name, def, flags) cdecl
constexpr uintptr_t CODMP_CVAR_COUNT_VA = 0x01912aec; // cvar count (>0 once cvar system is up)
constexpr int       CVAR_USERINFO       = 0x01;
constexpr int       CVAR_ROM            = 0x40;

bool apply_protocol_patch();        // protocol bump + master repoint in CoDMP.exe
void register_client_version_cvar(); // register USERINFO "cod1reloaded" cvar (once, after Com_Init)

}  // namespace patches

#endif
