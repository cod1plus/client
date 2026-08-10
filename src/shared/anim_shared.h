/*
 * anim_shared.h - CoD1 animation-controller field map, shared by BOTH sides.
 *
 * WHY THIS FILE EXISTS
 * cod2x has no lean hitbox desync because it does not maintain two implementations: it
 * reimplements BG_Player_DoControllersInternal once in src/shared/animation.cpp and
 * repoints BOTH call sites at it (animation.cpp:1464 server / :1466 client, via a
 * patch_call and two thin ABI shims _Win32 / _Linux). Drawn == tested by construction,
 * which is why cod2x can afford per-stance and per-side branches (12.5 / 2.5 / 5.0 / 2.5)
 * that have repeatedly desynced us.
 *
 * CoD1 splits the same bg code into two modules - cgame_mp_x86.dll and game.mp.i386.so -
 * and we hook both already (lean_fix.cpp client, pose_sync.c server). The architecture is
 * in place; what is missing is that the two hooks run DIFFERENT code. This header is the
 * first half of fixing that: one field map, used by one shared implementation, compiled
 * into mss32.dll and cod1plus.so.
 *
 * SCOPE NOTE - we do NOT need the full structs. cod2x replaces the whole engine function
 * so it needs every field. We keep our additive hooks, so the shared code only needs the
 * handful of fields below. That is the difference between a multi-week port and a
 * tractable one.
 *
 * PROVENANCE: every offset here was read out of game.mp.i386.so
 * (md5 343f99cd67b79ac74aeaa5261f63c011) by disassembling
 * BG_Player_DoControllersInternal, RVA 0x1a389..0x1ad71, 713 instructions, on 2026-08-10.
 * Nothing here is inferred from CoD2. Confirmed-in-game facts are marked; guesses are
 * marked UNCONFIRMED and must not be relied on until checked.
 */

#ifndef COD1_ANIM_SHARED_H
#define COD1_ANIM_SHARED_H

/* ---------------- entityState_s ----------------
 * NAMES AND OFFSETS ARE AUTHORITATIVE: read out of the engine's own entityState netfield
 * table (CoDMP.exe file 0x180110, 60 entries of {const char* name, int offset, int bits}).
 * That table is what the engine delta-compresses against, so it is the real struct, not a
 * reconstruction. The .so does not carry these strings - netfields live in the engine.
 * Cross-checked against the disassembly: fTorsoHeight is read exactly once (0x1a537), and
 * cod2x likewise uses it once (VectorSet(tag_origin_offset, 0,0, es->fTorsoHeight),
 * animation.cpp:178), while fTorsoPitch/fWaistPitch are read 7 and 6 times, always paired,
 * across the stand and prone branches. */
#define ES_EFLAGS            0x08   /* 24 bits.
                                     * CROUCH = 0x20, MEASURED 2026-08-10: ~30 stance
                                     * changes on two clients, bit 0x20 clear on every
                                     * boxh=70 sample and set on every boxh=50 one, no
                                     * exception. 0x4000 was NEVER set in any sample - it
                                     * is the CoD2 value and was imported here by mistake;
                                     * that error is also why the 2026-08-09 "eFlags reads
                                     * FALSE on a crouched player" result looked like the
                                     * field was unusable. It was the mask that was wrong.
                                     * PRONE is still left at the CoD2 0x8000 and is NOT
                                     * verified - no prone sample was captured. Probe
                                     * before trusting it.
                                     * The engine's own memset gate at 0x1a39f tests
                                     * eFlags & 0xc000, which is therefore NOT crouch:
                                     * a crouched leaner's buffer is not zeroed, and
                                     * crouch is a live case. Server-side this file's
                                     * consumers still use the bounding-box height
                                     * (GE_MINS/GE_MAXS below), which is independently
                                     * reliable. */
#define ES_CLIENTNUM         0x90   /* 8 bits;  read at 0x1a3d5 */
#define ES_LEGS_ANIM         0xcc   /* 10 bits; index into bgs animData.animations[].
                                     * cod2x reads .flags & 0x10 / & 0x20 for
                                     * isMovingLeft / isMovingRight (animation.cpp:302). */
