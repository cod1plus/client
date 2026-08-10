/*
 * lean_controllers.h - the ONE implementation of our controller-buffer adjustments,
 *                      shared by mss32.dll (client) and cod1plus.so (server).
 *
 * >>> THIS FILE AND lean_controllers.c EXIST TWICE ON DISK, BYTE-IDENTICAL:
 * >>>     cod1reloaded/src/shared/          (compiled into mss32.dll)
 * >>>     cod1plushookserver/src/shared/    (compiled into cod1plus.so)
 * >>> They are two git repos, and the server is built on a VPS from its own checkout,
 * >>> so a symlink or a relative include is not available. Both sides print lc_banner()
 * >>> at install: if the two banners in the two logs are not the same string, the copies
 * >>> have drifted and NOTHING below is trustworthy. That check is the whole reason the
 * >>> banner exists - do not remove it, and copy BOTH files whenever either changes.
 *
 * WHY IT EXISTS
 * cod2x has no lean hitbox desync for one structural reason: it does not maintain two
 * implementations. It rewrites BG_Player_DoControllersInternal once in
 * src/shared/animation.cpp and repoints both call sites at it (animation.cpp:1464 server
 * in G_PlayerController, :1466 client in CG_Player_DoControllers, via patch_call plus two
 * ABI shims _Win32/_Linux). Drawn == tested by construction.
 *
 * CoD1 splits the same bg code across cgame_mp_x86.dll and game.mp.i386.so. We do NOT
 * reimplement the engine function - our hooks are additive and both are already proven in
 * production (lean_fix.cpp client, pose_sync.c server). What was missing is that the two
 * hooks ran different code, tuned independently, so aligning one divergence always left
 * another. This file is the small, tractable version of the cod2x move: only OUR deltas
 * live here, and both hooks call them.
 *
 * WHAT THE HOST STILL OWNS (and why)
 *   - installing the hook and getting the buffer pointer;
 *   - deciding the STANCE. The two sides genuinely cannot share this: the client reads
 *     es->eFlags & 0x20 (measured 2026-08-10 over ~30 stance changes on two clients),
 *     the server reads the gentity bounding-box height because an eFlags read came back
 *     wrong there on 2026-08-09. Rather than bake a guess into the file that is supposed
 *     to be authoritative, stance comes in as a parameter and the dump prints both the
 *     stance and the raw eFlags so the disagreement, if any, is visible instead of
 *     assumed.
 *   - telling us what its OWN engine already applied laterally (engine_lateral).
 *
 * PROVENANCE of every offset and constant: src/shared/anim_shared.h, itself read out of
 * game.mp.i386.so md5 343f99cd67b79ac74aeaa5261f63c011 and the engine's entityState
 * netfield table. Nothing here is inferred from CoD2.
 */

#ifndef COD1_LEAN_CONTROLLERS_H
#define COD1_LEAN_CONTROLLERS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on ANY change to the shared arithmetic. Both sides print it; a mismatch in the
 * two logs means one repo was copied and the other was not. */
#define LC_VERSION 1

#define LC_SIDE_CLIENT 0
#define LC_SIDE_SERVER 1

#define LC_STANCE_STAND  0
#define LC_STANCE_CROUCH 1
#define LC_STANCE_PRONE  2

/* ---- the controller buffer: 24 floats / 8 vec3 ----------------------------------
 * Proven by the tail of the engine function (game 0x1aa87..0x1ab75): a 0..5 loop copies
 * six vec3 to out+i*12, then legsAngles -> out+0x48 and the offset vec3 -> out+0x54.
 * Same order the client already indexes (lean_fix.h:59-67). */
#define LC_BACK_LOW       0
#define LC_BACK_MID       3
#define LC_BACK_UP        6
#define LC_NECK           9
#define LC_HEAD          12
#define LC_PELVIS        15
#define LC_TAG_ANG       18   /* [pitch, yaw, roll] */
#define LC_TAG_ANG_YAW   19
#define LC_TAG_ANG_ROLL  20
#define LC_TAG_OFF       21   /* [x, y, z] */
#define LC_TAG_OFF_Y     22
#define LC_FLOATS        24

/*
 * Everything the shared code needs. The host fills it per invocation; nothing is cached
 * across calls except the dump throttle, which is keyed on client_num.
 */
typedef struct lc_ctx_s {
    int          side;            /* LC_SIDE_CLIENT | LC_SIDE_SERVER - dump label only */
    int          client_num;      /* es->clientNum (es+0x90) */
    int          stance;          /* LC_STANCE_* - see "WHAT THE HOST STILL OWNS" above */

    /* Lateral units this host's OWN engine copy already put in out[22] at fLeanFrac=1.
     * client: 2.5 (cgame's constant; MSVC pooled it across five readers, so it cannot be
     *              patched without corrupting four unrelated computations)
     * server: 7.5 (game .rodata 0x73d5c, retuned in place by COD1RELOADED_LEAN_CONST;
     *              GCC did not pool - that literal has exactly one reader)
     * lc_apply() tops each side up to the same LC_LATERAL_TOTAL, so the invariant that
     * used to be a comment in two files is now one subtraction in one file. */
    float        engine_lateral;

    unsigned int now_ms;          /* any monotonic ms clock; used only to throttle dumps */

    const void*  es;              /* entityState_s* - READ-ONLY, dump only. May be NULL. */
    const void*  ci;              /* clientInfo_t*  - READ-ONLY, dump only. May be NULL. */

    int          dump;            /* 0 = off, else max dump samples per client */
    void       (*log)(const char* line);   /* host sink: logger::logf / printf */
} lc_ctx_t;

/* Apply our deltas to the buffer the engine just produced. No allocation, no call back
 * into the game. Dumps "pre"/"post" when ctx->dump is set and the throttle allows.
 * Returns non-zero when it emitted a sample, so a host with a further stage of its own
 * can dump that stage in the same sample instead of on its own schedule. */
int lc_apply(float* out, const lc_ctx_t* ctx);

/* Emit one labelled sample of the buffer. Exposed separately so the client can also dump
 * AFTER its controller smoothing (a client-only stage the server has no counterpart for -
 * see lean_controllers.c, "STAGES"). Bypasses the throttle: call it only when lc_apply
 * has just sampled, or you will flood the log. */
void lc_dump(const float* out, const lc_ctx_t* ctx, const char* stage);

/* Identity of this exact shared build: version plus every constant that governs the
 * arithmetic. Print it on both sides at install and compare the two log lines. */
const char* lc_banner(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* COD1_LEAN_CONTROLLERS_H */
