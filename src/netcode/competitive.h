#ifndef COD1RELOADED_COMPETITIVE_H
#define COD1RELOADED_COMPETITIVE_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// Competitive netconfig = a real 40-tick experience.
//
// CoD1 is tuned for sv_fps 20. On an sv_fps 40 server, vanilla clients still cap
// snaps at 30 (server floors snapshotMsec = 1000/snaps at 33ms) and cl_maxpackets
// at 100, so the extra 25ms snapshots are skipped and the server-computed scoreboard
// ping (SV_CalcPings = average snapshot send->ack time, VA 0x45fd90) inflates. Lifting
// the caps and forcing snaps/cl_maxpackets/rate lets delivery reach 40/s, so each
// player's reported ping reflects their real link instead of the tickrate mismatch.
//
// All VAs are in CoDMP.exe (image base 0x400000), RE-confirmed 2026-07-02.
//
// NOTE on where each patch takes effect (CoDMP.exe is both client and dedicated server):
//   - snaps cap  = SERVER-side (SV_UserinfoChanged sets client->snapshotMsec). It only
//     matters on the machine acting as server; inert on a pure client. Run the mod on
//     the (Windows) dedicated/listen server too. Linux cod_lnxded = separate RE.
//   - cl_maxpackets cap = CLIENT-side (CL_WritePacket). Matters on each player's client.
//   - forcing snaps/rate (userinfo) makes the client actually REQUEST 40 (a lifted cap
//     alone does nothing if the client keeps requesting 20).

// snaps max clamp in SV_UserinfoChanged: "cmp ecx,0x1e" / "mov ecx,0x1e" -> 0x28 (40).
// Both the compare and the assignment must move together.
constexpr uintptr_t CODMP_SNAPS_CAP_CMP_VA  = 0x459266; // imm of "cmp ecx,0x1e"
constexpr uintptr_t CODMP_SNAPS_CAP_MOV_VA  = 0x45926a; // imm of "mov ecx,0x1e"
constexpr uint8_t   CODMP_SNAPS_CAP_OLD     = 0x1e;     // 30

// cl_maxpackets upper clamp in CL_WritePacket: "cmp eax,0x64" -> 0x7d (125).
constexpr uintptr_t CODMP_MAXPACKETS_CAP_VA = 0x40c51d; // imm of "cmp eax,0x64"
constexpr uint8_t   CODMP_MAXPACKETS_OLD    = 0x64;     // 100

// cvar_t* __cdecl Cvar_Set(const char* name, const char* value)  (force=qtrue thunk:
// "mov eax,1; jmp Cvar_Set2@0x43bbb0")
constexpr uintptr_t CODMP_CVAR_SET_VA       = 0x43bea0;
// cvar_t* __cdecl Cvar_FindVar(const char* name)
constexpr uintptr_t CODMP_CVAR_FINDVAR_VA2  = 0x43b790;

// ---- cvar LOCK (the CoD1 equivalent of cod2x's dvar limit clamping) ----------------
// CoD2 dvar_t carries limits.integer.min/max, so cod2x just narrows them and the engine
// refuses out-of-range values. CoD1's cvar_t is the plain Q3 one (name+0, string+4,
// flags+0x10, value+0x1c, integer+0x20, stride 0x2c) - NO limits field. The equivalent
// lever is CVAR_ROM: Cvar_Set2 tests "al,0x40" @0x43bcd8 and bails with
// "%s is read only." - so a ROM cvar cannot be changed by the player's console/config.
// Crucially the check is skipped when force!=0 ("cmp edi,esi; jne 0x43be17" @0x43bcd2),
// and Cvar_Set passes force=1, so WE keep full write access while the player is locked.
constexpr uint32_t COMP_CVAR_ROM   = 0x40;   // "is read only"      (engine rejects set)
constexpr uint32_t COMP_CVAR_INIT  = 0x10;   // "is write protected"
constexpr int      COMP_CVAR_STRING = 0x04;  // char* cvar->string
constexpr int      COMP_CVAR_FLAGS = 0x10;
constexpr int      COMP_CVAR_INT   = 0x20;

// CVAR_SYSTEMINFO: the server broadcasts these cvars to every client (systeminfo
// configstring) and the client engine SETS them locally - this is exactly how
// sv_cheats reaches clients. CONFIRMED in CoDMP.exe: sv_cheats is registered with
// flags imm8 0x48 = SYSTEMINFO(0x08)|ROM(0x40) at 3 sites (0x43c080, 0x45daa1,
// 0x4be1e1). So a server-set "g_competitive" carrying 0x08 propagates to all clients,
// which is cod2x's DVAR_SYSTEMINFO model, natively supported by CoD1.
constexpr uint32_t COMP_CVAR_SYSTEMINFO = 0x08;

struct CompetitiveConfig {
    bool enable;         // fallback when the server sends no g_competitive signal
    int  snaps;          // force snaps=          (<=0 = don't force)
    int  cl_maxpackets;  // force cl_maxpackets=  (<=0 = don't force)
    int  rate;           // force rate=           (<=0 = don't force)
    // cod2x parity: lock the cvars so the player cannot change them at all.
    bool lock_cvars;     // ROM-lock the forced cvars (competitive_lock_cvars)
    int  maxfps_min;     // com_maxfps allowed range, cod2x = 125..250 (0 = don't touch)
    int  maxfps_max;
};

extern CompetitiveConfig g_competitive_config;

bool apply_competitive_caps();   // byte-patch the snaps/cl_maxpackets caps (once, at attach)
void competitive_force_cvars();  // enforce + ROM-lock the netconfig cvars (polled)

}  // namespace patches

#endif