#define ES_TORSO_ANIM        0xd0   /* 10 bits */
#define ES_LEANF             0xd4   /* float; the networked lean. Distinct from
                                     * CI_LERP_LEAN - this one is the entityState mirror. */
#define ES_ANIM_MOVETYPE     0xe0   /* 4 bits only - a discrete 8-direction index, which is
                                     * why CoD1 movementDir cannot carry a continuous
                                     * velocity angle the way cod2x's does. */
#define ES_FTORSO_HEIGHT     0xe4   /* float; -> tag_origin_offset[2]  (read once, 0x1a537) */
#define ES_FTORSO_PITCH      0xe8   /* float; -> tag_origin_angles[0]  (7 uses) */
#define ES_FWAIST_PITCH      0xec   /* float; paired with fTorsoPitch  (6 uses) */

/* Server-side stance detection that actually works: gentity bounding-box height.
 * Measured live: 70 standing, 50 crouched. Prone is lower still. */
#define GE_MINS              0x104
#define GE_MAXS              0x110
#define STANCE_H_CROUCH      55.0f
#define STANCE_H_PRONE       40.0f

/* ---------------- clientInfo_t (stride 0x4b0) ----------------
 * This is a bg struct: the SAME layout in both modules. Cross-validated - the offsets
 * below were read from game.mp.i386.so, and lean_fix.h independently uses 0x3e0 and the
 * controller offsets 0x48/0x54 against cgame_mp_x86.dll. They agree. */
#define CI_LEGS_YAW          0x380  /* legs.yawAngle  - 0x1a438 */
#define CI_TORSO_YAW         0x3b0  /* torso.yawAngle - 0x1a444 */
#define CI_TORSO_F1          0x3b8  /* read once at 0x1a477. UNCONFIRMED semantics */
#define CI_MOVEMENT_YAW      0x3e0  /* movementYaw. cod2x gates its diagonal lean pose on
                                     * this (animation.cpp:332, 0 < yaw < 90). Already used
                                     * client-side as lean_fix.h CI_MOVEMENTYAW_OFFSET. */
#define CI_LERP_LEAN         0x3e4  /* the value fed to GetLeanFraction - 0x1a543.
                                     * Clamped to +/-0.5 by PM_UpdateLean (0x25cf5). */
#define CI_PLAYER_ANGLES     0x3e8  /* vec3: [0]=0x3e8 [1]=0x3ec(yaw) [2]=0x3f0.
                                     * NOT 0x3e0 - that is movementYaw. ClientEndFrame
                                     * 0x3b00c copies ps.viewangles[0..2] here. */

/* ---------------- animation table (bgs.animData.animations[]) ----------------
 * From BG_GetAnimationForIndex, game 0x189f6:
 *     cmp  index, [base + 0xb800]          ; count sits right after the array
 *     imul index, index, 0x5c              ; STRIDE
 *     add  index, [got - 0x8d90]           ; + array base
 * 0xb800 / 0x5c == 512 exactly, so the array is animation_t[512] followed by the count.
 * (The ebx here computes to 0x88d98 - the same GOT independently derived from the lean
 * constant's -0x1503c displacement. Two unrelated derivations, one value.) */
#define ANIM_STRIDE          0x5c   /* 92 bytes. cod2x's CoD2 animation_t is 0x60 on Linux,
                                     * so CoD1 has exactly one field fewer - which one is
                                     * NOT determined, and we do not need to know. */
#define ANIM_COUNT_OFF     0xb800   /* from the array base */
#define ANIM_ARRAY_PTR_GOTREL -0x8d90

