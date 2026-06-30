// 2D draw via cgame_mp_x86.dll renderer primitives. RVAs in engine_2d.h.

#include "features/engine_2d.h"
#include "core/logger.h"

#include <cstdint>

namespace patches {

namespace {

typedef void (*RE_DrawStretchPic_t)(float x, float y, float w, float h,
                                     float s1, float t1, float s2, float t2,
                                     qhandle_t hShader);

// NULL rgba = opaque white
typedef void (__cdecl *RE_SetColor_t)(const float* rgba);
RE_SetColor_t g_RE_SetColor = nullptr;

EngineSyscall_t*          g_syscall_slot      = nullptr;  // engine writes syscall fn here at cgame init; deref at call time
RE_DrawStretchPic_t       g_RE_DrawStretchPic = nullptr;
post_hud_draw_callback_t  g_post_hud_callback = nullptr;
bool                      g_hook_installed    = false;

volatile DWORD            g_last_hud_tick     = 0;

constexpr intptr_t SYSCALL_R_REGISTERSHADER       = 0x58;  // ui_assets_*
constexpr intptr_t SYSCALL_R_REGISTERSHADER_ALT   = 0x30;  // end of fcn.300322a0

}  // namespace

extern "C" {
    void* g_cg_draw2d_original     = nullptr;
    void* g_cg_regshaders_original = nullptr;
}

post_register_shaders_callback_t g_post_register_callback = nullptr;
bool                             g_regshaders_hook_installed = false;

extern "C" void cg_draw2d_post_run() {
    g_last_hud_tick = GetTickCount();
    if (g_post_hud_callback) g_post_hud_callback();
}

// hooked fn = CG_DrawActive(serverTime, stereoView, demoPlayback) cdecl 3 args.
// MUST forward args verbatim or the "Undefined StereoView" assert fires.
extern "C" __attribute__((naked))
void cg_draw2d_hook_trampoline() {
    asm(
        "pushl 0xc(%esp)\n\t"          // each push drops esp 4, so re-read 0xc
        "pushl 0xc(%esp)\n\t"
        "pushl 0xc(%esp)\n\t"
        "call *_g_cg_draw2d_original\n\t"
        "addl $12, %esp\n\t"
        "call _cg_draw2d_post_run\n\t"
        "ret\n\t"
    );
}

qhandle_t engine_2d_register_shader(const char* name, int flags) {
    if (!name) return 0;
    // fcn.300322a0 wrapper. safe ONLY inside CG_RegisterCgameShaders flow
    HMODULE cgame = GetModuleHandleA("cgame_mp_x86.dll");
    if (cgame) {
        typedef qhandle_t (*RegShaderWrap_t)(const char*, int);
        RegShaderWrap_t wrap = (RegShaderWrap_t)
            ((uintptr_t)cgame + CGAME_R_REGSHADER_WRAP_RVA);
        return wrap(name, flags);
    }
    return 0;
}

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

DWORD engine_2d_last_hud_tick() {
    return g_last_hud_tick;
}

void engine_2d_set_post_register_callback(post_register_shaders_callback_t cb) {
    g_post_register_callback = cb;
}

extern "C" void cg_regshaders_post_run() {
    if (g_post_register_callback) g_post_register_callback();
}

// CG_RegisterCgameShaders cdecl void(void)
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

// fire trampoline manually when CG_Init ran before the hook (first /devmap)
void engine_2d_force_register_shaders() {
    if (!g_cg_regshaders_original) {
        logger::logf("engine_2d: force_register: trampoline not set up");
        return;
    }
    logger::logf("engine_2d: force-triggering CG_RegisterCgameShaders");
    // via trampoline, not original, so callback runs
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

    const uint8_t op = *(const uint8_t*)call_site;
    if (op != 0xE8) {  // E8 = call rel32
        logger::logf("engine_2d: call site 0x%x bad opcode 0x%02x (expected E8)",
                     (unsigned)call_site, op);
        return false;
    }
    // existing rel32 must target fcn.30019b10
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
