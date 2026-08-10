// cod2x aimwalk fix ported to CoD1. single hook at call site cgame+0x51d8.

#include "gameplay/lean_fix.h"
#include "core/logger.h"
#include "core/patches.h"
#include "video/widescreen_fix.h"
#include "gameplay/swing_fix.h"
#include "gameplay/stance_fix.h"
#include "gameplay/anim_clamp.h"
#include "features/settings_menu.h"
#include "shared/lean_controllers.h"

#include <cstdio>
#include <cstdlib>
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
    // MOVED, no longer read: the back-bone reprojection is now LC_DIAG_BONES / LC_DIAG_K
    // in src/shared/lean_controllers.c so the server runs the same numbers from the same
    // source. The values below are the ones that shipped, kept only as a record of what
    // the shared constants were set from. Editing them here does nothing.
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
    // ALSO MOVED, no longer read: the lateral shift is LC_LATERAL_TOTAL (7.5, the total
    // both sides must reach) in src/shared/lean_controllers.c. This client contributes
    // 7.5 - 2.5 = 5.0 on top of cgame's own constant, exactly as it did, but the number
    // that governs it is now the same object the server reads.
    /* body_shift_lean_scale */ 1.0f,
    // 2.0 = the value the players actually validated (last .ini before the hardcode
    // migration). It was bumped to 3.0 mid-firefight on 2026-07-30 without validation;
    // audit 2026-07-31 reverts it. MUST equal PB_BODY_SHIFT_RIGHT_SCALE server-side.
    /* body_shift_right_scale*/ 2.0f,
    /* body_yaw_lock         */ 1.0f,
    // OFF since 1.6.5, and hardcoded. This filter had a discrete state (crouch + a 45-deg
    // movement-yaw bucket + the lean roll thresholded at 3 deg); on every flip it FROZE
    // the pose and restarted a 250 ms interpolation from the frozen one. A lean crosses
    // that 3-deg threshold going out and coming back, so a peek got two freezes - on a
    // motion that is continuous. Measured 2026-08-10 on a live dump: the model held
    // completely still for ~45 ms while the real lean moved from -0.20 to -0.05, and on a
    // turning lean the smoothed back_low sat 7.9 deg of pitch and 17 deg of root yaw away
    // from the pose the server was testing. It was not smoothing the model, it was
    // chopping it - and it was the last client-only stage after src/shared landed.
    /* ctrl_smooth_enable    */ false,
    /* ctrl_smooth_time      */ 250,
    /* ctrl_dump             */ 0,      // ini key `ctrl_dump`, 0 = off
};

