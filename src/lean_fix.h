#ifndef COD1RELOADED_LEAN_FIX_H
#define COD1RELOADED_LEAN_FIX_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// cgame_mp_x86.dll preferred load base
constexpr uintptr_t CGAME_PREFERRED_BASE = 0x30000000;

// RVAs identifies par RE (cgame_mp_x86.dll, voir docs/lean_fix_reference.md
// pour le doc maitre)
constexpr uintptr_t CGAME_DOCONTROLLERS_INTERNAL_RVA = 0x00004960; // calc des 6 vec3
constexpr uintptr_t CGAME_DOCONTROLLERS_CALL_SITE_RVA = 0x000051d8; // call site dans CG_Player_DoControllers

// Confirmes par RE :
constexpr uintptr_t CGAME_OFFSET_FPV_STAND_RVA  = 0x00034180; // CG_OffsetFirstPersonView STAND
constexpr uintptr_t CGAME_OFFSET_FPV_CROUCH_RVA = 0x00038300; // 2e reader de leanf (crouch/prone)
constexpr uintptr_t CGAME_ADD_LEAN_TO_POSITION_RVA = 0x00040df0; // BG_AddLeanToPosition

// Convertit ci->lerpLean (un angle accumulator, 0 a ~5 deg) en fLeanFrac
// propre [-1, +1] signe pour LEFT/RIGHT. Appele par DoControllersInternal
// a 0x300049c5 quand le bit lean est set sur le bone controller. Convention :
//   float fcn_3003db10(float lerpLean);  // cdecl, ret in ST(0)
constexpr uintptr_t CGAME_GET_LEAN_FRAC_RVA = 0x0003db10;

// Offsets dans le clientInfo (passe via ecx, 1200 bytes / 0x4b0)
constexpr uintptr_t CI_LERPLEAN_OFFSET = 0x3b8;

// VRAI leanf (pure lean fraction, signed [-1, 1]) du LOCAL player.
// Trouve via Cutter dans cgame : `data.3020af54` est lu comme leanFrac
// (3eme arg) par les call sites de AddLeanToPosition (e.g. 0x34473,
// 0x38xxx). C'est cg.predictedPlayerState.leanf ou equivalent.
// Aucune ecriture visible dans cgame (rempli par snapshot, read-only).
constexpr uintptr_t CG_LOCAL_LEANF_RVA = 0x20af54;

// Offsets dans l'entite (passe via ebx)
constexpr uintptr_t ENT_EFLAGS_OFFSET = 0x8;

// Bits dans eFlags (haut octet teste via `test ah, 0xc0`)
// NB : le bit 0x40 (`test al, 0x40` a 0x300049ba dans DoControllersInternal)
// n'est PAS un signal "is_leaning_key" fiable : toujours 0 in-game meme
// pendant un lean visible. Garde la constante pour reference.
constexpr uint32_t ENT_FLAG_LEANING_QUESTIONABLE = 0x40;
constexpr uint32_t ENT_FLAG_CROUCH = 0x4000;
constexpr uint32_t ENT_FLAG_PRONE  = 0x8000;

// Offsets dans le buffer controllers (vec3 = 12 bytes chacun).
// L'ordre vient de la table de bone-names a cgame+0x79670, lue par le
// loop d'application dans CG_Player_DoControllers : back_low en premier,
// pelvis en dernier. (Initialement j'avais pris l'ordre inverse, ce qui
// faisait appliquer la correction au PELVIS et faire pivoter le corps
// entier - effet "rouler sur soi-meme tete-en-bas".)
constexpr uintptr_t CTRL_BACK_LOW_ANGLES = 0x00;
constexpr uintptr_t CTRL_BACK_MID_ANGLES = 0x0c;
constexpr uintptr_t CTRL_BACK_UP_ANGLES  = 0x18;
constexpr uintptr_t CTRL_NECK_ANGLES     = 0x24;
constexpr uintptr_t CTRL_HEAD_ANGLES     = 0x30;
constexpr uintptr_t CTRL_PELVIS_ANGLES   = 0x3c;

