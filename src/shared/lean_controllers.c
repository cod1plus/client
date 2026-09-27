/*
 * lean_controllers.c - see lean_controllers.h for why this file exists and for the
 *                      "IT EXISTS TWICE, KEEP THE TWO COPIES IDENTICAL" rule.
 *
 * STAGES
 * The engine function runs first on both sides; we only post-process its buffer:
 *
 *   engine BG_Player_DoControllersInternal   ->  "pre"
 *   lc_apply(): lateral top-up, [v2: the BLEND], then the back-bone reprojection -> "post"
 *   (engine BG_Player_DoControllers limiter: a no-op when the blend is on, see BLEND)
 *
 * The old client-only apply_ctrl_smooth() stage ("smooth") is dead since 1.6.5 and stays
 * dead: the BLEND below is what it should have been - the same code on both sides, fed
 * only by networked state and the game clock, and never triggered by the lean.
 *
 * MEASUREMENT PROTOCOL this instrumentation is built for
 *   Two clients. One stands still, full lean left, does not move. The other watches.
 *   Both sides dump the same client_num in the same pose; diff the "post" lines field by
 *   field. Repeat crouched. The fields that differ are the answer - and if the IN line
 *   differs too, the divergence is UPSTREAM of this buffer (see the swing_fix note below)
 *   and no amount of tuning down here can close it.
 */

#include "lean_controllers.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ============================ the shared constants ============================
 * These four numbers ARE the contract. They used to live as prose in two files
 * ("THE TWO MUST STAY IN STEP", "MUST equal PB_BODY_SHIFT_RIGHT_SCALE server-side");
 * every time one side was retuned and the other was not, leaning heads silently stopped
 * registering. Now there is one copy, both sides read it, and lc_banner() prints it. */

/* Total lateral body shift, in units at fLeanFrac = 1, that the drawn AND tested pose
 * must both end up with. Today: client 2.5 (engine) + 5.0 (topped up here) = 7.5;
 * server 7.5 (engine, .rodata retuned) + 0.0 = 7.5. Changing this one number now moves
 * both sides at once, which is exactly what could not be guaranteed before. */
#define LC_LATERAL_TOTAL       7.5f

/* out[20] = fLeanFrac * 50.0f * 0.075f (consts .rodata 0x73d30 / 0x73da4, combined at
 * game 0x1a8b7..0x1a8cd). We recover fLeanFrac by dividing it back out. NEVER re-derive
 * it from ps.leanf: that is clamped to +/-0.5 by PM_UpdateLean (0x25cf5) while
 * GetLeanFraction(x) = (2-|x|)*x gives 0.75 at full lean, so leanf/0.5 over-injects 33%. */
#define LC_LEAN_ROLL_PER_FRAC  3.75f

/* Below this |fLeanFrac| there is no lean to correct (client lean_fix.cpp:173, server
 * pose_sync.c LEAN_EPS). Also rejects NaN, since NaN fails both comparisons. */
#define LC_LEAN_EPS            0.02f

/* Sanity ceiling. |GetLeanFraction(leanf)| maxes at 0.75, so a legitimate frame can never
 * reach 1.0; this only stops a garbage buffer from becoming a wild shift. It lives here
 * rather than on one side so that a garbage frame is discarded identically by both. */
#define LC_LEAN_SANE_MAX       1.0f

/* Back-bone reprojection (cod2x animation_adjustRotation). 2 = back_low + back_mid.
 * The client ran diag_k_pos = diag_k_neg = 0.75, i.e. its per-side branch was already the
 * identity, so the two collapse to one constant here. The client applies it
 * unconditionally (move_diag_lean_only = false) and so does the server; with yawDiff ~ 0
 * it IS the identity, which is the common case. */
#define LC_DIAG_BONES          2
#define LC_DIAG_K              0.75f

/* ---- entityState_s / clientInfo_t fields, dump only ----------------------------
 * Names and offsets from the engine's own netfield table (CoDMP.exe file 0x180110) and
 * the disassembly; see anim_shared.h. Nothing here is written. */