namespace {
bool g_installed = false;
uintptr_t g_cgame_base = 0;



// adjust_rotation_yd() and diag_yawdiff() used to live here. They moved to
// src/shared/lean_controllers.c, which the server compiles too - having one copy of them
// per side, each with its own tuning knobs, is the thing this session removed.
// move_diag_parent had four modes; only mode 0 (cod2x's childYaw - originYaw) was ever
// shipped, so the shared file implements that one and no selector.

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

// --- bridge to src/shared/lean_controllers.c ------------------------------------
// That file is compiled into BOTH mss32.dll and cod1plus.so and holds the adjustments
// themselves; everything below is just the client's half of the plumbing.

// Sink for the shared dump. extern "C" so its type matches the C function pointer in
// lc_ctx_t exactly, rather than relying on the compiler being relaxed about it.
extern "C" void lean_ctrl_log_sink(const char* line) {
    logger::logf("%s", line);
}

// Measurement knob, read once. Dumping is a diagnostic and gives no gameplay advantage,
// so unlike the pose values it is configurable at runtime:
//     cod1reloaded.ini  ->  ctrl_dump = 400        (primary; logged at every launch)
//     COD1RELOADED_CTRL_DUMP=400                   (fallback, matches the server's name)
// The ini comes first on purpose. The environment route silently produced nothing twice,
// because a shortcut or an Explorer double-click does not inherit a variable exported in
// some other shell - and from inside the game an unset variable is indistinguishable from
// one deliberately left off. Output lands in cod1reloaded.log next to the DLL.
static int ctrl_dump_budget() {
    static int budget = -1;
    if (budget < 0) {
        budget = g_lean_fix_config.ctrl_dump;
        if (budget <= 0) {
            const char* e = getenv("COD1RELOADED_CTRL_DUMP");
            budget = (e && *e) ? atoi(e) : 0;
        }
        if (budget < 0) budget = 0;
        if (budget)
            logger::logf("  ctrl dump ON: %d samples/client | %s", budget, lc_banner());
    }
    return budget;
}

// The lateral shift cgame's OWN copy of the shared bg code already applied before we got
// the buffer (out[22] += -fLeanFrac * this). MSVC pooled its float literals: there is
// exactly one 2.5f in cgame_mp_x86.dll (VA 0x3006b6bc) and FIVE readers, so unlike the
// server's copy this constant cannot be patched in place - the client's contribution has
// to stay a hook. lean_controllers.c tops us up from here to the shared total.
static const float CLIENT_ENGINE_LATERAL = 2.5f;

// Did lc_apply() sample this frame? apply_ctrl_smooth() runs immediately afterwards on
// the same buffer from the same trampoline, so a plain static carries the answer across
// and the "smooth" line lands in the same sample as "pre"/"post" instead of on its own
// schedule.
static bool  g_lc_sampled = false;
static lc_ctx_t g_lc_ctx  = {};

extern "C" void apply_lean_adjust(float* controllers,
                                  const void* client_info,
                                  const void* entity) {
    g_lc_sampled = false;   // before every return path, so the "smooth" stage below can
                            // never fire on a stale sample from an earlier entity
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
    // Prone applies nothing and returns here, so lc_apply's own prone branch is only ever
    // reached from the server. That is deliberate: the two used to disagree (the server
    // still ran the back-bone reprojection on a prone player while the client did not),
    // and the shared file resolves it in favour of what production draws today.
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

    // Client-only pose knobs, BOTH ZERO in the competitive build (see the config at the
    // top of this file for why they must stay that way). They are the last edits in this
    // file that the server has no copy of; if either is ever revived it belongs in
    // lean_controllers.c, not here, or the desync comes straight back.
    {
        float* cbuf = (float*)controllers;
        const float lf = cbuf[20] / 3.75f;   // ~fLeanFrac, sign=side

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

    // THE SHARED HALF. Everything that has to agree with the server - the lateral top-up
    // and the back-bone reprojection - now lives in src/shared/lean_controllers.c, which
    // is compiled into this DLL and into cod1plus.so from byte-identical copies. Both
    // call sites hand it the same struct; the only per-side inputs are the stance (each
    // side detects it differently - see the header) and what each side's own engine copy
    // already applied laterally.
    //
    // What used to be here: our own copy of the two adjustments, tuned against the
    // server's own copy by hand. That is the arrangement that would not converge -
    // aligning one of them always left the other off, because nothing forced them to be
    // the same code. This call is the fix for that, not another coefficient.
    memset(&g_lc_ctx, 0, sizeof(g_lc_ctx));
    g_lc_ctx.side           = LC_SIDE_CLIENT;
    g_lc_ctx.client_num     = *(const int*)((const char*)entity + ENT_CLIENTNUM_OFFSET);
    g_lc_ctx.stance         = is_crouch ? LC_STANCE_CROUCH : LC_STANCE_STAND;
    g_lc_ctx.engine_lateral = CLIENT_ENGINE_LATERAL;
    g_lc_ctx.now_ms         = (unsigned int)GetTickCount();
    g_lc_ctx.es             = entity;        // cgame passes the entityState here: this is
                                             // the pointer we read eFlags(+0x08) and
                                             // clientNum(+0x90) from, the same offsets the
                                             // server's es has.
    g_lc_ctx.ci             = client_info;
    g_lc_ctx.dump           = ctrl_dump_budget();
    g_lc_ctx.log            = lean_ctrl_log_sink;

    g_lc_sampled = lc_apply(controllers, &g_lc_ctx) != 0;
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

    // Third stage, CLIENT ONLY - dumped so the comparison can see it.
    // This filter has per-client history and a per-frame velocity clamp, so it is not a
    // pure function of anything the server holds and it cannot be moved into the shared
    // file as it stands. In a held, stationary lean it should converge and "smooth" should
    // equal "post"; if the measurement shows it does not, then this - not the lateral
    // constant and not the reprojection - is what is left of the desync.
    if (g_lc_sampled) lc_dump(buf, &g_lc_ctx, "smooth");
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
    // Identity of the shared module. The server prints the same line at its own install.
    // If the two strings differ, one repo's copy of src/shared/lean_controllers.* was
    // updated and the other was not, and every alignment claim below it is void.
    logger::logf("  shared: %s", lc_banner());
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
