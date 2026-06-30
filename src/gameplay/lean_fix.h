#ifndef COD1RELOADED_LEAN_FIX_H
#define COD1RELOADED_LEAN_FIX_H

#include <windows.h>
#include <stdint.h>

namespace patches {

constexpr uintptr_t CGAME_PREFERRED_BASE = 0x30000000;

// RVAs from RE (cgame_mp_x86.dll)
constexpr uintptr_t CGAME_DOCONTROLLERS_INTERNAL_RVA = 0x00004960; // computes the 6 vec3s
constexpr uintptr_t CGAME_DOCONTROLLERS_CALL_SITE_RVA = 0x000051d8; // in CG_Player_DoControllers

constexpr uintptr_t CGAME_OFFSET_FPV_STAND_RVA  = 0x00034180; // CG_OffsetFirstPersonView STAND
constexpr uintptr_t CGAME_OFFSET_FPV_CROUCH_RVA = 0x00038300; // 2nd leanf reader (crouch/prone)
constexpr uintptr_t CGAME_ADD_LEAN_TO_POSITION_RVA = 0x00040df0; // BG_AddLeanToPosition

// lerpLean (0..~5deg) -> signed fLeanFrac [-1,+1]. cdecl, ret in ST(0).
// called by DoControllersInternal at 0x300049c5.
constexpr uintptr_t CGAME_GET_LEAN_FRAC_RVA = 0x0003db10;

constexpr uintptr_t CI_LERPLEAN_OFFSET = 0x3b8;   // clientInfo via ecx, 0x4b0 bytes
// movementYaw (+90=left, 0=fwd, -90=right). swing_fix PATCH4 *0.3 @0x444b.
constexpr uintptr_t CI_MOVEMENTYAW_OFFSET = 0x3e0;

// local player leanf [-1,1]. read by AddLeanToPosition call sites (e.g. 0x34473).
// read-only (snapshot fills it).
constexpr uintptr_t CG_LOCAL_LEANF_RVA = 0x20af54;

constexpr uintptr_t ENT_EFLAGS_OFFSET = 0x8;   // entity via ebx

// clientInfo[entity+0x90] in CG_Player (cgame+0x1e6ae): imul 0x4b0 + base 0x3018dc8c
constexpr uintptr_t ENT_CLIENTNUM_OFFSET = 0x90;

// local clientNum: *(*(cgame+0x1e5f24) + 0xb8) (cgame+0x145f0)
constexpr uintptr_t CGAME_SNAP_PTR_RVA       = 0x1e5f24;
constexpr uintptr_t SNAP_LOCAL_CLIENTNUM_OFF = 0xb8;

// WARNING: bit 0x40 (test al,0x40 @0x300049ba) is NOT a reliable is_leaning
// signal: always 0 in-game even during a visible lean. kept for reference.
constexpr uint32_t ENT_FLAG_LEANING_QUESTIONABLE = 0x40;
constexpr uint32_t ENT_FLAG_CROUCH = 0x4000;
constexpr uint32_t ENT_FLAG_PRONE  = 0x8000;

// controllers buffer offsets (vec3 = 12 bytes). order from bone-name table at
// cgame+0x79670: back_low first, pelvis last. reversing = body upside-down.
constexpr uintptr_t CTRL_BACK_LOW_ANGLES = 0x00;
constexpr uintptr_t CTRL_BACK_MID_ANGLES = 0x0c;
constexpr uintptr_t CTRL_BACK_UP_ANGLES  = 0x18;
constexpr uintptr_t CTRL_NECK_ANGLES     = 0x24;
constexpr uintptr_t CTRL_HEAD_ANGLES     = 0x30;
constexpr uintptr_t CTRL_PELVIS_ANGLES   = 0x3c;

constexpr uintptr_t CTRL_TAG_ORIGIN_ANGLES = 0x48;  // vec3 [pitch, yaw, roll]
constexpr uintptr_t CTRL_TAG_ORIGIN_OFFSET = 0x54;  // vec3 [x, y, z]

struct LeanFixConfig {
    bool  enable;
    bool  apply_in_stand;
    int   diag_log_count;     // log first N invocations (0 = off)

    int   move_diag_fix;      // 0=off, 1=back_low, 2=+back_mid, 3=+back_up
    int   move_diag_parent;   // yawDiff mode (0=cod2x exact)
    float diag_k_pos;         // roll>=0 side (rig not mirrored)
    float diag_k_neg;         // roll<0 side

    float lean_diag_scale;    // 0=off, 1.0=cod2x
    float body_shift_lean_scale;

    float body_yaw_lock;      // 0=straight (rifle), 1.0=vanilla

    bool  ctrl_smooth_enable;
    int   ctrl_smooth_time;   // ms
};

extern LeanFixConfig g_lean_fix_config;

bool install_lean_fix(HMODULE cgame_module);   // idempotent
bool apply_to_cgame(HMODULE cgame_module);
bool cgame_needs_reapply(HMODULE cgame_module); // true if cgame reloaded -> patches reverted

}  // namespace patches

#endif