#define LC_ES_EFLAGS           0x08
#define LC_ES_MOVEMENT_DIR     0x6c   /* angles2[1] = (float) ps->movementDir, signed:
                                       * BG_PlayerStateToEntityState 0x1c5d1..0x1c5f9 */
#define LC_ES_LEGS_ANIM        0xcc
#define LC_ES_ANIM_MOVETYPE    0xe0
#define LC_ES_FTORSO_HEIGHT    0xe4
#define LC_ES_FTORSO_PITCH     0xe8
#define LC_ES_FWAIST_PITCH     0xec

#define LC_CI_LEGS_YAW         0x380  /* read by the engine at 0x1a438 */
#define LC_CI_TORSO_YAW        0x3b0  /* read by the engine at 0x1a444 */
#define LC_CI_UNK_3B8          0x3b8  /* read ONCE at 0x1a477. SETTLED 2026-09-09: it is
                                       * the SWUNG lean angle - BG_PlayerAngles' last
                                       * swing call (game 0x19f73..0x19fa9) writes it,
                                       * speed 0.15 (client: 1.0), clamp 45, flag at
                                       * +0x3bc. The value fed to GetLeanFraction is
                                       * still the instant one at 0x3e4 (0x1a543). Kept
                                       * under its old name so the dump format and the
                                       * banner stay comparable with older logs. */
#define LC_CI_MOVEMENT_YAW     0x3e0
#define LC_CI_LERP_LEAN        0x3e4  /* -> GetLeanFraction, 0x1a543 */
#define LC_CI_PLAYER_ANGLES    0x3e8  /* vec3; ClientEndFrame 0x3b00c copies ps.viewangles */

#define LC_MAX_CLIENTS         64
/* One sample per client per half second while leaning, plus one on every stance or
 * lean-step change. Not leaning is throttled ten times harder: the dump budget is small
 * on purpose (it has to be safe to leave on during a live match) and an idle lobby would
 * otherwise spend all of it before the tester is even in position. The slow tick still
 * runs, so the no-lean baseline every comparison needs is always in the log. */
#define LC_DUMP_PERIOD_MS      500u
#define LC_DUMP_IDLE_PERIOD_MS 5000u

/* ---- v2: the BLEND - see the long comment above lc_blend() -----------------------
 * Durations: cod2x's controllerMovementTime (animation.cpp:329 / :371). */
#define LC_BLEND_MS            250    /* stand / crouch */
#define LC_BLEND_MS_PRONE      400    /* any transition into, out of or within prone */
/* No committed sample for this long = no usable history (a player who just came into
 * view on the client, a map change, a demo seek): restart from the engine's own pose,
 * with no transition, instead of lerping from something stale. */
#define LC_BLEND_STALE_MS      200
/* The engine's limiter, outside a transition - same numbers, now on the game clock:
 * cgame 0x3006b8c0 / 0x3006b7a8, game -0x14fcc / -0x14fc8(%ebx) in 0x1adb6 / 0x1ae7e. */
#define LC_LIMIT_DEG_PER_MS    0.36f
#define LC_LIMIT_UNITS_PER_MS  0.1f
/* eFlags bits the engine's own controller function branches on (game 0x1a39f..0x1a7df,
 * the same four tests in cgame): */
#define LC_EF_DEAD             0x0001  /* 0x1a5a9: no view-yaw subtraction */
#define LC_EF_CROUCH           0x0020  /* 0x1a7df: the lean roll multiplier */
#define LC_EF_PRONE            0x0040  /* 0x1a489 / 0x1a5d1: the prone branch */
#define LC_EF_TURRET           0xc000  /* 0x1a39f: all-zero buffer */
#define LC_ANIM_TOGGLEBIT      0x0200  /* legsAnim & ~this = the animation (0x18130) */

/* ============================ small read helpers ============================
 * memcpy rather than a cast through float*: same code at -O2 on both toolchains, and it
 * cannot trip strict aliasing if these structs are ever given real C types. */
static float lc_rf(const void* base, unsigned off)
{
    float v;
    memcpy(&v, (const char*)base + off, sizeof(v));
    return v;
}

