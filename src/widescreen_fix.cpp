// Widescreen / resolution helper for CoD1.
//
// What CoD1 does wrong on widescreen:
//   - The menu only exposes 4:3 preset resolutions (max 1600x1200).
//     Modern monitors (1440p, 4K, 21:9) require manual /r_customwidth tricks.
//   - At any non-4:3 aspect ratio, the engine renders "Vert-" : it keeps
//     the vertical FOV constant and just STRETCHES the horizontal axis.
//     Result on 16:9: weapon models look squashed, you see no more of the
//     world than a 4:3 player. On 21:9 it's egregious.
//
// What this module does:
//   1) Hor+ FOV computation : converts the user's cg_fov (treated as 4:3
//      reference) into the horizontal FOV that PRESERVES vertical FOV and
//      EXPANDS horizontally based on actual aspect ratio. Math is here;
//      the actual engine hook needs the address of CG_CalcFov in
//      cgame_mp_x86.dll (see install_horplus_hook() below).
//
//   2) Custom resolution support : writes a tiny cod1reloaded_autoexec.cfg
//      snippet in main/ that the user execs from config_mp.cfg with
//      `exec cod1reloaded_autoexec.cfg`. This bypasses the menu's preset
//      list by setting r_mode -1 + r_customwidth + r_customheight.
//
//   3) Refresh rate override : same mechanism, sets r_displayrefresh.

#include "widescreen_fix.h"
#include "window_patch.h"
#include "logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>

