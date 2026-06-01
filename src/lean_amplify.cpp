// Lean amplify : patche les 5 sites AddLeanToPosition dans cgame.
// Voir lean_amplify.h pour le contexte.

#include "lean_amplify.h"
#include "logger.h"

#include <cstdio>
#include <cstring>

namespace patches {

LeanAmplifyConfig g_lean_amplify_config = {
    /* enable */ true,
    /* factor */ 1.5f,    // +50% de shift par defaut
};

namespace {
bool g_installed = false;

// Helper : patche un push imm32 float a une nouvelle valeur, avec
// sanity check sur la valeur attendue. Returns true si OK ou deja patche.
bool patch_push_float(uintptr_t addr, float expected, float new_val,
                      const char* site_label, const char* knob_label) {
    const uint8_t opcode = *(const uint8_t*)addr;
    if (opcode != 0x68) {
        logger::logf("  lean_amplify [%s/%s] : opcode 0x%02x a 0x%08x (attendu 0x68). Patch annule.",
                     site_label, knob_label, opcode, (unsigned)addr);
        return false;
    }
    const float current = *(const float*)(addr + 1);
    const float diff_exp = current - expected;
    const float diff_new = current - new_val;
    const float abs_exp  = diff_exp < 0 ? -diff_exp : diff_exp;
    const float abs_new  = diff_new < 0 ? -diff_new : diff_new;
    if (abs_exp > 0.01f && abs_new > 0.01f) {
        logger::logf("  lean_amplify [%s/%s] : value mismatch (got %.2f, expected %.2f or %.2f). Patch annule.",
                     site_label, knob_label, current, expected, new_val);
        return false;
    }
    if (abs_new < 0.01f) {
        return true;  // already patched, idempotent
    }

    DWORD old_protect = 0;
    if (!VirtualProtect((void*)(addr + 1), 4, PAGE_READWRITE, &old_protect)) {
        logger::logf("  lean_amplify [%s/%s] : VirtualProtect failed", site_label, knob_label);
        return false;
    }
    *(float*)(addr + 1) = new_val;
    VirtualProtect((void*)(addr + 1), 4, old_protect, &old_protect);
    return true;
}
}  // namespace

bool apply_lean_amplify(HMODULE cgame_module) {
    if (g_installed) return true;
    if (!cgame_module) return false;
    if (!g_lean_amplify_config.enable) {
        logger::logf("  lean_amplify : disabled in config, skipping");
        return true;
    }

    const float factor = g_lean_amplify_config.factor;
    // Bornes safety : factor=1.0 = no-op, factor=2.5 max (au-dela = collision broken)
    if (factor < 0.5f || factor > 2.5f) {
        logger::logf("  lean_amplify : factor %.2f hors bornes [0.5, 2.5]. Patch annule.", factor);
        return false;
    }
    if (factor > 0.99f && factor < 1.01f) {
        logger::logf("  lean_amplify : factor ~1.0 (no-op), skipping");
        return true;
    }

    const float new_20 = 20.0f * factor;
    const float new_16 = 16.0f * factor;

    const uintptr_t base = (uintptr_t)cgame_module;
    int ok_count = 0;
    int total = 0;

    for (int i = 0; i < 5; ++i) {
        const LeanSite& s = kLeanSites[i];
        char site_label[16];
        snprintf(site_label, sizeof(site_label), "site%d", i + 1);

        bool ok20 = patch_push_float(base + s.push20_rva, 20.0f, new_20,
                                     site_label, "shift");
        bool ok16 = patch_push_float(base + s.push16_rva, 16.0f, new_16,
                                     site_label, "dist");
        total += 2;
        if (ok20) ++ok_count;
        if (ok16) ++ok_count;
    }

    g_installed = (ok_count == total);
    logger::logf("  lean_amplify : %d/%d patches applied (factor=%.2f, shift %.1f->%.1f, dist %.1f->%.1f)",
                 ok_count, total, factor, 20.0f, new_20, 16.0f, new_16);
    return g_installed;
}

}  // namespace patches