static int lc_ri(const void* base, unsigned off)
{
    int v;
    memcpy(&v, (const char*)base + off, sizeof(v));
    return v;
}

static const char* lc_stance_name(int stance)
{
    if (stance == LC_STANCE_CROUCH) return "crouch";
    if (stance == LC_STANCE_PRONE)  return "prone ";
    return "stand ";
}

/* ============================ the arithmetic ============================ */

/* cod2x animation_adjustRotation, verbatim from lean_fix.cpp adjust_rotation_yd():
 * rotate one bone's (pitch, roll) pair by its yaw delta against tag_origin, leaving yaw
 * alone. Identity at yawDiffDeg = 0.
 *
 * This is the half of the desync that is angular rather than lateral. back_low and
 * back_mid carry the head, and the engine's head volume is an ORIENTED box that rotates
 * with the bone - so an angular error moves the box CORNERS far more than its centre,
 * which is exactly the reported symptom ("the upper outer corner of the head stays
 * unhittable" - the same corner for stand-left and crouch-right seen from the shooter).
 * Raising the lateral constant can never fix a rotation error, which is why two days of
 * tuning it did not converge. */
static void lc_adjust_rotation(float yaw_diff_deg, float* bone)
{
    const float rad = yaw_diff_deg * 0.01745329252f;
    const float cp  = cosf(rad), sp = sinf(rad);
    const float p   = bone[0], r = bone[2];
    bone[0] = p * cp - r * sp;
    bone[2] = p * sp + r * cp;
}

/* ============================ BLEND (v2) ============================
 *
 * cod2x's controller transition (animation.cpp:728-807), rebuilt so that it cannot bring
 * back what killed our first copy of it (apply_ctrl_smooth, off since 1.6.5).
 *
 * WHAT IT FIXES. When the movement changes (strafe left <-> right, stop, forward <-> back,
 * stand <-> crouch), the legs/pelvis yaw (tag_origin) swings to the new direction while
 * the three back bones counter-rotate so that the chest keeps facing the view. The
 * engine's limiter then moves every angle at 0.36 deg/ms ON ITS OWN: the pelvis (the
 * biggest angle) arrives last, the back bones first, and for that fraction of a second the
 * chest - and the head, and a leaning torso - is turned off the view. That is cod2x's
 * "weapon moving side to side when player movement changes"; with a lean it moves the
 * head sideways on every strafe key. The engine builds the yaw chain so that it sums to
 * the torso-vs-view angle (tag_origin = legs - view; back = 0.4/0.4/0.2 x (torso - legs);
 * neck/head = 0.3/0.7 x (view - torso)), which our swing patches hold at 0 on both sides.
 * Moving all the yaws by the SAME fraction keeps that sum at 0 all the way through: the
 * hips turn, the chest does not.
 *
 * HOW, and how it differs from cod2x - each point is one of the old bugs, or a new one
 * found while building this:
 *  - The trigger is discrete, networked and identical on both sides: the legs animation
 *    (toggle bit stripped) and the four eFlags bits the engine's controller function
 *    itself branches on (dead, crouch, prone, turret). The LEAN IS NOT IN IT, not even as
 *    a threshold: apply_ctrl_smooth keyed on the lean roll crossing 3 deg, so every peek
 *    froze the model twice. movementDir is not in it either: in CoD1 it is the angle of
 *    the VELOCITY against the view (game 0x1f60f..0x1f7bb, vectoangles of the origin
 *    delta, clamped +-90), so it moves on almost every frame while running and turning -
 *    as a trigger it would restart the transition continuously.
 *  - Stand / crouch transitions time-blend the YAW slots only. The lean (the rolls, the
 *    lateral offset) and the pitch go through the limiter below exactly as they did
 *    through the engine's, so a peek started together with a strafe is never held back.
 *    With cod2x's full blend the model would show a fraction of the lean for the first
 *    ~100 ms while the camera is already out: a peeker's advantage. Prone transitions
 *    blend everything over 400 ms, as cod2x does - the whole body is re-posed there.
 *  - Outside a transition the engine limiter is reproduced unchanged for every slot but
 *    the yaws, which move as ONE group (all scaled by the same factor, so that the
 *    largest moves 0.36 deg/ms). Per-component clamping is the very thing that turns the
 *    chest off the view; as a group they only differ from the engine when it clamps, and
 *    the sum stays 0 even then - a flick, or a diagonal (no animation change) no longer
 *    twists the torso either.
 *  - It runs BEFORE the back-bone reprojection, so the reprojection is computed from the
 *    yaws that are actually drawn / tested, not from the targets.
 *  - It is timed on the GAME clock (cg.time / level.time), never on call counts or on the
 *    wall clock. The server poses a player on demand (a bullet trace, a tag query - see
 *    G_DObjCalcPose) and the engine limiter advanced by a whole frame on EVERY such call
 *    and not at all between them; the client poses once per rendered frame, only while
 *    the player is on screen. Here exactly one caller per side advances the state
 *    (LC_BLEND_COMMIT): the client's pose path, and on the server pose_sync_frame(), once
 *    per server frame for every player. Everything else only reads it (LC_BLEND_PEEK).
 *    The result also goes into ci->control, so the engine's own pass is a no-op.
 *  - No usable history (first sight, map change, demo seek, a hitch longer than
 *    LC_BLEND_STALE_MS, a respawn) = the engine's own pose, no transition - never a blend
 *    from something stale.
 */