// Layout apres les 6 bones (confirme via cod2x cod2_player.h, meme engine
// id Tech 3 derivative donc layout identique CoD1/CoD2) :
//   0x48 : tag_origin_angles  (vec3 pitch/yaw/roll) - root orientation
//   0x54 : tag_origin_offset  (vec3 x/y/z)          - root position
// NB : on avait initialement labellise 0x48 comme "offset" -> erreur,
// mais inoffensive car les seuls writes etaient avec une valeur 0.0.
constexpr uintptr_t CTRL_TAG_ORIGIN_ANGLES = 0x48;  // vec3 [pitch, yaw, roll]
constexpr uintptr_t CTRL_TAG_ORIGIN_OFFSET = 0x54;  // vec3 [x, y, z]

// Parametres ajustables via cod1reloaded.ini
//
// Mecanisme de fix : on damp (multiplie par un facteur < 1) le YAW du neck
// et du head dans le buffer ecrit par le calculator. Pendant un lean+strafe,
// ces yaws montent a 16+ deg, ce qui fait visuellement rentrer la tete dans
// le torse. En les attenuant on ramene la tete dans l'axe du corps.
//
// scale = 1.0 : pas de modif (vanilla)
// scale = 0.5 : reduit le yaw de moitie
// scale = 0.0 : annule completement le yaw du bone (tete fixe dans l'axe)
struct LeanFixConfig {
    bool  enable;
    bool  apply_in_stand;     // appliquer aussi en debout
    int   diag_log_count;     // logge les N premieres invocations (0 = off)

    // Damping pitch (axe 0 du vec3). Pendant un lean+forward, l'engine
    // applique un pitch positif sur les back bones -> le buste s'incline
    // VERS L'AVANT (= "rolls forward / person looks shorter"). Sur LEFT
    // lean en particulier, ce pitch est exagere et le head peut disparaitre
    // hors du champ de vision. Damping pitch redresse le buste vertical.
    float neck_pitch_scale;
    float head_pitch_scale;
    float back_up_pitch_scale;
    float back_mid_pitch_scale;
    float back_low_pitch_scale;
    float pelvis_pitch_scale;

    // Damping yaw (axe 1 du vec3). Pendant un lean+strafe, le calculator
    // ecrit -14 deg de yaw sur back_low/back_mid - c'est la racine du
    // "twist" qui donne l'effet carapace. Reduire ces deux la a 0.4-0.5.
    // PELVIS est le bone parent : c'est lui qui porte le pivot quand on
    // strafe+forward (legs->pelvis->...->head). Damping pelvis_yaw redresse
    // le corps SANS toucher la transition legs->torse.
    float neck_yaw_scale;
    float head_yaw_scale;
    float back_up_yaw_scale;
    float back_mid_yaw_scale;
    float back_low_yaw_scale;
    float pelvis_yaw_scale;

    // Damping roll (axe 2 du vec3). Pendant un lean, le calculator ecrit
    // -11 deg de roll sur back_low/back_mid et +13 deg sur back_up
    // (contre-rotation en S). Si l'effet "courbe" est trop visible, dampe
    // ces rolls aussi.
    float neck_roll_scale;
    float head_roll_scale;
    float back_up_roll_scale;
    float back_mid_roll_scale;
    float back_low_roll_scale;
    float pelvis_roll_scale;

    // LEFT-lean specific overrides. id Tech 3's animation rig is NOT
    // perfectly mirrored : the same dampening applied to right lean can
    // produce "rolls forward + head disappears" on left lean (asymmetric
    // bone weight). These knobs let you tune left side independently.
    //
    // Convention : the values above are used for RIGHT lean (lerpLean > 0).
    // For LEFT lean (lerpLean < 0), if any of the *_left fields below is
    // >= 0, it OVERRIDES the corresponding base value. A value < 0 means
    // "use the base scale" -> backward compatible default behavior.
    float left_neck_pitch_scale;
    float left_head_pitch_scale;
    float left_back_up_pitch_scale;
    float left_back_mid_pitch_scale;
    float left_back_low_pitch_scale;
    float left_pelvis_pitch_scale;
    float left_neck_yaw_scale;
    float left_head_yaw_scale;
    float left_back_up_yaw_scale;
    float left_back_mid_yaw_scale;
    float left_back_low_yaw_scale;
    float left_pelvis_yaw_scale;
    float left_neck_roll_scale;
    float left_head_roll_scale;
    float left_back_up_roll_scale;
    float left_back_mid_roll_scale;
    float left_back_low_roll_scale;
    float left_pelvis_roll_scale;

