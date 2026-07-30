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

    bool   stretch_enable;   // "stretched" mode: render a 4:3 vfov into the widescreen
                             // buffer -> horizontal stretch (wider models, classic comp look)
};

extern WidescreenConfig g_widescreen_config;

// cgame_mp_x86.dll RVAs (RE'd). CALCFOV = CG_GetEffectiveFov, two callers.
constexpr uintptr_t CGAME_CALCFOV_RVA       = 0x000344c0;
constexpr uintptr_t CGAME_CALCFOV_CALL1_RVA = 0x000345e1; // CG_CalcViewValues tail
constexpr uintptr_t CGAME_CALCFOV_CALL2_RVA = 0x000350a5; // fcn.30034e40 CG_DrawSkyBoxPortal

// Stretch: the vfov trig in CG_CalcViewValues reads refdef.width/height via two `fild`
// instructions; their disp32 operands sit at these RVAs. Redirect them to mod-owned ints
// (g_stretch_w/h) so we can feed a forced 4:3 ratio (= horizontal stretch) live.
constexpr uintptr_t CGAME_VFOV_HEIGHT_OP_RVA = 0x000345e8; // operand of `fild [refdef.height]`
constexpr uintptr_t CGAME_VFOV_WIDTH_OP_RVA  = 0x000345ee; // operand of `fild [refdef.width]`
// Original operand values of the two fild sites above. Verified byte-exact in
// Main/cgame_mp_x86.dll: at RVA 0x345e8 the 4 operand bytes are 3c d3 20 30 =
// 0x3020d33c, i.e. imagebase 0x30000000 + RVA 0x20d33c (the other site holds ...d338).
// These were 0x34d33c/0x34d338 (0x140000 too high) which made the sanity check fail on
// every load -> "stretch operands redirected (PARTIAL)" and stretched did nothing.
//
// WHICH IS WHICH: traced through the FPU in CG_CalcFov @0x345e0:
//   fild [d33c] ; fild [d338] ; fld st(2) ; fmul k ; fptan ; fstp ; fdivp st(1) ; fpatan
// resolves to  fov_y = atan( [d33c] * tan(fov_x/2) / [d338] ), and the Q3 formula is
// fov_y = atan(height * tan(fov_x/2) / width). So **d33c is HEIGHT and d338 is WIDTH** -
// the opposite of the old naming. With the names swapped, feeding 4 and 3 for stretched
// produced a 3:4 (portrait) aspect, which is why stretched looked wrong. In the
// non-stretched path the two swaps cancelled out, so classic/widescreen still looked
// right - which is exactly why this stayed hidden.
constexpr uintptr_t CGAME_REFDEF_HEIGHT_RVA  = 0x0020d33c; // read by the fild @0x345e8
constexpr uintptr_t CGAME_REFDEF_WIDTH_RVA   = 0x0020d338; // read by the fild @0x345ee
// glConfig dims. These were swapped too: with the game running 1440x1080 the mod logged
// "seed dims 1080x1440", i.e. 0x1d5a28 holds the WIDTH and 0x1d5a2c the HEIGHT (normal
// struct order, width first). Combined with the swapped refdef names above, the error
// cancelled out in the non-stretched path - so nothing looked broken until stretched
// actually started working. Both are now named correctly; do not "re-align" them.
constexpr uintptr_t CGAME_VIDWIDTH_RVA       = 0x001d5a28; // glConfig.vidWidth  (=1440)
constexpr uintptr_t CGAME_VIDHEIGHT_RVA      = 0x001d5a2c; // glConfig.vidHeight (=1080)

float widescreen_get_aspect_ratio();
float widescreen_horplus_hfov(float cg_fov_43, float actual_aspect);

// Aspect ratio as a human string: "auto" / "4:3" / "16:9" / "16:10" / "21:9",
// or a plain number like "1.6" (= a custom ratio). Shared by the .ini loader and the
// in-game menu so both speak the same values.
void widescreen_set_aspect(const char* s);        // parse string -> aspect_mode (+ custom_ratio)
void widescreen_get_aspect(char* out, size_t n);  // aspect_mode/custom_ratio -> string

void widescreen_fix_apply();  // DllMain: autoexec.cfg + aspect detection
bool widescreen_apply_to_cgame(HMODULE cgame_module);  // install hooks (Hor+ and stretch), idempotent
void widescreen_update_stretch();  // per-tick: feed g_stretch_w/h (4:3 when stretched, else real dims)

}  // namespace patches

#endif