typedef struct lc_blend_state_s {
    int   valid;
    int   type;                   /* lc_move_type() at the last committed step */
    int   t_last;                 /* game time of the last committed step */
    int   t_start;                /* start of the current / last transition */
    int   len;                    /* its length, ms; 0 = none yet */
    int   all;                    /* it blends every slot (prone), else the yaws only */
    float from[LC_FLOATS];        /* the pose it started from */
    float cur[LC_FLOATS];         /* the pose of the last committed step */
} lc_blend_state_t;

static lc_blend_state_t lc_bs[LC_MAX_CLIENTS];

/* The yaw of every vec3 in the buffer, tag_origin's included. */
static const int lc_yaw_slot[7] = { 1, 4, 7, 10, 13, 16, LC_TAG_ANG_YAW };

#define LC_MT_DEAD    0x1
#define LC_MT_CROUCH  0x2
#define LC_MT_PRONE   0x4
#define LC_MT_TURRET  0x8

static int lc_move_type(const void* es)
{
    const int ef  = lc_ri(es, LC_ES_EFLAGS);
    const int leg = lc_ri(es, LC_ES_LEGS_ANIM) & 0x3ff & ~LC_ANIM_TOGGLEBIT;
    return ((ef & LC_EF_DEAD)   ? LC_MT_DEAD   : 0)
         | ((ef & LC_EF_CROUCH) ? LC_MT_CROUCH : 0)
         | ((ef & LC_EF_PRONE)  ? LC_MT_PRONE  : 0)
         | ((ef & LC_EF_TURRET) ? LC_MT_TURRET : 0)
         | (leg << 4);
}

/* the engine's BG_LerpAngles, one component (game 0x1ab82, cgame 0x30005040) */
static float lc_limit(float target, float cur, float max_step)
{
    const float d = target - cur;
    if (d > max_step)  return cur + max_step;
    if (d < -max_step) return cur - max_step;
    return target;
}

/* out: the engine's pose for this instant (after the lateral top-up) in, the blended
 * pose out. Reads / advances lc_bs[] according to ctx->blend. */
