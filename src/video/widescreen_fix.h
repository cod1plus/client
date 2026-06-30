#ifndef COD1RELOADED_WIDESCREEN_FIX_H
#define COD1RELOADED_WIDESCREEN_FIX_H

#include <windows.h>

namespace patches {

enum class AspectMode {
    Auto,
    R_4_3,
    R_16_9,
    R_16_10,
    R_21_9,
    Custom,
};

struct WidescreenConfig {
    bool       horplus_fov_enable;
    AspectMode aspect_mode;
    float      custom_ratio;
    bool       horplus_hook_caller2;  // also hook CG_DrawSkyBoxPortal so sky matches scene

    bool   force_resolution;  // r_mode -1 + r_customwidth/height via autoexec
    int    width;
    int    height;
    int    refresh_hz;  // 0 = engine auto-detect
};

extern WidescreenConfig g_widescreen_config;

// cgame_mp_x86.dll RVAs (RE'd). CALCFOV = CG_GetEffectiveFov, two callers.
constexpr uintptr_t CGAME_CALCFOV_RVA       = 0x000344c0;
constexpr uintptr_t CGAME_CALCFOV_CALL1_RVA = 0x000345e1; // CG_CalcViewValues tail
constexpr uintptr_t CGAME_CALCFOV_CALL2_RVA = 0x000350a5; // fcn.30034e40 CG_DrawSkyBoxPortal

float widescreen_get_aspect_ratio();
float widescreen_horplus_hfov(float cg_fov_43, float actual_aspect);

void widescreen_fix_apply();  // DllMain: autoexec.cfg + aspect detection
bool widescreen_apply_to_cgame(HMODULE cgame_module);  // install hook, idempotent

}  // namespace patches

#endif
