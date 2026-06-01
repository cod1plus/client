#ifndef COD1RELOADED_SWING_FIX_H
#define COD1RELOADED_SWING_FIX_H

#include <windows.h>
#include <stdint.h>

// Port des fixes "swing" de cod2x (animation.cpp BG_PlayerAngles).
//
// Cod2x retire la lenteur d'interpolation entre la rotation camera et la
// rotation visible du player model :
//   - bg_swingSpeed force a 1.0 (le legs/torso suit instant la cam)
//   - swingTolerance force a 0 (les legs commencent a tourner direct au
//     lieu d'attendre 40 deg)
//   - torso.yawAngle = playerAngles[1] (on retire le offset
//     `legsYawDirection * 0.3` qui tire le torso vers la direction de
//     mouvement)
//
// Effet attendu : weapon/head pointent EXACTEMENT ou tu vises pendant le
// strafe / lean, plus de lag de rotation. C'est LE fix attendu en
// competitif.

namespace patches {

// RVAs identifies dans cgame_mp_x86.dll (base 0x30000000) par RE des
// constantes utilisees dans BG_PlayerAngles :
//
//   0x451d  : push 40.0f  (legs swingTolerance) -> patch a push 0.0f
//   0x459d  : push 0.15f  (torso pitch swingSpeed immediate) -> push 1.0f
//
// Confirmes via xxd hunt :
//   0x451d byte sequence = 68 00 00 20 42  (push 0x42200000 = 40.0)
//   0x459d byte sequence = 68 9a 99 19 3e  (push 0x3E19999A = 0.15)
constexpr uintptr_t CGAME_SWING_LEGS_TOLERANCE_PUSH_RVA  = 0x451d;
constexpr uintptr_t CGAME_SWING_TORSO_PITCH_SPEED_PUSH_RVA = 0x459d;

struct SwingFixConfig {
    bool enable;
    // Override values (in degrees / float):
    //   legs_tolerance : original 40.0, set to 0.0 for instant legs follow
    //   torso_pitch_speed : original 0.15, set to 1.0 for instant pitch snap
    // Both expose tuning to .ini in case user wants a middle ground.
    float legs_tolerance;
    float torso_pitch_speed;
};

extern SwingFixConfig g_swing_fix_config;

// Patche les immediates dans BG_PlayerAngles. Idempotent.
// Returns true si patch applied successfully, false sinon (sanity check
// echoue = autre version du DLL, on ne touche rien).
bool apply_swing_fix(HMODULE cgame_module);

}  // namespace patches

#endif