static void lc_blend(float* out, const lc_ctx_t* ctx, int sampling)
{
    lc_blend_state_t* s;
    float res[LC_FLOATS];
    int   type, dt, all, in_win, restart, new_all, i;
    float f, max_a, max_o, max_d, k;
    const int t  = ctx->game_time;
    const int cn = ctx->client_num;

    if (!ctx->es || cn < 0 || cn >= LC_MAX_CLIENTS) return;
    s    = &lc_bs[cn];
    type = lc_move_type(ctx->es);

    /* No usable history: the engine's pose, and (on commit) a fresh state from it. A
     * respawn counts as one - dead -> alive is a teleport, not a movement. */
    if (!s->valid || t < s->t_last || t - s->t_last > LC_BLEND_STALE_MS ||
        ((s->type & LC_MT_DEAD) && !(type & LC_MT_DEAD))) {
        if (ctx->blend == LC_BLEND_COMMIT) {
            s->valid  = 1;
            s->type   = type;
            s->t_last = t;
            s->len    = 0;
            memcpy(s->cur, out, sizeof(s->cur));
        }
        if (sampling && ctx->log) {
            char line[160];
            snprintf(line, sizeof(line), "LC%d %c cn=%-2d BLEND type=0x%05x reset (t=%d)",
                     LC_VERSION, ctx->side == LC_SIDE_SERVER ? 'S' : 'C', cn, type, t);
            ctx->log(line);
        }
        return;
    }

    dt      = t - s->t_last;
    restart = (type != s->type);
    in_win  = s->len > 0 && t - s->t_start <= s->len;
    f       = in_win ? (float)(t - s->t_start) / (float)s->len : 1.0f;
    all     = in_win && s->all;
    max_a   = (float)dt * LC_LIMIT_DEG_PER_MS;
    max_o   = (float)dt * LC_LIMIT_UNITS_PER_MS;

    /* 1) THIS INSTANT UNDER THE CURRENT RULE - the running transition, or the limiter -
     *    toward the engine's pose of this instant. */

    /* everything but the yaws: the engine limiter, or the time blend in a prone window */
    for (i = 0; i < LC_TAG_OFF; ++i)
        res[i] = all ? s->from[i] * (1.0f - f) + out[i] * f
                     : lc_limit(out[i], s->cur[i], max_a);
    if (all) {
        for (i = LC_TAG_OFF; i < LC_FLOATS; ++i)
            res[i] = s->from[i] * (1.0f - f) + out[i] * f;
    } else {                              /* BG_LerpOffset: the vector, not per axis */
        const float dx = out[21] - s->cur[21], dy = out[22] - s->cur[22],
                    dz = out[23] - s->cur[23];
        const float l2 = dx * dx + dy * dy + dz * dz;
        k = (l2 > 0.0f) ? max_o / sqrtf(l2) : 1.0f;
        if (k >= 1.0f) k = 1.0f;
        res[21] = s->cur[21] + dx * k;
        res[22] = s->cur[22] + dy * k;
        res[23] = s->cur[23] + dz * k;
    }

    /* the yaws: time-blended together in a window, else limited together */
    if (in_win) {
        for (i = 0; i < 7; ++i) {
            const int j = lc_yaw_slot[i];
            res[j] = s->from[j] * (1.0f - f) + out[j] * f;
        }
    } else {
        max_d = 0.0f;
        for (i = 0; i < 7; ++i) {
            const int   j = lc_yaw_slot[i];
            const float d = fabsf(out[j] - s->cur[j]);
            if (d > max_d) max_d = d;
        }
        k = (max_d > max_a) ? max_a / max_d : 1.0f;
        for (i = 0; i < 7; ++i) {
            const int j = lc_yaw_slot[i];
            res[j] = s->cur[j] + (out[j] - s->cur[j]) * k;
        }
    }

    /* 2) A NEW MOVEMENT TYPE starts a new transition, from the pose shown at this instant.
     *    When one was already running, that pose is where it had got to by now (1): a
     *    quick succession of changes - an animation flickering on a threshold, strafe spam
     *    faster than 250 ms - keeps moving at the transition's own pace. Restarting from
     *    the last committed pose instead would not move at all on a change frame, and on
     *    the server, which commits once per frame, a change EVERY frame would freeze the
     *    pose outright: the old bug in a new place (test/test_ctrl_blend.c, case 5).
     *    When none was running it is exactly the last pose, so the first instant of a
     *    transition moves nothing whatever the frame length: that is what keeps a 50 ms
     *    server frame and a 7 ms client frame on the same curve. */
    new_all = ((type | s->type) & LC_MT_PRONE) != 0;
    if (restart && !in_win) {
        if (new_all) {
            memcpy(res, s->cur, sizeof(res));
        } else {
            for (i = 0; i < 7; ++i) res[lc_yaw_slot[i]] = s->cur[lc_yaw_slot[i]];
        }
    }

    if (ctx->blend == LC_BLEND_COMMIT) {
        if (restart) {
            memcpy(s->from, res, sizeof(s->from));
            s->type    = type;
            s->t_start = t;
            s->all     = new_all;
            s->len     = new_all ? LC_BLEND_MS_PRONE : LC_BLEND_MS;
        }
        memcpy(s->cur, res, sizeof(s->cur));
        s->t_last = t;
    }

    if (sampling && ctx->log) {
        char line[200];
        snprintf(line, sizeof(line),
                 "LC%d %c cn=%-2d BLEND type=0x%05x%s win=%s f=%.2f dt=%d t=%d "
                 "toY %+.2f->%+.2f",
                 LC_VERSION, ctx->side == LC_SIDE_SERVER ? 'S' : 'C', cn, type,
                 restart ? " NEW" : "", in_win ? (s->all ? "all" : "yaw") : "-", f, dt, t,
                 out[LC_TAG_ANG_YAW], res[LC_TAG_ANG_YAW]);
        ctx->log(line);
    }
    memcpy(out, res, sizeof(res));
}

