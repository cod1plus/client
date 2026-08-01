// cod2x aimwalk fix ported to CoD1. single hook at call site cgame+0x51d8.

#include "gameplay/lean_fix.h"
#include "core/logger.h"
#include "core/patches.h"
#include "video/widescreen_fix.h"
#include "gameplay/swing_fix.h"
#include "gameplay/stance_fix.h"
#include "gameplay/anim_clamp.h"
#include "features/settings_menu.h"

#include <cstdio>
#include <cstring>
#include <cmath>

namespace patches {

// COMPETITIVE RELEASE: these are HARDCODED (no longer read from cod1reloaded.ini) so a
// player can't edit the model/movement fixes to gain an advantage. Values = the tuned
// set that was in the .ini as of 2026-07-24. To change them now you must rebuild.
LeanFixConfig g_lean_fix_config = {
    /* enable                */ true,
    /* apply_in_stand        */ true,
    /* diag_log_count        */ 0,      // release: no diag logging
    /* move_diag_fix         */ 2,
    /* move_diag_lean_only   */ false,
    /* move_diag_parent      */ 0,
    /* diag_k_pos            */ 0.75f,
    /* diag_k_neg            */ 0.75f,
    // DISABLED 2026-07-30, same reason as body_shift below: both write cbuf[20], which
    // IS the lean value (this file reads lf = cbuf[20]/3.75 from it), so they change how
    // far the model leans ON SCREEN while the server keeps using the engine's own lean.
    //   lean_diag_right_scale 0.4 -> cbuf[20] += 2.88 on a full right lean: 1.77x the
    //     engine lean, so the visible head sits ~35 units out against a hit sphere at
    //     17.6 - unhittable by a wide margin.
    //   lean_diag_scale 1.0 -> cbuf[20] -= 7.20 on a left lean, which flips the sign:
    //     the body visibly leans the WRONG WAY.
    // Cosmetic pose tweaks cannot move the player relative to his own hitbox on a
    // competitive mod. Keep the knobs for a future version that syncs the pose to the
    // server; do not re-enable them as they are.
    /* lean_diag_scale       */ 0.0f,
    /* lean_diag_right_scale */ 0.0f,
    // RESTORED 2026-07-30. Briefly zeroed because it made leaning players unhittable -
    // wrong call: it is the fix for the "helicopter" peek (CoD1 barely moves the body on
    // a lean, so a peeker shows an arm and nothing else). Removing it brought that
    // straight back.
    //
    // The real defect was that the shift moved the DRAWN body without the server
    // knowing, so the model sat 5-12.5 units off its own hitbox. Fixed properly by
    // mirroring the exact same offset onto the server hitbox: lean_hitbox.c recomputes
    // it from leanf + stance, which are values both sides already have, so no network
    // sync is involved. See LEAN_BODY_SHIFT_* there - THE TWO MUST STAY IN STEP.
    /* body_shift_lean_scale */ 1.0f,
    // 2.0 = the value the players actually validated (last .ini before the hardcode
    // migration). It was bumped to 3.0 mid-firefight on 2026-07-30 without validation;
    // audit 2026-07-31 reverts it. MUST equal PB_BODY_SHIFT_RIGHT_SCALE server-side.
    /* body_shift_right_scale*/ 2.0f,
    /* body_yaw_lock         */ 1.0f,
    /* ctrl_smooth_enable    */ true,
    /* ctrl_smooth_time      */ 250,
};

namespace {
bool g_installed = false;
uintptr_t g_cgame_base = 0;



static void adjust_rotation_yd(float yawDiffDeg, float* child) {
    const float rad = yawDiffDeg * 0.01745329252f;
    const float cp = cosf(rad), sp = sinf(rad);
    const float p = child[0], r = child[2];
    child[0] = p * cp - r * sp;
    child[2] = p * sp + r * cp;
}
static float diag_yawdiff(int mode, float childYaw, float originYaw) {
    switch (mode) {
        case 1:  return originYaw - childYaw;
        case 2:  return childYaw;
        case 3:  return -childYaw;
        default: return childYaw - originYaw;   // exact cod2x
    }
}

// Reproduce cod2x's movementYaw INVARIANT on our yawDiff.
//
// cod2x PM_SetMovementDir does, before the angle ever reaches the controllers:
//     if (isMovingBackwards) moveyaw = AngleNormalize180(moveyaw + 180);
//     moveyaw = fclamp(moveyaw, -90, 90);
// so their legs-vs-view yaw (tag_origin_angles[1], the parent of the yawDiff) can
// NEVER exceed +-90 and backward movement folds to ~0. cos(yawDiff) is therefore
// never negative and animation_adjustRotation can safely run unconditionally.
//
// CoD1's movementDir is KEYS-based (no such normalize/clamp): moving straight back
// leaves the legs ~180 deg from the view, so cos(180) = -1 FLIPS the back-bone pitch
// -> "crouch + S dips the head". Folding here restores cod2x's invariant at the only
// place we can reach it, and unlike gating on lean it also covers crouch+lean+back.
__attribute__((unused)) static float diag_fold(float yd) {
    while (yd >  180.0f) yd -= 360.0f;
    while (yd < -180.0f) yd += 360.0f;
    if (yd >  90.0f || yd < -90.0f) {          // backward-ish: fold like cod2x's +180
        yd += 180.0f;
        while (yd >  180.0f) yd -= 360.0f;
        while (yd < -180.0f) yd += 360.0f;
    }
    if (yd >  90.0f) yd =  90.0f;              // cod2x's fclamp(-90, 90)
    if (yd < -90.0f) yd = -90.0f;
    return yd;
}
}  // namespace

extern "C" void apply_lean_adjust(float* controllers,
                                  const void* client_info,
                                  const void* entity) {
    if (!g_lean_fix_config.enable) return;
    if (!controllers || !client_info || !entity) return;

    {   // hot-reload ini knobs, throttled 400ms
        static DWORD s_last_reload = 0;
        const DWORD now = GetTickCount();
        if (now - s_last_reload > 400) {
            s_last_reload = now;
            hot_reload_lean_reshape();
        }
    }

    const uint32_t eflags = *(const uint32_t*)((const char*)entity + ENT_EFLAGS_OFFSET);
    const bool is_crouch = (eflags & ENT_FLAG_CROUCH) != 0;
    const bool is_prone  = (eflags & ENT_FLAG_PRONE)  != 0;
    if (is_prone) return;
    if (!is_crouch && !g_lean_fix_config.apply_in_stand) return;


    // body-yaw lock on view during strafe (anti-swing SMG). 0=straight, 1=vanilla.
    if (g_lean_fix_config.body_yaw_lock != 1.0f) {
        float* bb = (float*)controllers;
        const float k = g_lean_fix_config.body_yaw_lock;
        bb[19] *= k;   // tag_origin yaw
        bb[1]  *= k;   // back_low yaw
        bb[4]  *= k;   // back_mid yaw
        bb[7]  *= k;   // back_up yaw
    }

    // cod2x diagonal lean-left pose (#2) + body sideways shift (#3).
    // animation.cpp:332-357 + 184-201. runs before the diag fix.
    // WARNING: lean sign convention unverified in-game (flip if wrong side).
    {
        float* cbuf = (float*)controllers;
        const float lf = cbuf[20] / 3.75f;   // ~fLeanFrac, sign=side

        // #3 lateral body shift; ADD to CoD1's existing strafe term. cbuf[22]=tag_origin_offset[1]
        if (g_lean_fix_config.body_shift_lean_scale != 0.0f && fabsf(lf) > 0.02f) {
            const bool ll = lf < 0.0f;   // lean-left (fLeanFrac<0)
            float K = is_crouch ? (ll ? 12.5f : 2.5f) : (ll ? 5.0f : 2.5f);
            // cod2x under-shifts the RIGHT lean (weapon side): body barely leaves the
            // wall so the peeker shows only an arm. Boost the right side to expose it.
            if (!ll) K *= g_lean_fix_config.body_shift_right_scale;
            cbuf[22] += -lf * K * g_lean_fix_config.body_shift_lean_scale;
        }

        // #2 diagonal lean-left pose
        if (g_lean_fix_config.lean_diag_scale != 0.0f && lf < -0.02f) {
            const float myaw = *(const float*)((const char*)client_info + CI_MOVEMENTYAW_OFFSET);
            if (myaw > 5.0f && myaw < 90.0f) {
                const float off = -lf * g_lean_fix_config.lean_diag_scale;
                if (is_crouch) {
                    cbuf[18] += off * 3.8f;
                    cbuf[20] -= off * 3.8f;
                    cbuf[0]  += 40.0f * off;
                    cbuf[1]  += 30.0f * off;
                    cbuf[3]  += -20.0f * off;
                    cbuf[6]  += -20.0f * off;   // sum ~0 at weapon
                } else {
                    cbuf[18] += off * 7.2f;
                    cbuf[20] -= off * 7.2f;
                }
            }
        }

        // right-lean pose — CoD1 gap: neither vanilla nor cod2x exposes the body on a
        // right lean (the weapon side), so forward/backward + lean-right ("helicopter")
        // shows only the arm. ROLL-ONLY whole-body tilt to the right (cod2x's forward
        // pitch made the body nose-dive when combined with fwd+strafe — dropped). NO
        // direction gate: moving straight has movementYaw=0 (keys-based), a direction
        // gate misses it — exactly the exploit. Hot-reload knob (0=off).
        if (g_lean_fix_config.lean_diag_right_scale != 0.0f && lf > 0.02f) {
            const float off = lf * g_lean_fix_config.lean_diag_right_scale;
            cbuf[20] += off * (is_crouch ? 3.8f : 7.2f);
        }
    }

    // cod2x animation_adjustRotation: THE fix for the fwd+strafe+lean nose-dive.
    // reprojects back-bone pitch/roll by yaw delta vs tag_origin so lean roll
    // stays lateral instead of diving. identity when yawDiff~0.
    //
    // move_diag_lean_only: cod2x's tag angles come from a VELOCITY-based movementDir,
    // so with no lean the yawDiff is ~0 and the reprojection is identity. CoD1's
    // movementDir is KEYS-based -> moving straight BACK (S) gives a ~180deg yawDiff,
    // and cos(180)=-1 FLIPS the back-bone pitch => "crouch + S makes the head dip".
    // Since this fix only matters while leaning, gate it on lean so plain crouch+back
    // (no lean) never gets reprojected. Hot-reload knob (0 = old always-on behaviour).
    const float lf_diag = ((const float*)controllers)[20] / 3.75f;   // side, ~fLeanFrac
    if (g_lean_fix_config.move_diag_fix > 0 &&
        (!g_lean_fix_config.move_diag_lean_only || fabsf(lf_diag) > 0.02f)) {
        float* bl   = (float*)((char*)controllers + CTRL_BACK_LOW_ANGLES);
        float* bm   = (float*)((char*)controllers + CTRL_BACK_MID_ANGLES);
        float* bu   = (float*)((char*)controllers + CTRL_BACK_UP_ANGLES);
        float* to_a = (float*)((char*)controllers + CTRL_TAG_ORIGIN_ANGLES);
        const int   mode = g_lean_fix_config.move_diag_parent;
        const float toy  = to_a[1];
        const float o_bl0 = bl[0], o_bl1 = bl[1], o_bl2 = bl[2];

        // per-side strength (rig not mirrored)
        const float kside = (o_bl2 < 0.0f) ? g_lean_fix_config.diag_k_neg
                                           : g_lean_fix_config.diag_k_pos;
        adjust_rotation_yd(diag_yawdiff(mode, bl[1], toy) * kside, bl);
        if (g_lean_fix_config.move_diag_fix >= 2)
            adjust_rotation_yd(diag_yawdiff(mode, bm[1], toy) * kside, bm);
        if (g_lean_fix_config.move_diag_fix >= 3)
            adjust_rotation_yd(diag_yawdiff(mode, bu[1], toy) * kside, bu);

        static DWORD s_dl = 0; static int s_dn = 0;
        const DWORD now_d = GetTickCount();
        if (s_dn < g_lean_fix_config.diag_log_count && (now_d - s_dl) > 200 &&
            (fabsf(o_bl1 - toy) > 1.5f || fabsf(o_bl2) > 1.5f)) {
            s_dl = now_d; ++s_dn;
            logger::logf("diagfix m=%d toY=%.1f blY=%.1f yd=%.1f | "
                         "blPitch %.1f->%.1f  blRoll %.1f->%.1f",
                         mode, toy, o_bl1, diag_yawdiff(mode, o_bl1, toy),
                         o_bl0, bl[0], o_bl2, bl[2]);
        }
    }

}

// shortest angle delta [-180,180]
static inline float ctrl_ang_delta(float to, float from) {
    float d = to - from;
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// cod2x synchronized controller smoothing (controllerMovementType +
// BG_LerpOverTime). on a discrete state change, lerp all 24 floats (8 contiguous
// vec3) together over ctrl_smooth_time ms -> bones move in sync, no tag_origin
// vs back desync. state per clientNum, derived from the buffer.
struct CtrlSmoothState {
    bool  init;
    int   last;
    DWORD end;
    DWORD last_tick;   // for per-frame velocity clamp (frametime)
    int   ms;
    float start[24];
    float cur[24];
};
static CtrlSmoothState g_ctrl_smooth[64] = {};

extern "C" void apply_ctrl_smooth(float* controllers,
                                  const void* client_info,
                                  const void* entity) {
    (void)client_info;
    if (!g_lean_fix_config.enable) return;
    if (!g_lean_fix_config.ctrl_smooth_enable) return;
    if (!controllers || !entity) return;

    const uint32_t eflags = *(const uint32_t*)((const char*)entity + ENT_EFLAGS_OFFSET);
    if (eflags & ENT_FLAG_PRONE) return;
    const bool is_crouch = (eflags & ENT_FLAG_CROUCH) != 0;

    const int cn = *(const int*)((const char*)entity + ENT_CLIENTNUM_OFFSET);
    if (cn < 0 || cn >= 64) return;
    CtrlSmoothState& st = g_ctrl_smooth[cn];

    float* buf = (float*)controllers;

    // discrete movement state: buf[19]=movement yaw, buf[8]=back_up roll (lean)
    const float myaw     = buf[19];
    const int   ybucket  = (int)floorf(myaw / 45.0f + (myaw >= 0.0f ? 0.5f : -0.5f));
    const float leanRoll = buf[8];
    const int   lbucket  = (leanRoll < -3.0f) ? -1 : (leanRoll > 3.0f ? 1 : 0);
    const int   type     = (is_crouch ? 1 : 0)
                         | ((ybucket & 0x3F) << 1)
                         | ((lbucket + 1) << 7);

    const DWORD now = GetTickCount();

    if (!st.init) {
        st.init = true;
        st.last = type;
        st.last_tick = now;
        for (int i = 0; i < 24; ++i) st.cur[i] = buf[i];
        return;
    }

    // per-frame budget for the velocity clamp (cod2x BG_LerpAngles/BG_LerpOffset:
    // 0.36 deg/ms on angles, 0.1 units/ms on offsets). This is what kills the
    // lean-spam: the model lean can only move ~1.4 deg/frame @250fps, so spamming
    // the lean key can't flicker it -> enemies see a smooth, trackable model.
    int frametime = (int)(now - st.last_tick);
    st.last_tick = now;
    if (frametime < 1)   frametime = 1;
    if (frametime > 100) frametime = 100;          // cap on hitch / alt-tab
    const float maxAng = (float)frametime * 0.36f;
    const float maxOff = (float)frametime * 0.1f;

    if (type != st.last) {
        st.last = type;
        for (int i = 0; i < 24; ++i) st.start[i] = st.cur[i];   // freeze pose
        st.ms = g_lean_fix_config.ctrl_smooth_time;
        if (st.ms < 1) st.ms = 1;
        st.end = now + (DWORD)st.ms;
    }

    const int   remain = (int)(st.end - now);
    const float fr = 1.0f - (float)remain / (float)st.ms;

    for (int i = 0; i < 24; ++i) {
        const float target = buf[i];
        float out;
        if (fr >= 0.0f && fr <= 1.0f) {
            // active movement-type transition: time-lerp from frozen pose
            if (i < 21) out = st.start[i] + ctrl_ang_delta(target, st.start[i]) * fr; // angles
            else        out = st.start[i] + (target - st.start[i]) * fr;              // offset (linear)
        } else if (i < 21) {
            // steady state: clamp angle change per frame (BG_LerpAngles)
            const float d = ctrl_ang_delta(target, st.cur[i]);
            out = (d >  maxAng) ? st.cur[i] + maxAng
                : (d < -maxAng) ? st.cur[i] - maxAng
                : target;
        } else {
            // steady state: clamp offset change per frame (BG_LerpOffset)
            const float d = target - st.cur[i];
            out = (d >  maxOff) ? st.cur[i] + maxOff
                : (d < -maxOff) ? st.cur[i] - maxOff
                : target;
        }
        st.cur[i] = out;
        buf[i] = out;
    }
}

extern "C" {
    void* g_lean_saved_buf = nullptr;
    void* g_lean_saved_ci  = nullptr;
    void* g_lean_saved_ent = nullptr;
    void* g_lean_original  = nullptr;
}

extern "C" __attribute__((naked))
void lean_fix_hook() {
    asm(
        "movl %eax, _g_lean_saved_buf\n\t"
        "movl %ecx, _g_lean_saved_ci\n\t"
        "movl %ebx, _g_lean_saved_ent\n\t"
        "call *_g_lean_original\n\t"
        "pushl _g_lean_saved_ent\n\t"
        "pushl _g_lean_saved_ci\n\t"
        "pushl _g_lean_saved_buf\n\t"
        "call _apply_lean_adjust\n\t"
        "addl $12, %esp\n\t"
        "pushl _g_lean_saved_ent\n\t"
        "pushl _g_lean_saved_ci\n\t"
        "pushl _g_lean_saved_buf\n\t"
        "call _apply_ctrl_smooth\n\t"
        "addl $12, %esp\n\t"
        "ret\n\t"
    );
}

bool install_lean_fix(HMODULE cgame_module) {
    if (!cgame_module) return false;

    const uintptr_t base       = (uintptr_t)cgame_module;
    const uintptr_t calculator = base + CGAME_DOCONTROLLERS_INTERNAL_RVA;
    const uintptr_t call_site  = base + CGAME_DOCONTROLLERS_CALL_SITE_RVA;

    const uint8_t opcode = *(const uint8_t*)call_site;
    if (opcode != 0xE8) {
        // not a call here - cgame mid-(re)load or wrong DLL version; retry next poll
        logger::logf("  lean fix: opcode 0x%02x at cgame+0x%lx (expected 0xE8), skip",
                     opcode, (unsigned long)CGAME_DOCONTROLLERS_CALL_SITE_RVA);
        return false;
    }

    const int32_t existing_offset  = *(const int32_t*)(call_site + 1);
    const uintptr_t existing_target = call_site + 5 + (uintptr_t)(intptr_t)existing_offset;
    if (existing_target == (uintptr_t)&lean_fix_hook) {
        // already pointing at our hook (idempotent, e.g. re-checked same instance)
        g_lean_original = (void*)calculator;
        g_cgame_base    = base;
        g_installed     = true;
        return true;
    }
    if (existing_target != calculator) {
        logger::logf("  lean fix: call targets cgame+0x%lx, expected 0x%lx - skip",
                     (unsigned long)(existing_target - base),
                     (unsigned long)CGAME_DOCONTROLLERS_INTERNAL_RVA);
        return false;
    }

    g_lean_original = (void*)calculator;
    g_cgame_base    = base;

    const uintptr_t hook_addr  = (uintptr_t)&lean_fix_hook;
    const int32_t   new_offset = (int32_t)(hook_addr - (call_site + 5));
    DWORD old_prot = 0;
    if (!VirtualProtect((void*)(call_site + 1), 4, PAGE_READWRITE, &old_prot))
        return false;
    *(int32_t*)(call_site + 1) = new_offset;
    VirtualProtect((void*)(call_site + 1), 4, old_prot, &old_prot);

    g_installed = true;
    logger::logf("  lean hook installed at cgame+0x%lx -> trampoline 0x%08x",
                 (unsigned long)CGAME_DOCONTROLLERS_CALL_SITE_RVA,
                 (unsigned)hook_addr);
    logger::logf("  leanf source: cgame+0x%lx (direct read, no camera hook)",
                 (unsigned long)CG_LOCAL_LEANF_RVA);
    return true;
}

bool apply_to_cgame(HMODULE cgame_module) {
    if (!cgame_module) return false;

    widescreen_apply_to_cgame(cgame_module);
    settings_menu_apply_to_cgame(cgame_module);  // unlock cg_fov (before CG_Init proceeds)
    apply_swing_fix(cgame_module);
    apply_stance_fix(cgame_module);  // sync model stance blend with 1st person
    // apply_anim_clamp(cgame_module);  // REVERTED 2026-07-24: the `xor esi,esi` patch
    //   clobbers a register the CALLER still needs (the old path called Com_Error and
    //   never returned, so clobbering was harmless there) -> camera/mouse glitches.
    if (!g_lean_fix_config.enable) {
        logger::logf("  lean hook NOT installed (disabled)");
        return true;
    }
    return install_lean_fix(cgame_module);
}

// True when cgame is loaded but our patches are gone (engine reloaded the DLL on
// a map change -> bytes reverted to vanilla). Cheap sentinel read, polled by the
// watcher so it can re-apply. Returns false while cgame is mid-(re)load.
bool cgame_needs_reapply(HMODULE cgame_module) {
    if (!cgame_module) return false;
    const uintptr_t base = (uintptr_t)cgame_module;

    if (g_lean_fix_config.enable) {
        const uintptr_t cs = base + CGAME_DOCONTROLLERS_CALL_SITE_RVA;
        if (*(const uint8_t*)cs != 0xE8) return false;  // not ready yet
        const int32_t off = *(const int32_t*)(cs + 1);
        if (cs + 5 + (uintptr_t)(intptr_t)off != (uintptr_t)&lean_fix_hook)
            return true;  // reverted to original target
    }
    if (g_swing_fix_config.enable) {
        const uintptr_t p = base + CGAME_SWING_LEGS_TOLERANCE_PUSH_RVA;
        if (*(const uint8_t*)p == 0x68 && *(const float*)(p + 1) > 39.0f)
            return true;  // legs swingTolerance back to vanilla 40.0
    }
    return false;
}

}  // namespace patches
