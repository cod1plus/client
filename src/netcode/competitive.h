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

// cvar_t* __cdecl Cvar_Set(const char* name, const char* value)  (force=qtrue thunk)
constexpr uintptr_t CODMP_CVAR_SET_VA       = 0x43bea0;

struct CompetitiveConfig {
    bool enable;         // master switch: lift caps + force the netconfig cvars
    int  snaps;          // force snaps=          (<=0 = don't force)
    int  cl_maxpackets;  // force cl_maxpackets=  (<=0 = don't force)
    int  rate;           // force rate=           (<=0 = don't force)
};

extern CompetitiveConfig g_competitive_config;

bool apply_competitive_caps();   // byte-patch the snaps/cl_maxpackets caps (once, at attach)
void competitive_force_cvars();  // Cvar_Set snaps/cl_maxpackets/rate (throttled re-lock)

}  // namespace patches

#endif