/* ============================ dump ============================ */

static int lc_dump_gate(const lc_ctx_t* ctx, float lf)
{
    static unsigned last_ms[LC_MAX_CLIENTS];
    static int      last_key[LC_MAX_CLIENTS];
    static int      emitted[LC_MAX_CLIENTS];
    int cn, key;
    unsigned period;

    if (!ctx->dump || !ctx->log) return 0;
    cn = ctx->client_num;
    if (cn < 0 || cn >= LC_MAX_CLIENTS) return 0;
    if (emitted[cn] >= ctx->dump) return 0;

    /* stance plus the lean fraction in 0.05 steps: a held pose emits on the period, a
     * changing one emits on every step, and neither can flood a live server. */
    key = ctx->stance * 1000 + (int)(lf * 20.0f);
    period = (lf > LC_LEAN_EPS || lf < -LC_LEAN_EPS) ? LC_DUMP_PERIOD_MS
                                                     : LC_DUMP_IDLE_PERIOD_MS;

    if (key == last_key[cn] && (unsigned)(ctx->now_ms - last_ms[cn]) < period)
        return 0;

    last_key[cn] = key;
    last_ms[cn]  = ctx->now_ms;
    ++emitted[cn];
    return 1;
}

/* The inputs. If these differ between the two sides, the buffers will differ no matter
 * what this file does, and the cause is upstream: the client patches BG_PlayerAngles in
 * cgame ONLY (swing_fix.cpp - legs swingTolerance 40->0, torso yaw swingSpeed ->1.0,
 * torso yaw movefrac 0.3->0, lean swing speed 0.15->1.0), and every one of those writes
 * ci->legs.yawAngle / ci->torso.yawAngle / the lean swing channel, which are precisely
 * the fields the engine controller function reads. The server runs vanilla
 * BG_PlayerAngles. pose_sync.c already carries a switch for this (POSE_SYNC_YAW, default
 * off); this line is what tells us whether it needs to be on. */
