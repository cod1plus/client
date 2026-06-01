#ifndef COD1RELOADED_WIDESCREEN_FIX_H
#define COD1RELOADED_WIDESCREEN_FIX_H

#include <windows.h>

namespace patches {

// Aspect ratio mode for Hor+ FOV computation.
//   Auto   : monitor detected via GetSystemMetrics()
//   R_4_3  : 1.333 -> no widescreen correction applied (vanilla CoD1)
//   R_16_9 : 1.778
//   R_16_10: 1.600
//   R_21_9 : 2.370 (ultrawide)
//   Custom : use custom_ratio
enum class AspectMode {
    Auto,
    R_4_3,
    R_16_9,
    R_16_10,
    R_21_9,
    Custom,
};

struct WidescreenConfig {
    // Hor+ FOV correction. When enabled, cg_fov keeps the value YOU set,
    // but the actual rendered horizontal FOV expands proportionally to your
    // monitor's aspect ratio - you SEE more horizontally on widescreen
    // without the vanilla "Vert-" stretching.
    bool       horplus_fov_enable;
    AspectMode aspect_mode;
    float      custom_ratio;
    // Also hook the SECOND reader of CG_GetEffectiveFov (in fcn.30034e40).
    // This second site is suspected to be the mouse-sensitivity scaling
    // path - hooking it makes the mouse feel ~20% faster on widescreen
    // because the engine sees fov 96 instead of 80. Leave at FALSE unless
    // you know your build needs it (e.g. third-person view debugging).
    bool       horplus_hook_caller2;

    // Force resolution by writing an autoexec snippet. The engine doesn't
    // expose 1440p/4K/21:9 in the menu - we set r_mode -1 + r_customwidth/
    // r_customheight via a generated .cfg the user execs from config_mp.cfg.
    bool   force_resolution;
    int    width;
    int    height;
    int    refresh_hz;  // 0 = engine auto-detect
};

extern WidescreenConfig g_widescreen_config;

// cgame_mp_x86.dll RVAs identified by RE:
//   fcn.300344c0 : "CG_GetEffectiveFov" - returns the clamped/zoom-adjusted
//                  horizontal FOV in ST(0). 2 call sites read its result.
constexpr uintptr_t CGAME_CALCFOV_RVA       = 0x000344c0;
constexpr uintptr_t CGAME_CALCFOV_CALL1_RVA = 0x000345e1; // CG_CalcViewValues tail
constexpr uintptr_t CGAME_CALCFOV_CALL2_RVA = 0x000350a5; // fcn.30034e40

// Read configured aspect ratio, accounting for Auto detection. Returns
// the actual width/height ratio (e.g. 1.778 for 16:9).
float widescreen_get_aspect_ratio();

// Compute Hor+ adjusted horizontal FOV.
//   cg_fov_43      : the FOV value the user set (treated as 4:3 reference)
//   actual_aspect  : current monitor aspect ratio
// Returns the corrected horizontal FOV in degrees.
float widescreen_horplus_hfov(float cg_fov_43, float actual_aspect);

// Apply all process-level widescreen tweaks at startup (autoexec.cfg
// generation, aspect ratio detection). Called from DllMain.
void widescreen_fix_apply();

// Install the Hor+ hook in cgame_mp_x86.dll. Called once cgame is loaded
// (lazy, from the cgame watcher thread). Idempotent.
bool widescreen_apply_to_cgame(HMODULE cgame_module);

}  // namespace patches

#endif
