#ifndef COD1RELOADED_SWING_FIX_H
#define COD1RELOADED_SWING_FIX_H

#include <windows.h>
#include <stdint.h>

// cod2x BG_PlayerAngles swing fixes: model follows the camera without lag.

namespace patches {

// cgame_mp_x86.dll push imm32 RVAs: 0x451d=40.0 (legs tol), 0x459d=0.15 (torso pitch speed)
constexpr uintptr_t CGAME_SWING_LEGS_TOLERANCE_PUSH_RVA  = 0x451d;
constexpr uintptr_t CGAME_SWING_TORSO_PITCH_SPEED_PUSH_RVA = 0x459d;

// torso yaw swingSpeed dvar: mov esi,ds:0x301dd388 @0x4468, redirect operand @0x446a
constexpr uintptr_t CGAME_SWING_TORSO_YAW_MOV_RVA  = 0x4468;
constexpr uintptr_t CGAME_SWING_TORSO_YAW_DVAR_RVA = 0x1dd388;
extern "C" float g_torso_yaw_speed_live;

// torso yaw movementYaw*0.3 + view: fmul ds:0x3006b6f8 @0x444b, redirect operand @0x444d
constexpr uintptr_t CGAME_SWING_TORSO_YAW_MOVEFRAC_FMUL_RVA  = 0x444d;
constexpr uintptr_t CGAME_SWING_TORSO_YAW_MOVEFRAC_CONST_RVA = 0x6b6f8;
extern "C" float g_torso_yaw_movefrac_live;

struct SwingFixConfig {
    bool  enable;
    float legs_tolerance;     // 40 -> 0
    float torso_pitch_speed;  // 0.15 -> 1.0
};

extern SwingFixConfig g_swing_fix_config;

bool apply_swing_fix(HMODULE cgame_module);  // idempotent; no-op on DLL mismatch

}  // namespace patches

#endif