static void lc_dump_inputs(const lc_ctx_t* ctx)
{
    char line[512];
    const char side = (ctx->side == LC_SIDE_SERVER) ? 'S' : 'C';

    if (!ctx->es || !ctx->ci) {
        snprintf(line, sizeof(line), "LC%d %c cn=%-2d IN  st=%s (es/ci not provided)",
                 LC_VERSION, side, ctx->client_num, lc_stance_name(ctx->stance));
        ctx->log(line);
        return;
    }

    snprintf(line, sizeof(line),
             "LC%d %c cn=%-2d IN  st=%s ef=0x%08x la=%d mvt=%d "
             "tH=%+.3f tP=%+.3f wP=%+.3f | "
             "legsY=%+.3f torsoY=%+.3f f3b8=%+.4f mvY=%+.3f lean=%+.4f "
             "pa=%+.2f/%+.2f/%+.2f",
             LC_VERSION, side, ctx->client_num, lc_stance_name(ctx->stance),
             (unsigned)lc_ri(ctx->es, LC_ES_EFLAGS),
             lc_ri(ctx->es, LC_ES_LEGS_ANIM) & 0x3ff,
             lc_ri(ctx->es, LC_ES_ANIM_MOVETYPE) & 0xf,
             lc_rf(ctx->es, LC_ES_FTORSO_HEIGHT),
             lc_rf(ctx->es, LC_ES_FTORSO_PITCH),
             lc_rf(ctx->es, LC_ES_FWAIST_PITCH),
             lc_rf(ctx->ci, LC_CI_LEGS_YAW),
             lc_rf(ctx->ci, LC_CI_TORSO_YAW),
             lc_rf(ctx->ci, LC_CI_UNK_3B8),
             lc_rf(ctx->ci, LC_CI_MOVEMENT_YAW),
             lc_rf(ctx->ci, LC_CI_LERP_LEAN),
             lc_rf(ctx->ci, LC_CI_PLAYER_ANGLES + 0),
             lc_rf(ctx->ci, LC_CI_PLAYER_ANGLES + 4),
             lc_rf(ctx->ci, LC_CI_PLAYER_ANGLES + 8));
    ctx->log(line);
}

void lc_dump(const float* out, const lc_ctx_t* ctx, const char* stage)
{
    char line[640];
    int  n;
    unsigned i;
    static const char* const names[8] = {
        "bl", "bm", "bu", "nk", "hd", "pv", "toA", "toO"
    };

    if (!out || !ctx || !ctx->log) return;

    n = snprintf(line, sizeof(line), "LC%d %c cn=%-2d %-6s lf=%+.4f",
                 LC_VERSION,
                 (ctx->side == LC_SIDE_SERVER) ? 'S' : 'C',
                 ctx->client_num,
                 stage ? stage : "?",
                 out[LC_TAG_ANG_ROLL] * (1.0f / LC_LEAN_ROLL_PER_FRAC));
    if (n < 0 || (unsigned)n >= sizeof(line)) return;

    for (i = 0; i < 8; ++i) {
        int k = snprintf(line + n, sizeof(line) - (unsigned)n,
                         " %s=%+.3f,%+.3f,%+.3f",
                         names[i], out[i * 3 + 0], out[i * 3 + 1], out[i * 3 + 2]);
        if (k < 0 || (unsigned)(n + k) >= sizeof(line)) break;
        n += k;
    }
    ctx->log(line);
}

/* ============================ entry point ============================ */

