// Lean fix — port du fix aimwalk cod2x pour CoD1.
//
// v22 : detection du lean sans toucher a la camera.
//
// La fraction de lean du joueur local est deja disponible en memoire a
// cgame+0x20af54 (CG_LOCAL_LEANF_RVA, confirme par RE via Cutter).
// Lecture directe dans apply_lean_adjust — aucun hook camera.
//
// Deux sources → MEME roll lateral sur tag_origin_angles[2] :
//   A) Lean stationnaire (Q ou E seul) : leanf != 0
//   B) Aimwalk strafe diagonal         : engine_yaw > 5° + velocity gate
//
// Hook unique : trampoline naked au call site cgame+0x51d8 (body pipeline).

#include "lean_fix.h"
#include "logger.h"
#include "widescreen_fix.h"
#include "swing_fix.h"
#include "lean_amplify.h"

#include <cstdio>
#include <cstring>
#include <cmath>

namespace patches {

LeanFixConfig g_lean_fix_config = {
    /* enable             */ false,
    /* apply_in_stand     */ true,
    /* diag_log_count     */ 300,

    /* neck/head/back_up/back_mid/back_low/pelvis pitch */ 1, 1, 1, 1, 1, 1,
    /* neck/head/back_up/back_mid/back_low/pelvis yaw   */ 1, 1, 1, 1, 1, 1,
    /* neck/head/back_up/back_mid/back_low/pelvis roll  */ 1, 1, 1, 1, 1, 1,
    /* left_* pitch  */ -1, -1, -1, -1, -1, -1,
    /* left_* yaw    */ -1, -1, -1, -1, -1, -1,
    /* left_* roll   */ -1, -1, -1, -1, -1, -1,
    /* left_* yaw_offset  */ 0, 0, 0, 0, 0, 0,
    /* right_* yaw_offset */ 0, 0, 0, 0, 0, 0,

    /* aimwalk_fix_enable     */ true,
    /* aimwalk_stand_pitch    */ 0.0f,
    /* aimwalk_stand_roll     */ 3.0f,
    /* aimwalk_crouch_pitch   */ 0.0f,
    /* aimwalk_crouch_roll    */ 2.0f,
    /* aimwalk_stand_lift     */ 0.0f,
    /* aimwalk_crouch_lift    */ 0.0f,
    /* aimwalk_stand_yaw_scale*/ 1.0f,
    /* aimwalk_crouch_yaw_scale*/ 1.0f,
    /* aimwalk_smooth_rate    */ 6.0f,

    /* headclip_fix_enable      */ false,
    /* headclip_back_low_pitch  */ 0, /* headclip_back_low_yaw */ 0,
    /* headclip_back_mid_pitch  */ 0, /* headclip_back_up_pitch */ 0,
    /* headclip_apply_in_stand  */ false,
    /* headclip_stand_back_low_pitch */ 0, /* headclip_stand_back_low_yaw */ 0,
    /* headclip_stand_back_mid_pitch */ 0, /* headclip_stand_back_up_pitch */ 0,
    /* body_shift_enable           */ false,
    /* body_shift_left_stand       */ 0, /* body_shift_left_crouch  */ 0,
    /* body_shift_right_stand      */ 0, /* body_shift_right_crouch */ 0,
    /* crouch_back_low_pitch_offset */ 0, /* crouch_back_mid_pitch_offset */ 0,
    /* crouch_back_up_pitch_offset  */ 0, /* crouch_neck_pitch_offset     */ 0,
    /* crouch_head_pitch_offset     */ 0, /* crouch_origin_z_offset       */ 0,
    /* lean_back_low_roll_amount */ 0, /* lean_back_mid_roll_amount */ 0,
    /* lean_back_up_roll_amount  */ 0, /* lean_neck_counter_roll */ 0,
    /* lean_head_counter_roll    */ 0,
    /* lean_aimwalk_v15_enable           */ false,
    /* lean_aimwalk_v15_speed_threshold  */ 0,
    /* lean_aimwalk_v15_pitch_back_low   */ 0,
    /* lean_aimwalk_v15_pitch_back_mid   */ 0,
    /* lean_aimwalk_v15_pitch_back_up    */ 0,
};

namespace {
bool g_installed = false;
uintptr_t g_cgame_base = 0;

struct AimwalkSmooth {
    const void* ci_key;
    float       scale;
    LONGLONG    last_qpc;
};
AimwalkSmooth g_aimwalk_smooth[64] = {};

double aimwalk_seconds_since(LONGLONG last_qpc, LONGLONG now_qpc) {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    if (freq.QuadPart == 0) return 0.0;
    return (double)(now_qpc - last_qpc) / (double)freq.QuadPart;
}

AimwalkSmooth* aimwalk_get_state(const void* ci) {
    if (!ci) return nullptr;
    int free_idx = -1;
    int oldest_idx = 0;
    LONGLONG oldest_qpc = 0x7FFFFFFFFFFFFFFFLL;
    for (int i = 0; i < 64; ++i) {
        if (g_aimwalk_smooth[i].ci_key == ci) return &g_aimwalk_smooth[i];
        if (!g_aimwalk_smooth[i].ci_key && free_idx < 0) free_idx = i;
        if (g_aimwalk_smooth[i].last_qpc < oldest_qpc) {
            oldest_qpc = g_aimwalk_smooth[i].last_qpc;
            oldest_idx = i;
        }
    }
    int slot = (free_idx >= 0) ? free_idx : oldest_idx;
    g_aimwalk_smooth[slot].ci_key   = ci;
    g_aimwalk_smooth[slot].scale    = 0.0f;
    g_aimwalk_smooth[slot].last_qpc = 0;
    return &g_aimwalk_smooth[slot];
}

int g_diag_logged = 0;
}  // namespace

extern "C" void apply_lean_adjust(float* controllers,
                                  const void* client_info,
                                  const void* entity) {
    if (!g_lean_fix_config.enable) return;
    if (!controllers || !client_info || !entity) return;

    const uint32_t eflags = *(const uint32_t*)((const char*)entity + ENT_EFLAGS_OFFSET);
    const bool is_crouch = (eflags & ENT_FLAG_CROUCH) != 0;
    const bool is_prone  = (eflags & ENT_FLAG_PRONE)  != 0;
    if (is_prone) return;
    if (!is_crouch && !g_lean_fix_config.apply_in_stand) return;
    if (!g_lean_fix_config.aimwalk_fix_enable) return;

    float* tag_origin_angles = (float*)((char*)controllers + CTRL_TAG_ORIGIN_ANGLES);

    AimwalkSmooth* st = aimwalk_get_state(client_info);
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const LONGLONG now_qpc = qpc.QuadPart;
    const double dt = (st && st->last_qpc != 0)
                      ? aimwalk_seconds_since(st->last_qpc, now_qpc)
                      : 0.0;

    // === Gate 1 : lean fraction du joueur local ===
    // Lecture directe a cgame+0x20af54 (confirme par RE : c'est la valeur
    // passee comme leanFrac a BG_AddLeanToPosition). Signe [-1, +1].
    // Non-zero uniquement quand Q ou E est maintenu.
    float leanf = 0.0f;
    if (g_cgame_base != 0)
        leanf = *(const float*)(g_cgame_base + CG_LOCAL_LEANF_RVA);
    const bool lean_gate = fabsf(leanf) > 0.05f;

    // === Gate 2 : engine_yaw diagonal (aimwalk) ===
    const float engine_yaw = tag_origin_angles[1];
    const bool moving_diagonal_left  = (engine_yaw > 5.0f)  && (engine_yaw < 90.0f);
    const bool moving_diagonal_right = (engine_yaw < -5.0f) && (engine_yaw > -90.0f);
    const bool yaw_gate = moving_diagonal_left || moving_diagonal_right;

    // === Gate 3 : velocity ===
    bool velocity_gate = false;
    float speed_sq = 0.0f;
    if (g_cgame_base != 0) {
        static float s_prev_origin_x = 0.0f;
        static float s_prev_origin_y = 0.0f;
        static bool  s_origin_init = false;
        const float cur_origin_x = *(const float*)(g_cgame_base + 0x20af28);
        const float cur_origin_y = *(const float*)(g_cgame_base + 0x20af2c);
        if (!s_origin_init) {
            s_prev_origin_x = cur_origin_x;
            s_prev_origin_y = cur_origin_y;
            s_origin_init = true;
        }
        const float dx = cur_origin_x - s_prev_origin_x;
        const float dy = cur_origin_y - s_prev_origin_y;
        speed_sq = dx*dx + dy*dy;
        s_prev_origin_x = cur_origin_x;
        s_prev_origin_y = cur_origin_y;
        velocity_gate = (speed_sq > 0.05f);
    }

    const bool aimwalk_gate  = yaw_gate && velocity_gate;
    const bool gate_open     = lean_gate || aimwalk_gate;
    const float target_scale = gate_open ? 1.0f : 0.0f;

    float aimwalk_scale = target_scale;
    if (st) {
        const float rate = g_lean_fix_config.aimwalk_smooth_rate;
        if (rate <= 0.0f || dt <= 0.0) {
            st->scale = target_scale;
        } else {
            float alpha = 1.0f - expf(-rate * (float)dt);
            if (alpha > 1.0f) alpha = 1.0f;
            if (alpha < 0.0f) alpha = 0.0f;
            st->scale += (target_scale - st->scale) * alpha;
        }
        aimwalk_scale = st->scale;
        st->last_qpc = now_qpc;
    }

    // Signe : depuis leanf direct si lean key, sinon depuis direction diagonal.
    // Convention a verifier : si corps penche dans le mauvais sens,
    // swap +1.0f / -1.0f dans le bloc lean_gate ci-dessous.
    float roll_sign = 0.0f;
    if (lean_gate) {
        roll_sign = (leanf > 0.0f) ? +1.0f : -1.0f;
    } else if (yaw_gate) {
        roll_sign = moving_diagonal_left ? -1.0f : +1.0f;
    }

    const bool log_worthy =
        aimwalk_scale > 0.001f
        || fabsf(leanf)    > 0.03f
        || speed_sq        > 0.02f
        || fabsf(engine_yaw) > 3.0f;
    if (log_worthy && g_diag_logged < g_lean_fix_config.diag_log_count) {
        const char* gate_name = lean_gate    ? "LEAN"
                              : aimwalk_gate ? "AIMW"
                              : yaw_gate     ? "y"
                              : velocity_gate? "v"
                              : "0";
        logger::logf("lean[%d] cr=%d yaw=%.2f spd=%.3f leanf=%+.3f gate=%s sign=%+.0f scale=%.3f",
                     g_diag_logged, is_crouch, engine_yaw, speed_sq,
                     leanf, gate_name, roll_sign, aimwalk_scale);
        ++g_diag_logged;
    }

    if (aimwalk_scale <= 0.001f || roll_sign == 0.0f) return;

    if (is_crouch) {
        tag_origin_angles[0] += g_lean_fix_config.aimwalk_crouch_pitch * aimwalk_scale;
        tag_origin_angles[2] += g_lean_fix_config.aimwalk_crouch_roll  * aimwalk_scale * roll_sign;
    } else {
        tag_origin_angles[0] += g_lean_fix_config.aimwalk_stand_pitch * aimwalk_scale;
        tag_origin_angles[2] += g_lean_fix_config.aimwalk_stand_roll  * aimwalk_scale * roll_sign;
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
        "ret\n\t"
    );
}

bool install_lean_fix(HMODULE cgame_module) {
    if (g_installed) return true;
    if (!cgame_module) return false;

    const uintptr_t base       = (uintptr_t)cgame_module;
    const uintptr_t calculator = base + CGAME_DOCONTROLLERS_INTERNAL_RVA;
    const uintptr_t call_site  = base + CGAME_DOCONTROLLERS_CALL_SITE_RVA;

    const uint8_t opcode = *(const uint8_t*)call_site;
    if (opcode != 0xE8) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "cod1reloaded lean fix: opcode inattendu a cgame+0x%lx (0x%02x).\n"
            "Hook annule.",
            (unsigned long)CGAME_DOCONTROLLERS_CALL_SITE_RVA, opcode);
        MessageBoxA(NULL, msg, "cod1reloaded", MB_OK | MB_ICONWARNING);
        return false;
    }

    const int32_t existing_offset  = *(const int32_t*)(call_site + 1);
    const uintptr_t existing_target = call_site + 5 + (uintptr_t)(intptr_t)existing_offset;
    if (existing_target != calculator) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "cod1reloaded lean fix: call cible cgame+0x%lx mais on attendait 0x%lx.\n"
            "Hook annule.",
            (unsigned long)(existing_target - base),
            (unsigned long)CGAME_DOCONTROLLERS_INTERNAL_RVA);
        MessageBoxA(NULL, msg, "cod1reloaded", MB_OK | MB_ICONWARNING);
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
    apply_swing_fix(cgame_module);
    apply_lean_amplify(cgame_module);

    if (!g_lean_fix_config.enable) {
        logger::logf("  lean hook NOT installed (disabled)");
        return true;
    }
    return install_lean_fix(cgame_module);
}

}  // namespace patches
