// cod2x BG_PlayerAngles swing fixes: model snaps to camera, no leg/torso lag.

#include "gameplay/swing_fix.h"
#include "core/logger.h"

#include <cstdio>
#include <cstring>

namespace patches {

SwingFixConfig g_swing_fix_config = {
    /* enable             */ true,
    /* legs_tolerance     */ 0.0f,   // vanilla 40.0
    /* torso_pitch_speed  */ 1.0f,   // vanilla 0.15
};

// torso yaw swingSpeed, read by redirected mov (patch 3). 1.0=locked to view, ~0.15=vanilla swing
extern "C" float g_torso_yaw_speed_live = 1.0f;

// movementYaw fraction added to torso yaw target, read by redirected fmul (patch 4). 0=lock to view, 0.3=vanilla
extern "C" float g_torso_yaw_movefrac_live = 0.0f;

namespace {
bool g_installed = false;

// push imm32 = 68 xx xx xx xx; validates opcode + current value vs DLL version
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

    const float current = *(const float*)(addr + 1);

    // may already be new_value on re-init (vid_restart)
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
    // no g_installed early-return: re-runnable so the watcher can re-apply after a
    // map-change cgame reload. The per-patch helpers below are byte-idempotent.
    if (!cgame_module) return false;
    if (!g_swing_fix_config.enable) {
        logger::logf("  swing_fix : disabled in config, skipping");
        return true;
    }

    const uintptr_t base = (uintptr_t)cgame_module;

    // patch 1: legs swingTolerance push 40.0, cgame+0x451d = `68 00 00 20 42`
    const uintptr_t legs_tol_push = base + CGAME_SWING_LEGS_TOLERANCE_PUSH_RVA;
    const bool ok1 = patch_push_imm32_float(legs_tol_push, 40.0f,
                                            g_swing_fix_config.legs_tolerance,
                                            "legs_tolerance");

    // patch 2: torso pitch swingSpeed push 0.15, cgame+0x459d = `68 9a 99 19 3e`
    const uintptr_t torso_pitch_speed_push = base + CGAME_SWING_TORSO_PITCH_SPEED_PUSH_RVA;
    const bool ok2 = patch_push_imm32_float(torso_pitch_speed_push, 0.15f,
                                            g_swing_fix_config.torso_pitch_speed,
                                            "torso_pitch_speed");

    // patch 3: torso yaw swingSpeed dvar 0x301dd388, mov esi,ds:.. @0x4468, operand @0x446a.
    // dvar read at 5 sites: torso @0x446a + legs @0x4495/0x44ee/0x4503/0x4513.
    // redirect TORSO ONLY: forcing legs to 1.0 snaps legs to walk dir, torso clamped to legs = aimwalk bug.
    bool ok3 = false;
    {
        static const uintptr_t YAW_OPS[1] = {0x446a};
        const uint32_t expected = (uint32_t)(base + CGAME_SWING_TORSO_YAW_DVAR_RVA);
        const uint32_t ours     = (uint32_t)(uintptr_t)&g_torso_yaw_speed_live;
        int n = 0;
        for (int k = 0; k < 1; ++k) {
            uint32_t* operand = (uint32_t*)(base + YAW_OPS[k]);
            if (*operand != expected && *operand != ours) {
                logger::logf("  swing_fix [yaw_speed] : operande @rva 0x%lx inattendue "
                             "(0x%08x), skip.", (unsigned long)YAW_OPS[k], (unsigned)*operand);
                continue;
            }
            DWORD oldp = 0;
            if (VirtualProtect(operand, 4, PAGE_READWRITE, &oldp)) {
                *operand = ours;
                VirtualProtect(operand, 4, oldp, &oldp);
                FlushInstructionCache(GetCurrentProcess(), operand, 4);
                ++n;
            }
        }
        ok3 = (n >= 1);
        logger::logf("  swing_fix [yaw_speed] : %d/1 chargement dvar redirige "
                     "-> notre var (=%.2f) [TORSE seul ; jambes=vanilla]", n, g_torso_yaw_speed_live);
    }

    // patch 4: head/weapon lock - fmul ds:imm32 @0x444b, operand @0x444d -> our var (0=drop 0.3*movementYaw)
    bool ok4 = false;
    {
        const uintptr_t fmul_op = base + CGAME_SWING_TORSO_YAW_MOVEFRAC_FMUL_RVA; // 0x444d
        if (*(const uint16_t*)(fmul_op - 2) != 0x0dd8) {                          // fmul ds:imm32
            logger::logf("  swing_fix [torso_yaw_movefrac] : opcode inattendu @0x%08x "
                         "(pas 'fmul ds:imm32'). Patch annule.", (unsigned)(fmul_op - 2));
        } else {
            uint32_t* operand = (uint32_t*)fmul_op;
            const uint32_t expected = (uint32_t)(base + CGAME_SWING_TORSO_YAW_MOVEFRAC_CONST_RVA);
            const uint32_t ours     = (uint32_t)(uintptr_t)&g_torso_yaw_movefrac_live;
            if (*operand != expected && *operand != ours) {
                logger::logf("  swing_fix [torso_yaw_movefrac] : operande inattendu "
                             "(got 0x%08x, attendu 0x%08x). Patch annule.",
                             (unsigned)*operand, (unsigned)expected);
            } else {
                DWORD oldp = 0;
                if (VirtualProtect(operand, 4, PAGE_READWRITE, &oldp)) {
                    *operand = ours;
                    VirtualProtect(operand, 4, oldp, &oldp);
                    FlushInstructionCache(GetCurrentProcess(), operand, 4);
                    ok4 = true;
                    logger::logf("  swing_fix [torso_yaw_movefrac] : fmul @0x%08x redirige "
                                 "-> notre var (=%.2f ; 0=arme verrouillee sur la vue)",
                                 (unsigned)(fmul_op - 2), g_torso_yaw_movefrac_live);
                }
            }
        }
    }

    if (ok1 && ok2 && ok3 && ok4) {
        g_installed = true;
        logger::logf("  swing_fix : 4/4 patches applied (legs_tol=%.2f pitch_spd=%.2f yaw_spd=%.2f yaw_movefrac=%.2f)",
                     g_swing_fix_config.legs_tolerance,
                     g_swing_fix_config.torso_pitch_speed,
                     g_torso_yaw_speed_live, g_torso_yaw_movefrac_live);
    } else {
        logger::logf("  swing_fix : partial install (legs=%d pitch=%d yaw=%d movefrac=%d) - check DLL version",
                     ok1, ok2, ok3, ok4);
    }
    return ok1 && ok2;
}

}  // namespace patches
