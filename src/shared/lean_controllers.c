/*
 * lean_controllers.c - see lean_controllers.h for why this file exists and for the
 *                      "IT EXISTS TWICE, KEEP THE TWO COPIES IDENTICAL" rule.
 *
 * STAGES
 * The engine function runs first on both sides; we only post-process its buffer:
 *
 *   engine BG_Player_DoControllersInternal   ->  "pre"
 *   lc_apply(): lateral top-up, then the back-bone reprojection   ->  "post"
 *   client only: apply_ctrl_smooth() temporal filter              ->  "smooth"
 *
 * The third stage has no server counterpart and this file deliberately does NOT own it:
 * it is a display filter with per-client history and a per-frame velocity clamp, so it is
 * not a pure function of the state the server has. It is the one client-only stage left
 * after this file lands, and it is why lc_dump() is exported - dumping "smooth" next to
 * "post" is what will show whether it moves the drawn pose away from the tested one, or
 * whether it converges as intended when the player holds a lean still.
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
#define LC_ES_LEGS_ANIM        0xcc
#define LC_ES_ANIM_MOVETYPE    0xe0
#define LC_ES_FTORSO_HEIGHT    0xe4
#define LC_ES_FTORSO_PITCH     0xe8
#define LC_ES_FWAIST_PITCH     0xec

#define LC_CI_LEGS_YAW         0x380  /* read by the engine at 0x1a438 */
#define LC_CI_TORSO_YAW        0x3b0  /* read by the engine at 0x1a444 */
#define LC_CI_UNK_3B8          0x3b8  /* read ONCE at 0x1a477, semantics unknown.
                                       * lean_fix.h labels this "lerpLean" - that label is
                                       * stale (the value the engine feeds GetLeanFraction
                                       * is at 0x3e4, read at 0x1a543) and nothing reads
                                       * the constant, but it is dumped here so the
                                       * measurement settles it instead of a third file
                                       * inheriting the wrong name. */
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

    /* PRONE: apply nothing at all, on both sides.
     * This was a live divergence until now and it was never on anyone's list: the client
     * returned before every adjustment (lean_fix.cpp:130) while the server only skipped
     * the lateral shift and still ran the back-bone reprojection (pose_sync.c `goto diag`
     * from the prone branch). A prone player's tested skeleton was therefore reprojected
     * while the drawn one was not. Resolved in favour of what production draws today,
     * since that is the pose testers validated. */
    if (ctx->stance == LC_STANCE_PRONE) {
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

    /* 2) BACK-BONE REPROJECTION - unconditional, like both sides do today.
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

    if (sampling) lc_dump(out, ctx, "post");
    return sampling;
}

const char* lc_banner(void)
{
    /* Built once, on first call; the string is a pure function of the constants above, so
     * two builds from identical sources produce identical banners and two builds from
     * drifted sources do not. */
    static char s[160];
    if (!s[0]) {
        snprintf(s, sizeof(s),
                 "lean_controllers v%d lateral_total=%.2f roll_per_frac=%.3f eps=%.3f "
                 "sane=%.2f diag=%d/%.2f",
                 LC_VERSION, LC_LATERAL_TOTAL, LC_LEAN_ROLL_PER_FRAC, LC_LEAN_EPS,
                 LC_LEAN_SANE_MAX, LC_DIAG_BONES, LC_DIAG_K);
    }
    return s;
}
