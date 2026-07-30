// Sync the 3rd-person model stance blend with the 1st-person stance time (see header).

#include "gameplay/stance_fix.h"
#include "core/logger.h"

namespace patches {

StanceFixConfig g_stance_fix_config = {};

namespace {

uint32_t clamp_ms(int ms) {
    if (ms < 50)   return 50;
    if (ms > 1000) return 1000;
    return (uint32_t)ms;
}

// poke the hold imm32 to `want`. Only touches the site if it currently holds the
// vanilla 400 or a plausible previously-poked value (wrong DLL version -> no-op).
bool sync_hold(HMODULE cgame, uint32_t want, const char* tag) {
    if (!cgame) return false;
    uint32_t* p = (uint32_t*)((uintptr_t)cgame + CGAME_STANCE_HOLD_IMM_RVA);
    const uint32_t cur = *p;
    if (cur == want) return true;
    if (cur != CGAME_STANCE_HOLD_VANILLA && (cur < 50 || cur > 1000)) {
        logger::logf("  stance_fix: imm @cgame+0x%x = 0x%x (expected 0x190) - wrong DLL, skip",
                     (unsigned)CGAME_STANCE_HOLD_IMM_RVA, cur);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *p = want;
    VirtualProtect(p, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 4);
    logger::logf("  stance_fix: model stance blend %u -> %u ms (%s)", cur, want, tag);
    return true;
}

}  // namespace

bool apply_stance_fix(HMODULE cgame_module) {
    if (!g_stance_fix_config.enable) return true;
    return sync_hold(cgame_module, clamp_ms(g_stance_fix_config.blend_ms), "apply");
}

void stance_fix_hot_repoke() {
    HMODULE cg = GetModuleHandleA("cgame_mp_x86.dll");
    if (!cg) return;
    const uint32_t want = g_stance_fix_config.enable
                        ? clamp_ms(g_stance_fix_config.blend_ms)
                        : CGAME_STANCE_HOLD_VANILLA;   // live-disable restores vanilla
    if (*(const uint32_t*)((uintptr_t)cg + CGAME_STANCE_HOLD_IMM_RVA) != want)
        sync_hold(cg, want, "live");
}

}  // namespace patches
