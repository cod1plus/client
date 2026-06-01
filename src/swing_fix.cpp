// Swing fix : remplace les immediates dans BG_PlayerAngles pour rendre le
// player model snap directement a l'angle de la camera (au lieu d'avoir
// un lag de rotation legs/torso).
//
// Port direct cod2x animation.cpp BG_PlayerAngles :
//   - bg_swingSpeed override a 1.0 (vanilla 0.15)
//   - swingTolerance override a 0.0 (vanilla 40.0)
//
// On patche directement les push imm32 dans le code machine du DLL.

#include "swing_fix.h"
#include "logger.h"

#include <cstdio>
#include <cstring>

namespace patches {

SwingFixConfig g_swing_fix_config = {
    /* enable             */ true,
    /* legs_tolerance     */ 0.0f,   // vanilla 40.0, cod2x 0.0
    /* torso_pitch_speed  */ 1.0f,   // vanilla 0.15, cod2x 1.0
};

namespace {
bool g_installed = false;

// Patch un push imm32 (5 bytes : 68 xx xx xx xx) en validant que le byte
// d'opcode est bien 0x68 ET que l'imm32 courant matche un expected_value
// (sanity check : evite de patcher si version DLL differente).
//
// Returns true si patche ou deja patche, false si signature mismatch.
bool patch_push_imm32_float(uintptr_t addr,
                            float expected_value,
                            float new_value,
                            const char* label) {
    const uint8_t opcode = *(const uint8_t*)addr;
    if (opcode != 0x68) {
        logger::logf("  swing_fix [%s] : opcode inattendu 0x%02x a cgame+RVA "
                     "(attendu 0x68 = push imm32). Patch annule.",
                     label, opcode);
        return false;
    }

    // Float stored as 4 bytes after the opcode byte
    const float current = *(const float*)(addr + 1);

    // Tolerance for sanity check : the value might already be our new_value
    // if this is a re-init after vid_restart etc.
    const float diff_expected = current - expected_value;
    const float diff_new      = current - new_value;
    const float abs_diff_expected = diff_expected < 0 ? -diff_expected : diff_expected;
    const float abs_diff_new      = diff_new      < 0 ? -diff_new      : diff_new;

    if (abs_diff_expected > 0.01f && abs_diff_new > 0.01f) {
        logger::logf("  swing_fix [%s] : float value mismatch (got %.4f, "
                     "expected %.4f or already-patched %.4f). Patch annule.",
                     label, current, expected_value, new_value);
        return false;
    }

    if (abs_diff_new < 0.01f) {
        // Already patched
        logger::logf("  swing_fix [%s] : deja patche (value=%.4f)", label, current);
        return true;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect((void*)(addr + 1), 4, PAGE_READWRITE, &old_protect)) {
        logger::logf("  swing_fix [%s] : VirtualProtect failed. Patch annule.", label);
        return false;
    }
    *(float*)(addr + 1) = new_value;
    VirtualProtect((void*)(addr + 1), 4, old_protect, &old_protect);

    logger::logf("  swing_fix [%s] : patched at 0x%08x (%.4f -> %.4f)",
                 label, (unsigned)addr, expected_value, new_value);
    return true;
}
}  // namespace

bool apply_swing_fix(HMODULE cgame_module) {
    if (g_installed) return true;
    if (!cgame_module) return false;
    if (!g_swing_fix_config.enable) {
        logger::logf("  swing_fix : disabled in config, skipping");
        return true;
    }

    const uintptr_t base = (uintptr_t)cgame_module;

    // PATCH 1 : legs swingTolerance push 40.0 -> push <config.legs_tolerance>
    // Found via RE : cgame+0x451d is `68 00 00 20 42` (push 0x42200000 = 40.0)
    // Context (BG_PlayerAngles, just before BG_SwingAngles call for legs yaw):
    //   ... mov eax, [esp+0x18]
    //       mov esi, [<dvar/cache addr>]
    //       push esi
    //       push 150.0  <- clampTolerance
    //       push 40.0   <- swingTolerance (THIS ONE)
    //       push ebp    <- destination
    //       call BG_SwingAngles
    const uintptr_t legs_tol_push = base + CGAME_SWING_LEGS_TOLERANCE_PUSH_RVA;
    const bool ok1 = patch_push_imm32_float(legs_tol_push, 40.0f,
                                            g_swing_fix_config.legs_tolerance,
                                            "legs_tolerance");

    // PATCH 2 : torso pitch swingSpeed push 0.15 -> push <config.torso_pitch_speed>
    // Found via RE : cgame+0x459d is `68 9a 99 19 3e` (push 0x3E19999A = 0.15)
    // Context (BG_PlayerAngles, last BG_SwingAngles call for torso pitch):
    //   ... fmul dword [0.6f-const]
    //       fstp [esp+0x2c]
    //       mov eax, [esp+0x2c]
    //       push 0.15    <- swingSpeed (THIS ONE)
    //       push 45.0    <- clampTolerance
    //       push 0       <- swingTolerance
    //       lea ecx, [edi+0x3bc]
    //       lea esi, [edi+0x3b8]
    //       push eax     <- destination
    //       call BG_SwingAngles
    const uintptr_t torso_pitch_speed_push = base + CGAME_SWING_TORSO_PITCH_SPEED_PUSH_RVA;
    const bool ok2 = patch_push_imm32_float(torso_pitch_speed_push, 0.15f,
                                            g_swing_fix_config.torso_pitch_speed,
                                            "torso_pitch_speed");

    if (ok1 && ok2) {
        g_installed = true;
        logger::logf("  swing_fix : 2/2 patches applied (legs_tol=%.2f torso_pitch_speed=%.2f)",
                     g_swing_fix_config.legs_tolerance,
                     g_swing_fix_config.torso_pitch_speed);
    } else {
        logger::logf("  swing_fix : partial install (legs=%d torso=%d) - check DLL version",
                     ok1, ok2);
    }
    return ok1 && ok2;
}

}  // namespace patches