namespace patches {

// Adresse de CG_GetEffectiveFov d'origine, posee par le hook horplus.
// Definie au scope namespace (linkage EXTERNE) : sa seule lecture est dans
// l'asm du trampoline (invisible au compilo), donc en linkage interne -O2
// l'eliminerait comme un store mort -> undefined reference au link.
extern "C" { void* g_calcfov_original = nullptr; }

// Default configuration.
WidescreenConfig g_widescreen_config = {
    /* horplus_fov_enable    */ true,
    /* aspect_mode           */ AspectMode::Auto,
    /* custom_ratio          */ 1.778f,
    // caller2 = CG_DrawSkyBoxPortal (confirmed by RE - parses skybox
    // configstring then calls CG_GetEffectiveFov to compute fov_y for
    // the sky). Must be hooked too, otherwise the skybox renders at
    // vanilla FOV while the main scene uses Hor+ -> horizon mismatch.
    /* horplus_hook_caller2  */ true,
    /* force_resolution      */ false,
    /* width                 */ 1920,
    /* height                */ 1080,
    /* refresh_hz            */ 0,
};

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Compute monitor aspect ratio via Windows desktop metrics.
float detect_monitor_aspect() {
    const int cx = GetSystemMetrics(SM_CXSCREEN);
    const int cy = GetSystemMetrics(SM_CYSCREEN);
    if (cy <= 0) return 1.778f;  // safe default
    return (float)cx / (float)cy;
}

// Compute the actual GAME VIEWPORT aspect ratio via the game window's
// client rect. Falls back to monitor metrics if the window isn't ready.
//
// CRITICAL: this is what determines the Hor+ correction. Using the monitor
// aspect when the game renders at a different aspect (e.g. monitor=16:9 but
// game viewport=4:3 because of r_mode preset) causes visible distortion -
// the engine applies a 16:9 hfov to a 4:3 frame, weapon/world look compressed.
float detect_viewport_aspect() {
    HWND hwnd = get_game_window();
    if (hwnd) {
        RECT rc;
        if (GetClientRect(hwnd, &rc)) {
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            if (h > 0 && w > 0) return (float)w / (float)h;
        }
    }
    return detect_monitor_aspect();
}

float aspect_for_mode(AspectMode m, float custom) {
    switch (m) {
        // Auto = actual game viewport (handles 4:3 stretched, vid_restart, etc.)
        case AspectMode::Auto:    return detect_viewport_aspect();
        case AspectMode::R_4_3:   return 4.0f / 3.0f;
        case AspectMode::R_16_9:  return 16.0f / 9.0f;
        case AspectMode::R_16_10: return 16.0f / 10.0f;
        case AspectMode::R_21_9:  return 64.0f / 27.0f;  // common 21:9 = 2.370
        case AspectMode::Custom:  return custom > 0.1f ? custom : 1.778f;
    }
    return 1.778f;
}

const char* aspect_mode_name(AspectMode m) {
    switch (m) {
        case AspectMode::Auto:    return "auto";
        case AspectMode::R_4_3:   return "4:3";
        case AspectMode::R_16_9:  return "16:9";
        case AspectMode::R_16_10: return "16:10";
        case AspectMode::R_21_9:  return "21:9";
        case AspectMode::Custom:  return "custom";
    }
    return "?";
}

// Locate <CoD1>/main/ - we're loaded INTO CoDMP.exe, so the EXE dir is
// the install root.
bool get_main_dir(char* out, size_t out_size) {
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;

    char* slash = strrchr(exe_path, '\\');
    if (!slash) return false;
    *(slash + 1) = '\0';

    int written = snprintf(out, out_size, "%smain\\", exe_path);
    return written > 0 && (size_t)written < out_size;
}

// Write the autoexec.cfg inside main/. This is ALWAYS regenerated at
// startup to reflect the current cod1reloaded.ini state - that way the
// user can safely toggle widescreen_force_resolution and the .cfg's
// content tracks. When everything is disabled, we write a comment-only
// "no-op" file so the `exec cod1reloaded_autoexec.cfg` line in
// config_mp.cfg becomes neutral (no resolution change at all).
void write_autoexec_cfg() {
    char main_dir[MAX_PATH];
    if (!get_main_dir(main_dir, sizeof(main_dir))) {
        logger::logf("widescreen: cannot locate main/ folder");
        return;
    }

    char cfg_path[MAX_PATH];
    snprintf(cfg_path, sizeof(cfg_path), "%scod1reloaded_autoexec.cfg", main_dir);

    FILE* f = fopen(cfg_path, "w");
    if (!f) {
        logger::logf("widescreen: cannot open %s for write (err=%lu)",
                     cfg_path, GetLastError());
        return;
    }

    fprintf(f, "// auto-generated by cod1reloaded.dll - DO NOT EDIT\n");
    fprintf(f, "// to take effect, add this line to your config_mp.cfg:\n");
    fprintf(f, "//   exec cod1reloaded_autoexec.cfg\n");
    fprintf(f, "// this file is REWRITTEN on every game launch from\n");
    fprintf(f, "// cod1reloaded.ini - don't edit it, edit the .ini instead.\n\n");

    const bool want_res     = g_widescreen_config.force_resolution &&
                              g_widescreen_config.width  > 0 &&
                              g_widescreen_config.height > 0;
    const bool want_refresh = g_widescreen_config.refresh_hz > 0;

    if (!want_res && !want_refresh) {
        fprintf(f, "// (no-op: widescreen_force_resolution=false in cod1reloaded.ini)\n");
        fprintf(f, "// remove this file to clean up, or re-enable in the .ini.\n");
        fclose(f);
        logger::logf("widescreen: wrote no-op autoexec to %s "
                     "(force_resolution=false, refresh=0)", cfg_path);
        return;
    }

    if (want_res) {
        fprintf(f, "seta r_mode \"-1\"\n");
        fprintf(f, "seta r_customwidth \"%d\"\n",  g_widescreen_config.width);
        fprintf(f, "seta r_customheight \"%d\"\n", g_widescreen_config.height);
        fprintf(f, "seta r_aspectratio \"%g\"\n",
                widescreen_get_aspect_ratio());
    }

    if (want_refresh) {
        fprintf(f, "seta r_displayrefresh \"%d\"\n",
                g_widescreen_config.refresh_hz);
    }

    fprintf(f, "\nvid_restart\n");
    fclose(f);

    logger::logf("widescreen: wrote autoexec to %s", cfg_path);
}

// Ensure config_mp.cfg has `exec cod1reloaded_autoexec.cfg` at the bottom.
// The engine reads cfg files line-by-line at startup, treating each as a
// command - so we just need the line present. Note: the engine rewrites
// config_mp.cfg on shutdown but only saves seta dvars (not exec commands),
// so our line is lost on each shutdown. We re-add it on each launch.
// This is run BEFORE the engine reads its config, since we run in DllMain.
void ensure_exec_line_in_config_mp() {
    char main_dir[MAX_PATH];
    if (!get_main_dir(main_dir, sizeof(main_dir))) return;

    char cfg_path[MAX_PATH];
    snprintf(cfg_path, sizeof(cfg_path), "%sconfig_mp.cfg", main_dir);

    // 1) Read existing content (may not exist - rare, fresh install).
    FILE* fr = fopen(cfg_path, "rb");
    bool has_line = false;
    bool ends_with_newline = true;
    if (fr) {
        fseek(fr, 0, SEEK_END);
        long sz = ftell(fr);
        fseek(fr, 0, SEEK_SET);
        if (sz > 0 && sz < 4 * 1024 * 1024) {
            char* buf = (char*)malloc((size_t)sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, (size_t)sz, fr);
                buf[n] = '\0';
                has_line = (strstr(buf, "exec cod1reloaded_autoexec.cfg") != NULL);
                if (n > 0 && buf[n - 1] != '\n' && buf[n - 1] != '\r') {
                    ends_with_newline = false;
                }
                free(buf);
            }
        }
        fclose(fr);
    } else {
        // File doesn't exist - we'll create it with just the exec line.
        ends_with_newline = true;
    }

