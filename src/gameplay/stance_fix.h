#ifndef COD1RELOADED_STANCE_FIX_H
#define COD1RELOADED_STANCE_FIX_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// 3rd-person stance-transition sync (the cod2x stance port, CoD1-shaped).
//
// CoD1 already times stance changes natively: BG_SetNewAnimation (cgame 0x3870) arms
// a blend-hold `ci+0x4a4 = cg.time + 400ms` whenever the legs anim crosses a stance
// class (stand<->crouch<->prone), and every blend started inside that window is
// force-extended to it. But the 1st-person view/hitbox transitions in 150ms (down) /
// 200ms (up) -> the model enemies see lags the real hitbox by up to 250ms during
// crouch spam. Fix = re-time that hold to match the 1st person. One imm32:
//   `add ecx, 0x190` @ cgame+0x397d, imm32 @ +0x397f.
// Independent from viewheight_lerp_speed (gamex86 rate-path): no interaction.
struct StanceFixConfig {
    bool enable   = true;
    int  blend_ms = 250;   // HARDCODED for competitive (not from .ini); 250=tuned 2026-07-24, 400=vanilla
};

extern StanceFixConfig g_stance_fix_config;

constexpr uintptr_t CGAME_STANCE_HOLD_IMM_RVA = 0x0000397f; // imm32 of `add ecx, 0x190`
constexpr uint32_t  CGAME_STANCE_HOLD_VANILLA = 0x190;      // 400 ms

bool apply_stance_fix(HMODULE cgame_module);  // idempotent; re-run on cgame reload
void stance_fix_hot_repoke();                 // hot-reload: re-poke on value/enable change

}  // namespace patches

#endif
