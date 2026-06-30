#ifndef COD1RELOADED_ENGINE_2D_H
#define COD1RELOADED_ENGINE_2D_H

#include <windows.h>

namespace patches {

typedef int qhandle_t;

// after vanilla CG_Draw2D, so draws land on top
typedef void (*post_hud_draw_callback_t)(void);

// RVAs in cgame_mp_x86.dll. fcn.300322a0 = R_RegisterShader wrapper, NOT safe
// outside CG_RegisterCgameShaders. regshader syscall ids: 0x30, 0x58.
constexpr uintptr_t CGAME_CG_DRAW2D_RVA            = 0x00019b10;  // CG_DrawActive
constexpr uintptr_t CGAME_CG_DRAW2D_CALLSITE_RVA   = 0x000357af;
constexpr uintptr_t CGAME_SYSCALL_PTR_RVA          = 0x00076898;
constexpr uintptr_t CGAME_RE_DRAWSTRETCHPIC_RVA    = 0x00032820;
constexpr uintptr_t CGAME_RE_SETCOLOR_RVA          = 0x00032800;  // trap_R_SetColor, syscall 0x48; NULL = opaque white
constexpr uintptr_t CGAME_CG_REGSHADERS_RVA          = 0x00022080;
constexpr uintptr_t CGAME_CG_REGSHADERS_CALLSITE_RVA = 0x00024693;
constexpr uintptr_t CGAME_R_REGSHADER_WRAP_RVA       = 0x000322a0;

// engine sets via dllEntry() at cgame load
typedef intptr_t (*EngineSyscall_t)(intptr_t syscall_id, ...);

// idempotent; false if call site signature mismatches (different cgame build)
bool engine_2d_install_hook(HMODULE cgame_module);

// name = virtual path under main/. flags: 5 = MIP_2D (HUD). call after engine
// init only (post-hud callback), never from DllMain.
qhandle_t engine_2d_register_shader(const char* name, int flags);

qhandle_t engine_2d_register_shader_via(const char* name, int flags, intptr_t syscall_id);

// coords in virtual 640x480 space
void engine_2d_draw_pic(float x, float y, float w, float h, qhandle_t hShader);

void engine_2d_draw_stretch_pic(float x, float y, float w, float h,
                                float s1, float t1, float s2, float t2,
                                qhandle_t hShader);

void engine_2d_set_color(float r, float g, float b, float a);
void engine_2d_reset_color();  // RE_SetColor(NULL); call after draws so HUD isn't tinted

void engine_2d_set_post_hud_callback(post_hud_draw_callback_t cb);

// GetTickCount() of last hook run, 0 if never. CG_Draw2D runs only in-game, so
// distinguishes game vs menu.
DWORD engine_2d_last_hud_tick();

typedef void (*post_register_shaders_callback_t)(void);
bool engine_2d_install_register_hook(HMODULE cgame_module);
void engine_2d_set_post_register_callback(post_register_shaders_callback_t cb);

void engine_2d_force_register_shaders();

}  // namespace patches

#endif