    if (has_line) {
        logger::logf("widescreen: config_mp.cfg already has exec line, skipping");
        return;
    }

    // 2) Append the exec line.
    FILE* fa = fopen(cfg_path, "ab");
    if (!fa) {
        logger::logf("widescreen: cannot append to %s (err=%lu) - "
                     "add `exec cod1reloaded_autoexec.cfg` manually",
                     cfg_path, GetLastError());
        return;
    }
    if (!ends_with_newline) fputc('\n', fa);
    fprintf(fa, "exec cod1reloaded_autoexec.cfg\n");
    fclose(fa);

    logger::logf("widescreen: appended `exec cod1reloaded_autoexec.cfg` to %s",
                 cfg_path);
}

// -- Hor+ FOV hook --
//
// fcn.300344c0 in cgame_mp_x86.dll is the "CG_GetEffectiveFov" helper:
// reads cg_fov->value, clamps to [min, max], blends with zoom/stun FOV,
// returns the effective horizontal FOV in ST(0). It's a leaf-ish function
// with no args, ECX preserved (push/pop internally).
//
// 2 call sites read its result:
//   cgame+0x345e1 (CG_CalcViewValues tail, reached via jmp from fcn.30034bb0)
//   cgame+0x500a5 (fcn.30034e40 - probably CG_OffsetThirdPersonView)
//
// Strategy: at each call site, rewrite the relative offset of the
// `call rel32` instruction to point at our naked trampoline. The trampoline
// calls the original (preserving the cdecl-ish convention), then converts
// the returned 4:3-reference hfov to the Hor+ corrected hfov for the actual
// aspect ratio. ECX is saved across our C callback because cdecl trashes
// it (the original happened to preserve ECX, callers may rely on it).

// Cached aspect ratio - avoids calling GetSystemMetrics() each frame in
// Auto mode. Recomputed in widescreen_fix_apply() at startup.
static float g_cached_aspect = 1.778f;

// g_calcfov_original est defini plus haut (hors de l'anonymous namespace).