/* DIRECTION OF MOVEMENT - do NOT port cod2x's method.
 * cod2x derives isMovingLeft/isMovingRight from animations[legsAnim].flags & 0x10 / 0x20
 * (animation.cpp:302). A scan of the whole of game.mp.i386.so .text found ZERO tests of
 * bits 0x10/0x20 against any plausible animation-struct offset: CoD1 does not carry those
 * flags. Chasing the flags field further is chasing something that is not there.
 *
 * CoD1 hands us the same information more directly, in ES_ANIM_MOVETYPE (es+0xe0): a 4-bit
 * discrete direction index, idle plus 8 directions. It is better suited to shared code than
 * the CoD2 route for a structural reason - it lives in the ENTITY STATE, so it is networked,
 * and both sides therefore read the same value by construction rather than by agreement.
 *
 * STILL TO DO: the meaning of the 16 values (the ANIM_MT_* enum order) is not established.
 * Resolve it empirically - log es->animMovetype while walking in each of the 8 directions,
 * standing and crouched. That is minutes of work; more disassembly here is not warranted. */

/* ---------------- clientControllers_t: 24 floats / 8 vec3 ----------------
 * Proven by the function tail at 0x1ab42..0x1ab78, which copies six stack locals into
 * out+0x48..0x5c, and by the 0..5 loop before it that writes out+i*12. */
#define CB_BACK_LOW          0    /* floats 0..2   */
#define CB_BACK_MID          3
#define CB_BACK_UP           6
#define CB_NECK              9
#define CB_HEAD             12
#define CB_PELVIS           15
#define CB_TAG_ORIGIN_ANG   18    /* out+0x48; [2]=roll=float 20 carries the lean */
#define CB_TAG_ORIGIN_OFF   21    /* out+0x54; [1]=lateral=float 22 carries the shift */
#define CB_TAG_ANG_PITCH    18
#define CB_TAG_ANG_YAW      19
#define CB_TAG_ANG_ROLL     20
#define CB_TAG_OFF_X        21
#define CB_TAG_OFF_Y        22
#define CB_TAG_OFF_Z        23
#define CB_FLOATS           24

/* ---------------- the lean arithmetic the engine already performs ----------------
 * GetLeanFraction(x) = (2 - |x|) * x   (game 0x71a17). lerpLean is clamped to +/-0.5,
 * so a full lean gives fLeanFrac = 0.75, NOT 1.0. Getting this wrong over-injects by 33%.
 *
 *   out[20] = fLeanFrac * 50.0 * 0.075          (consts .rodata 0x73d30 / 0x73da4)
 *   out[22] += -fLeanFrac * LEAN_LATERAL_UNITS  (const .rodata 0x73d5c, read ONLY at
 *                                                0x1a593: flds -0x1503c(%ebx), GOT-rel,
 *                                                preceded by fchs, followed by fmulp)
 *
 * Recovering fLeanFrac inside a hook: read out[20] / LEAN_ROLL_PER_FRAC. This cannot be
 * wrong - it is literally the number, not a re-derivation from lerpLean. */
#define LEAN_ROLL_PER_FRAC   3.75f   /* 50.0 * 0.075 */
#define LEAN_FRAC_FULL       0.75f
#define LEAN_EPS             0.02f

/* The lateral constant, per side of the wire. These MUST satisfy:
 *     CLIENT_ENGINE_LATERAL + CLIENT_HOOK_LATERAL == SERVER_LATERAL
 * or leaning heads stop registering, silently, with nothing logged.
 *
 * Client: MSVC POOLED its float literals - cgame_mp_x86.dll holds exactly ONE 2.5f
 * (VA 0x3006b6bc) read by FIVE different sites, so the client constant CANNOT be patched
 * without corrupting four unrelated computations. The client side must stay a hook.
 * Server: GCC did NOT pool - five separate 2.5f literals, ours has exactly one reader,
 * so .rodata 0x73d5c can be retuned in place (COD1RELOADED_LEAN_CONST). */
#define CLIENT_ENGINE_LATERAL   2.5f   /* cgame's own constant - unpatchable, see above */
#define CLIENT_HOOK_LATERAL     5.0f   /* lean_fix.cpp #3 - one value, no branches */
#define SERVER_LATERAL          7.5f   /* game .rodata 0x73d5c, patched at startup */

#endif /* COD1_ANIM_SHARED_H */