int lc_apply(float* out, const lc_ctx_t* ctx)
{
    float lf;
    int   sampling;

    if (!out || !ctx) return 0;

    lf = out[LC_TAG_ANG_ROLL] * (1.0f / LC_LEAN_ROLL_PER_FRAC);

    sampling = lc_dump_gate(ctx, lf);
    if (sampling) {
        lc_dump_inputs(ctx);
        lc_dump(out, ctx, "pre");
    }

    /* PRONE: no lateral top-up and no reprojection, on both sides (the blend, 2, still
     * runs: a prone transition is the biggest pose change there is).
     * This was a live divergence until now and it was never on anyone's list: the client
     * returned before every adjustment (lean_fix.cpp:130) while the server only skipped
     * the lateral shift and still ran the back-bone reprojection (pose_sync.c `goto diag`
     * from the prone branch). A prone player's tested skeleton was therefore reprojected
     * while the drawn one was not. Resolved in favour of what production draws today,
     * since that is the pose testers validated. */
    if (ctx->stance == LC_STANCE_PRONE) {
        if (ctx->blend != LC_BLEND_OFF) {
            lc_blend(out, ctx, sampling);
            if (ctx->control) memcpy(ctx->control, out, LC_FLOATS * sizeof(float));
        }
        if (sampling) lc_dump(out, ctx, "post");
        return sampling;
    }

    /* 1) LATERAL BODY SHIFT - top this host up to the shared total.
     *
     * The engine already applied ctx->engine_lateral units of its own (out[22] +=
     * -fLeanFrac * that), with a different constant baked into each module, so the two
     * sides start from different buffers. Subtracting is what makes one shared line
     * produce the same final number on both:
     *     client: 2.5 engine + 5.0 here = 7.5
     *     server: 7.5 engine + 0.0 here = 7.5
     * No stance branch and no per-side branch. That is not a simplification, it is the
     * requirement: anything that varies here is something the other side has to be told
     * about out of band, and every such variation has cost us a hitbox desync. The old
     * per-stance table (crouch-left 12.5 against 5.0 everywhere else) is exactly how the
     * server ended up owning a permanent hand-maintained mirror of a client constant. */
    if ((lf > LC_LEAN_EPS || lf < -LC_LEAN_EPS) &&
        !(lf > LC_LEAN_SANE_MAX || lf < -LC_LEAN_SANE_MAX)) {
        out[LC_TAG_OFF_Y] += -lf * (LC_LATERAL_TOTAL - ctx->engine_lateral);
    }

    /* 2) BLEND (v2) - see lc_blend(). Between the two, on purpose: the top-up is part of
     * the pose being blended (the engine limiter used to see it too), and the reprojection
     * below must work from the yaws that end up drawn / tested, not from the targets. */
    if (ctx->blend != LC_BLEND_OFF) lc_blend(out, ctx, sampling);

    /* 3) BACK-BONE REPROJECTION - unconditional, like both sides do today.
     * tag_origin yaw is read once up front; lc_adjust_rotation only writes [0] and [2],
     * so the later bones' yaw is unaffected by the earlier ones. */
    {
        const float toy = out[LC_TAG_ANG_YAW];
        lc_adjust_rotation((out[LC_BACK_LOW + 1] - toy) * LC_DIAG_K, &out[LC_BACK_LOW]);
#if LC_DIAG_BONES >= 2
        lc_adjust_rotation((out[LC_BACK_MID + 1] - toy) * LC_DIAG_K, &out[LC_BACK_MID]);
#endif
#if LC_DIAG_BONES >= 3
        lc_adjust_rotation((out[LC_BACK_UP + 1] - toy) * LC_DIAG_K, &out[LC_BACK_UP]);
#endif
    }

    /* The engine limiter that follows compares the buffer with ci->control and moves
     * control toward it: equal, it is a no-op, and what is drawn / tested is exactly this
     * buffer. Our blend state keeps the pre-reprojection pose; control gets the final one,
     * which is the only thing the engine reads it for (DObjSetControlTagAngles /
     * DObjSetLocalTag, right after the limiter). */
    if (ctx->blend != LC_BLEND_OFF && ctx->control)
        memcpy(ctx->control, out, LC_FLOATS * sizeof(float));

    if (sampling) lc_dump(out, ctx, "post");
    return sampling;
}

const char* lc_banner(void)
{
    /* Built once, on first call; the string is a pure function of the constants above, so
     * two builds from identical sources produce identical banners and two builds from
     * drifted sources do not. */
    static char s[240];
    if (!s[0]) {
        snprintf(s, sizeof(s),
                 "lean_controllers v%d lateral_total=%.2f roll_per_frac=%.3f eps=%.3f "
                 "sane=%.2f diag=%d/%.2f blend=%d/%dms stale=%d lim=%.2f/%.2f yawgroup",
                 LC_VERSION, LC_LATERAL_TOTAL, LC_LEAN_ROLL_PER_FRAC, LC_LEAN_EPS,
                 LC_LEAN_SANE_MAX, LC_DIAG_BONES, LC_DIAG_K, LC_BLEND_MS, LC_BLEND_MS_PRONE,
                 LC_BLEND_STALE_MS, LC_LIMIT_DEG_PER_MS, LC_LIMIT_UNITS_PER_MS);
    }
    return s;
}