// cdecl wrapper called by the trampoline. Takes the vanilla effective hfov
// on the stack, returns the Hor+ corrected hfov in ST(0) (cdecl float).
//
// We recompute the aspect ratio EACH FRAME (cheap, single GetClientRect call)
// so the correction tracks the actual viewport - resilient against vid_restart,
// player switching resolutions mid-game, window moves between monitors, etc.
// In Auto mode, an aspect <= 4:3 yields a no-op (matches vanilla CoD1).
extern "C" float __cdecl apply_horplus_fov(float fov_43) {
    if (!g_widescreen_config.horplus_fov_enable) return fov_43;
    const float aspect = aspect_for_mode(g_widescreen_config.aspect_mode,
                                         g_widescreen_config.custom_ratio);
    // Below or equal to 4:3 -> no correction needed (engine's vanilla math
    // already gives the right hfov). Avoids over-correcting when the game
    // renders in a 4:3 viewport on a 16:9 monitor.
    if (aspect <= 4.0f / 3.0f + 0.01f) return fov_43;
    return widescreen_horplus_hfov(fov_43, aspect);
}

// Naked trampoline. Call signature matches the original:
//   - no stack args
//   - ECX preserved (matches original's behavior)
//   - returns float via ST(0)
extern "C" __attribute__((naked))
void calcfov_horplus_wrapper() {
    asm(
        // Call original CG_GetEffectiveFov. ST(0) = vanilla effective hfov.
        // The original does push/pop ECX itself, so ECX is preserved here.
        "call *_g_calcfov_original\n\t"

        // Save ECX before cdecl C call (apply_horplus_fov may trash it).
        "pushl %ecx\n\t"

        // Pop ST(0) into stack slot (4 bytes) to pass as float arg.
        "subl $4, %esp\n\t"
        "fstps (%esp)\n\t"

        // call apply_horplus_fov(fov_43) -> returns float in ST(0)
        "call _apply_horplus_fov\n\t"

        // Clean up the stack arg + restore ECX.
        "addl $4, %esp\n\t"
        "popl %ecx\n\t"

        "ret\n\t"
    );
}

// Patch a single call site (5-byte E8+rel32) to redirect to our wrapper.
bool patch_call_site(uintptr_t call_site, uintptr_t expected_target,
                     uintptr_t wrapper) {
    // Sanity 1: opcode E8 (call rel32)
    const uint8_t op = *(const uint8_t*)call_site;
    if (op != 0xE8) {
        logger::logf("widescreen: call site 0x%x opcode mismatch (0x%02x, expected E8)",
                     (unsigned)call_site, op);
        return false;
    }
    // Sanity 2: existing rel32 resolves to expected target
    const int32_t old_off = *(const int32_t*)(call_site + 1);
    const uintptr_t old_target = call_site + 5 + (intptr_t)old_off;
    if (old_target != expected_target) {
        logger::logf("widescreen: call site 0x%x targets 0x%x but expected 0x%x",
                     (unsigned)call_site, (unsigned)old_target, (unsigned)expected_target);
        return false;
    }
    // Patch : new rel32 = wrapper - (call_site + 5)
    const int32_t new_off = (int32_t)(wrapper - (call_site + 5));
    DWORD old_protect = 0;
    if (!VirtualProtect((void*)(call_site + 1), 4, PAGE_READWRITE, &old_protect)) {
        return false;
    }
    *(int32_t*)(call_site + 1) = new_off;
    VirtualProtect((void*)(call_site + 1), 4, old_protect, &old_protect);
    return true;
}