    // YAW OFFSETS (additive degrees, scaled linearly by lean strength).
    //
    // Discovery from diag logs : during STATIONARY lean (no strafe), the
    // engine writes near-zero yaws on all bones. Scales multiply zero by
    // anything = zero visible change. To create a visible "lean shift",
    // we ADD a yaw offset to specific bones.
    //
    // Convention from data : right lean produces head[1] = +15 to +17 deg,
    // so positive yaw = head/torso rotated toward right side of body.
    // Therefore : NEGATIVE offset shifts the bone LEFT in body frame.
    //
    // Offset is multiplied by lean_magnitude / 1.7 so it scales smoothly
    // (no effect at zero lean, full effect at peak lean ~1.7).
    //
    // Per-bone offsets for LEFT lean only :
    float left_neck_yaw_offset;
    float left_head_yaw_offset;
    float left_back_up_yaw_offset;
    float left_back_mid_yaw_offset;
    float left_back_low_yaw_offset;
    float left_pelvis_yaw_offset;

    // Per-bone offsets for RIGHT lean only (positive = shift further right) :
    float right_neck_yaw_offset;
    float right_head_yaw_offset;
    float right_back_up_yaw_offset;
    float right_back_mid_yaw_offset;
    float right_back_low_yaw_offset;
    float right_pelvis_yaw_offset;

    // AIMWALK FIX (ported from CoD2x animation.cpp:332-348). When the
    // player leans LEFT while walking forward/backward, cod2x adds a
    // forward+left tilt to the ROOT bone (tag_origin_angles) so the body
    // model visibly shifts in the lean direction. This is the "aimwalk"
    // behavior competitive players expect.
    //
    // Defaults from cod2x : stand 7.2 deg, crouch 3.8 deg.
    // Axis 0 (pitch) = forward tilt, axis 2 (roll) = lateral tilt (negative = left).
    bool  aimwalk_fix_enable;
    float aimwalk_stand_pitch;     // root pitch offset (negative = backward)
    float aimwalk_stand_roll;      // root roll  offset (signed)
    float aimwalk_crouch_pitch;
    float aimwalk_crouch_roll;
    float aimwalk_stand_lift;      // tag_origin_offset[2] (positive = raise body)
    float aimwalk_crouch_lift;
    float aimwalk_stand_yaw_scale; // tag_origin_angles[1] multiplier (1=vanilla, 0=no twist)
    float aimwalk_crouch_yaw_scale;

    // Smoothing rate for aimwalk scale transitions (exponential decay,
    // per-second). Higher = snappier transition, lower = slower ramp.
    // The engine snaps the back yaw / tag_origin yaw from 0 to ~50 deg
    // in a single frame when the walk anim kicks in. Without smoothing
    // our aimwalk goes 0% -> 100% in 1 frame too, which looks jerky.
    //   8.0  : ~300ms to reach 90% (default, smooth but not slow)
    //   5.0  : ~500ms (slower, more cinematic)
    //   15.0 : ~150ms (snappy)
    //   0.0  : disable smoothing (snap, original behavior)
    float aimwalk_smooth_rate;

    // HEADCLIP FIX (ported from CoD2x). When a player crouches + leans
    // LEFT, they can peek around a corner with their camera while the
    // body model lags behind cover (the "clipping" competitive exploit).
    // CoD2x's solution : add a strong forward+yaw tilt to back bones so
    // the visible body model dips into view, exposing the player to fire
    // that should hit them. Defaults : on, with CoD2x's original multipliers.
    // Note : we apply this for any movement state (cod2x checks isMovingFW
    // but we don't have movement detection yet - good enough for now).
    bool  headclip_fix_enable;
    float headclip_back_low_pitch;  // 40 in cod2x (crouch values)
    float headclip_back_low_yaw;    // 30
    float headclip_back_mid_pitch;  // -20
    float headclip_back_up_pitch;   // -20

