// Native engine 2D drawing for cod1reloaded.
//
// Bridges to cgame_mp_x86.dll's existing renderer primitives :
//   R_RegisterShader    : load texture by virtual path
//   RE_DrawStretchPic   : draw a 2D textured quad with full UV control
//   CG_Draw2D           : the per-frame HUD entry point (we hook its end)
//
// By piggy-backing on these we get NATIVE rendering inside the OpenGL
// surface : correct z-order, correct scaling, correct alpha, no separate
// Win32 overlay window. The same primitives used for the vanilla HUD draw
// our avatars / readyup UI / future custom 2D elements.
//
// All three function addresses come from a RE pass on cgame_mp_x86.dll
// (see docs/cod1_symbols.md or the RVA constants in engine_2d.h).

#include "engine_2d.h"
#include "logger.h"

#include <cstdint>

namespace patches {

namespace {

// Resolved cgame primitives.
typedef void (*RE_DrawStretchPic_t)(float x, float y, float w, float h,
                                     float s1, float t1, float s2, float t2,
                                     qhandle_t hShader);

// trap_R_SetColor(const float rgba[4]) : cdecl, 1 arg. NULL = blanc opaque.
typedef void (__cdecl *RE_SetColor_t)(const float* rgba);
RE_SetColor_t g_RE_SetColor = nullptr;

// Pointer-to-pointer : the engine writes the actual syscall function here
// during cgame DLL init (via the dllEntry export). We dereference at call
// time so we always get the engine's current syscall handler.
EngineSyscall_t*          g_syscall_slot      = nullptr;
RE_DrawStretchPic_t       g_RE_DrawStretchPic = nullptr;
post_hud_draw_callback_t  g_post_hud_callback = nullptr;
bool                      g_hook_installed    = false;

// Syscall IDs for shader registration. Both are seen in cgame disasm
// registering shader names. We try both and use whichever returns >0.
constexpr intptr_t SYSCALL_R_REGISTERSHADER       = 0x58;  // used directly for ui_assets_*
constexpr intptr_t SYSCALL_R_REGISTERSHADER_ALT   = 0x30;  // used at the end of fcn.300322a0

}  // namespace

// State for the trampolines (C linkage so the naked asm can reference it
// by stable symbol name).
extern "C" {
    void* g_cg_draw2d_original     = nullptr;
    void* g_cg_regshaders_original = nullptr;
}

post_register_shaders_callback_t g_post_register_callback = nullptr;
bool                             g_regshaders_hook_installed = false;

// Called by the naked trampoline AFTER the vanilla CG_Draw2D returns.
// Any state set up by the engine for HUD rendering is still active.
extern "C" void cg_draw2d_post_run() {
    if (g_post_hud_callback) g_post_hud_callback();
}

// Naked trampoline.
//
// The hooked function is CG_DrawActive(serverTime, stereoView, demoPlayback)
// - cdecl with 3 int args on the stack (Cutter reports arg_4h/8h/10h).
// The "CG_DrawActive: Undefined StereoView" assertion in vanilla code
// triggers if stereoView (= arg2) isn't STEREO_CENTER/LEFT/RIGHT, so we
// MUST forward the caller's args verbatim instead of calling void(void).
//
// Stack layout on entry to our trampoline (= just after the caller's CALL) :
//   [esp+0x00]  return address to caller (fcn.30035180+0x357b4)
//   [esp+0x04]  arg1 (serverTime)
//   [esp+0x08]  arg2 (stereoView)
//   [esp+0x0c]  arg3 (demoPlayback)
//
// We push them in reverse order so the inner CALL sees the same layout
// (rel +0x04 to +0x0c). Each push moves ESP down by 4 so we keep reading
// [esp+0xc] which now points at the next arg up.
extern "C" __attribute__((naked))
void cg_draw2d_hook_trampoline() {
    asm(
        // Forward 3 args to original.
        "pushl 0xc(%esp)\n\t"          // arg3 (demoPlayback)
        "pushl 0xc(%esp)\n\t"          // arg2 (stereoView) - was at esp+8, now esp+0xc
        "pushl 0xc(%esp)\n\t"          // arg1 (serverTime) - was at esp+4, now esp+0xc
        "call *_g_cg_draw2d_original\n\t"
        "addl $12, %esp\n\t"            // clean up the 3 args we pushed

        // Vanilla HUD just finished drawing - now our overlay on top.
        "call _cg_draw2d_post_run\n\t"

        "ret\n\t"
    );
}

qhandle_t engine_2d_register_shader(const char* name, int flags) {
    if (!name) return 0;
    // Preferred path : call the engine's own R_RegisterShader wrapper
    // (fcn.300322a0) - but ONLY safe when invoked from within the
    // CG_RegisterCgameShaders flow (= our post_register callback).
    HMODULE cgame = GetModuleHandleA("cgame_mp_x86.dll");
    if (cgame) {
        typedef qhandle_t (*RegShaderWrap_t)(const char*, int);
        RegShaderWrap_t wrap = (RegShaderWrap_t)
            ((uintptr_t)cgame + CGAME_R_REGSHADER_WRAP_RVA);
        return wrap(name, flags);
    }
    return 0;
}

// Test variant : explicitly try a given syscall ID. Used by the diagnostic.
qhandle_t engine_2d_register_shader_via(const char* name, int flags, intptr_t syscall_id) {
    if (!g_syscall_slot || !name) return 0;
    EngineSyscall_t syscall = *g_syscall_slot;
    if (!syscall) return 0;
    return (qhandle_t)syscall(syscall_id, name, flags);
}

void engine_2d_draw_stretch_pic(float x, float y, float w, float h,
                                float s1, float t1, float s2, float t2,
                                qhandle_t hShader) {
    if (!g_RE_DrawStretchPic || hShader <= 0) return;
    g_RE_DrawStretchPic(x, y, w, h, s1, t1, s2, t2, hShader);
}

void engine_2d_draw_pic(float x, float y, float w, float h, qhandle_t hShader) {
    engine_2d_draw_stretch_pic(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, hShader);
}

void engine_2d_set_color(float r, float g, float b, float a) {
    if (!g_RE_SetColor) return;
    const float c[4] = { r, g, b, a };
    g_RE_SetColor(c);
}

void engine_2d_reset_color() {
    if (g_RE_SetColor) g_RE_SetColor(nullptr);
}

void engine_2d_set_post_hud_callback(post_hud_draw_callback_t cb) {
    g_post_hud_callback = cb;
}

void engine_2d_set_post_register_callback(post_register_shaders_callback_t cb) {
    g_post_register_callback = cb;
}

// C handler for the CG_RegisterCgameShaders trampoline.
extern "C" void cg_regshaders_post_run() {
    if (g_post_register_callback) g_post_register_callback();
}

// Trampoline for CG_RegisterCgameShaders. The function is cdecl void(void)
// (no args, no return value). We call original then invoke the callback.
extern "C" __attribute__((naked))
void cg_regshaders_hook_trampoline() {
    asm(
        "call *_g_cg_regshaders_original\n\t"
        "call _cg_regshaders_post_run\n\t"
        "ret\n\t"
    );
}

bool engine_2d_install_register_hook(HMODULE cgame_module) {
    if (g_regshaders_hook_installed) return true;
    if (!cgame_module) return false;

    const uintptr_t base       = (uintptr_t)cgame_module;
    const uintptr_t target     = base + CGAME_CG_REGSHADERS_RVA;
    const uintptr_t call_site  = base + CGAME_CG_REGSHADERS_CALLSITE_RVA;

    if (*(uint8_t*)call_site != 0xE8) {
        logger::logf("engine_2d: regshaders call site 0x%x bad opcode (0x%02x)",
                     (unsigned)call_site, *(uint8_t*)call_site);
        return false;
    }
    const int32_t old_off = *(const int32_t*)(call_site + 1);
    const uintptr_t old_target = call_site + 5 + (intptr_t)old_off;
    if (old_target != target) {
        logger::logf("engine_2d: regshaders call site target mismatch (got 0x%x, "
                     "want 0x%x)", (unsigned)old_target, (unsigned)target);
        return false;
    }

    g_cg_regshaders_original = (void*)target;
    const uintptr_t wrapper = (uintptr_t)&cg_regshaders_hook_trampoline;
    const int32_t new_off = (int32_t)(wrapper - (call_site + 5));

    DWORD old_protect = 0;
    if (!VirtualProtect((void*)(call_site + 1), 4, PAGE_READWRITE, &old_protect))
        return false;
    *(int32_t*)(call_site + 1) = new_off;
    VirtualProtect((void*)(call_site + 1), 4, old_protect, &old_protect);

    g_regshaders_hook_installed = true;
    logger::logf("engine_2d: CG_RegisterCgameShaders hook installed (call site "
                 "cgame+0x%x -> 0x%p)",
                 (unsigned)CGAME_CG_REGSHADERS_CALLSITE_RVA, (void*)wrapper);
    return true;
}

// Manually trigger CG_RegisterCgameShaders via our trampoline. This is the
// way to get our post-register callback to fire when CG_Init has already
// run before we installed the hook (typical on first /devmap).
void engine_2d_force_register_shaders() {
    if (!g_cg_regshaders_original) {
        logger::logf("engine_2d: force_register: trampoline not set up");
        return;
    }
    logger::logf("engine_2d: force-triggering CG_RegisterCgameShaders");
    // Calling the original directly bypasses our trampoline (no callback).
    // We want the trampoline -> so we call THROUGH the patched call site.
    // Easiest: invoke our trampoline directly.
    typedef void (*VoidFn_t)(void);
    VoidFn_t fn = (VoidFn_t)&cg_regshaders_hook_trampoline;
    fn();
    logger::logf("engine_2d: force_register: returned");
}

bool engine_2d_install_hook(HMODULE cgame_module) {
    if (g_hook_installed) return true;
    if (!cgame_module) return false;

    const uintptr_t base = (uintptr_t)cgame_module;
    g_syscall_slot      = (EngineSyscall_t*)(base + CGAME_SYSCALL_PTR_RVA);
    g_RE_DrawStretchPic = (RE_DrawStretchPic_t)(base + CGAME_RE_DRAWSTRETCHPIC_RVA);
    g_RE_SetColor       = (RE_SetColor_t)(base + CGAME_RE_SETCOLOR_RVA);

    const uintptr_t cg_draw2d = base + CGAME_CG_DRAW2D_RVA;
    const uintptr_t call_site = base + CGAME_CG_DRAW2D_CALLSITE_RVA;

    // Sanity 1: opcode E8 (call rel32)
    const uint8_t op = *(const uint8_t*)call_site;
    if (op != 0xE8) {
        logger::logf("engine_2d: call site 0x%x bad opcode 0x%02x (expected E8)",
                     (unsigned)call_site, op);
        return false;
    }
    // Sanity 2: existing rel32 targets fcn.30019b10
    const int32_t old_off = *(const int32_t*)(call_site + 1);
    const uintptr_t old_target = call_site + 5 + (intptr_t)old_off;
    if (old_target != cg_draw2d) {
        logger::logf("engine_2d: call site targets 0x%x but expected 0x%x",
                     (unsigned)old_target, (unsigned)cg_draw2d);
        return false;
    }

    g_cg_draw2d_original = (void*)cg_draw2d;
    const uintptr_t wrapper = (uintptr_t)&cg_draw2d_hook_trampoline;
    const int32_t new_off = (int32_t)(wrapper - (call_site + 5));

    DWORD old_protect = 0;
    if (!VirtualProtect((void*)(call_site + 1), 4, PAGE_READWRITE, &old_protect)) {
        return false;
    }
    *(int32_t*)(call_site + 1) = new_off;
    VirtualProtect((void*)(call_site + 1), 4, old_protect, &old_protect);

    g_hook_installed = true;
    logger::logf("engine_2d: hook installed (CG_Draw2D call site at cgame+0x%x "
                 "-> trampoline 0x%p, syscall slot=0x%p (value=0x%p), "
                 "RE_DrawStretchPic=0x%p)",
                 (unsigned)CGAME_CG_DRAW2D_CALLSITE_RVA,
                 (void*)wrapper,
                 (void*)g_syscall_slot,
                 (void*)(g_syscall_slot ? *g_syscall_slot : nullptr),
                 (void*)g_RE_DrawStretchPic);
    return true;
}

}  // namespace patches