bool install_horplus_hook(HMODULE cgame_module) {
    if (!g_widescreen_config.horplus_fov_enable) {
        logger::logf("widescreen: horplus hook disabled by config");
        return true;
    }
    if (!cgame_module) return false;

    const uintptr_t base    = (uintptr_t)cgame_module;
    const uintptr_t calcfov = base + CGAME_CALCFOV_RVA;
    const uintptr_t call1   = base + CGAME_CALCFOV_CALL1_RVA;
    const uintptr_t call2   = base + CGAME_CALCFOV_CALL2_RVA;
    const uintptr_t wrapper = (uintptr_t)&calcfov_horplus_wrapper;

    g_calcfov_original = (void*)calcfov;

    // call1 = render path (CG_CalcViewValues tail). ALWAYS hooked - this is
    // what makes Hor+ visible in the rendered image.
    bool ok1 = patch_call_site(call1, calcfov, wrapper);
    if (!ok1) {
        logger::logf("widescreen: horplus hook FAILED on render call site");
        return false;
    }

    // call2 = CG_DrawSkyBoxPortal (RE confirmed). Computes fov_y for the
    // skybox using the same math as the main scene. Must be hooked too
    // when Hor+ is active, otherwise the sky horizon doesn't match the
    // world geometry. ON by default; opt-out via .ini for debug.
    bool ok2 = false;
    if (g_widescreen_config.horplus_hook_caller2) {
        ok2 = patch_call_site(call2, calcfov, wrapper);
        if (!ok2) {
            logger::logf("widescreen: horplus hook FAILED on secondary call site");
        }
    }

    logger::logf("widescreen: horplus hook installed render=%d secondary=%s "
                 "(trampoline 0x%08x, current aspect %.4f)",
                 ok1, (g_widescreen_config.horplus_hook_caller2
                       ? (ok2 ? "yes" : "FAILED") : "skipped"),
                 (unsigned)wrapper, g_cached_aspect);
    return ok1;
}

}  // namespace

float widescreen_get_aspect_ratio() {
    return aspect_for_mode(g_widescreen_config.aspect_mode,
                           g_widescreen_config.custom_ratio);
}

float widescreen_horplus_hfov(float cg_fov_43, float actual_aspect) {
    // Step 1: compute vertical FOV that a 4:3 setup would have at cg_fov_43.
    //         vfov = 2 * atan( (3/4) * tan(cg_fov_43/2) )
    const float vfov_rad = 2.0f * atanf(
        (3.0f / 4.0f) * tanf(cg_fov_43 * kPi / 360.0f));

    // Step 2: keep that vfov, compute hfov for the actual aspect ratio.
    //         hfov = 2 * atan( aspect * tan(vfov/2) )
    const float hfov_rad = 2.0f * atanf(
        actual_aspect * tanf(vfov_rad / 2.0f));

    return hfov_rad * 180.0f / kPi;
}

void widescreen_fix_apply() {
    const float aspect = widescreen_get_aspect_ratio();
    g_cached_aspect = aspect;
    logger::logf("widescreen: aspect_mode=%s ratio=%.4f (monitor=%dx%d)",
                 aspect_mode_name(g_widescreen_config.aspect_mode),
                 aspect,
                 GetSystemMetrics(SM_CXSCREEN),
                 GetSystemMetrics(SM_CYSCREEN));

    if (g_widescreen_config.horplus_fov_enable) {
        const float fov80_horplus = widescreen_horplus_hfov(80.0f, aspect);
        const float fov90_horplus = widescreen_horplus_hfov(90.0f, aspect);
        logger::logf("widescreen: horplus enabled. cg_fov 80 -> effective hfov %.1f, "
                     "cg_fov 90 -> effective hfov %.1f",
                     fov80_horplus, fov90_horplus);
    }

    // Always regenerate the autoexec.cfg to mirror the current .ini state.
    // When all options are off, this writes a comment-only no-op file so
    // the `exec cod1reloaded_autoexec.cfg` line in config_mp.cfg stays
    // harmless. User can safely toggle widescreen_force_resolution in the
    // .ini without ever touching config_mp.cfg again.
    write_autoexec_cfg();

    // Auto-patch config_mp.cfg to exec our autoexec (if not already there).
    // The engine strips this line on save, so we re-add on each launch.
    ensure_exec_line_in_config_mp();

    // The actual hook needs cgame_mp_x86.dll - applied later via
    // widescreen_apply_to_cgame() from the cgame watcher.
}

bool widescreen_apply_to_cgame(HMODULE cgame_module) {
    return install_horplus_hook(cgame_module);
}

}  // namespace patches