    // HEADCLIP STAND extension. Cod2x ne fait que crouch mais le bug
    // existe AUSSI en stand (le joueur lean derriere un mur, caméra
    // voit l'ennemi, modele 3D ne montre pas la tete). On etend.
    // Default a la moitie des values crouch parce que la geometrie du
    // body est differente (camera plus haute, lean rotation pivot autre).
    bool  headclip_apply_in_stand;
    float headclip_stand_back_low_pitch;
    float headclip_stand_back_low_yaw;
    float headclip_stand_back_mid_pitch;
    float headclip_stand_back_up_pitch;

    // BODY SIDEWAYS SHIFT (port cod2x animation.cpp:184-200).
    // Decale physiquement le model 3D dans la direction du lean (modifie
    // tag_origin_offset[1]). Sans ca, la camera leane mais le model reste
    // en place -> head invisible pour l'enemy derriere mur.
    //
    // Cod2x values (max shift en units quand fLeanFrac = +-1) :
    //   stand+left : 5.0  (camera leane gauche, model shift gauche aussi)
    //   stand+right: 2.5
    //   crouch+left: 12.5 (crouch shift plus = head plus exposee)
    //   crouch+right: 2.5
    bool  body_shift_enable;
    float body_shift_left_stand;
    float body_shift_left_crouch;
    float body_shift_right_stand;
    float body_shift_right_crouch;

    // CROUCH-only : ajout d'un offset de PITCH (axe 0) a chaque bone du dos.
    // Permet de redresser le buste (bones du dos -> negatif = retro-flexion),
    // ce qui visuellement remonte la tete pendant le crouch.
    // Valeur 0.0 = no-op.
    float crouch_back_low_pitch_offset;
    float crouch_back_mid_pitch_offset;
    float crouch_back_up_pitch_offset;
    float crouch_neck_pitch_offset;
    float crouch_head_pitch_offset;

    // CROUCH-only : offset Z (vertical) ajoute a tag_origin_offset (+0x50
    // dans le buffer). Eleve le modele entier de N unites quand le joueur
    // est accroupi. Valeur 0.0 = no-op. Positif = vers le haut.
    float crouch_origin_z_offset;

    // v14 lean roll offsets - REAL lean visibility fix using clean leanf
    // from cg+0x20af54. ROLL = lateral spine curve (body bends sideways
    // like a real lean). Scaled by abs(leanf)*2 (leanf maxes at ±0.5).
    //
    // Recommended values:
    //   back_low_roll  = 5-10° (hip slight tilt)
    //   back_mid_roll  = 10-15° (mid spine bend)
    //   back_up_roll   = 15-20° (upper back deeper bend = the visible lean)
    //   neck_counter   = 10-15° (head counter-tilts to stay vertical-ish)
    //   head_counter   = 15-20° (face stays mostly level)
    float lean_back_low_roll_amount;
    float lean_back_mid_roll_amount;
    float lean_back_up_roll_amount;
    float lean_neck_counter_roll;
    float lean_head_counter_roll;

    // v15 AIMWALK (forward bend when moving + leaning, like CoD2X pose).
    // Detection : origin delta frame-to-frame (no need to find velocity).
    //   - origin.x at cg+0x20af28
    //   - origin.y at cg+0x20af2c
    // Si abs(leanf) > 0.1 AND speed_per_frame > threshold -> aimwalk active.
    // On ajoute pitch forward sur back bones = body se penche en avant
    // pendant qu'on lean + walk (l'iconic CoD2X "lean into the run" pose).
    //
    // Disable via `lean_aimwalk_v15_enable = false` dans .ini = revert easy.
    bool  lean_aimwalk_v15_enable;
    float lean_aimwalk_v15_speed_threshold;   // dx²+dy² per frame, ex 0.1
    float lean_aimwalk_v15_pitch_back_low;    // forward pitch on hip
    float lean_aimwalk_v15_pitch_back_mid;
    float lean_aimwalk_v15_pitch_back_up;
};

extern LeanFixConfig g_lean_fix_config;

// Installe le hook dans cgame_mp_x86.dll. Idempotent.
bool install_lean_fix(HMODULE cgame_module);

// Applique tous les patches cibles du cgame.
bool apply_to_cgame(HMODULE cgame_module);

}  // namespace patches

#endif
