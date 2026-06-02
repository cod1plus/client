#ifndef COD1RELOADED_ENGINE_2D_H
#define COD1RELOADED_ENGINE_2D_H

#include <windows.h>

namespace patches {

// CoD1 cgame uses qhandle_t (int) for shader / texture handles, same as Q3.
typedef int qhandle_t;

// Per-frame callback signature. Invoked AFTER the vanilla CG_Draw2D has
// finished drawing the HUD, so anything you draw will appear on top.
typedef void (*post_hud_draw_callback_t)(void);

// RVAs identified by RE in cgame_mp_x86.dll :
//   fcn.30019b10 = CG_DrawActive (3-arg cdecl per-frame entry)
//   fcn.30032820 = RE_DrawStretchPic (engine 2D quad with shader)
//   0x30076898   = engine syscall function pointer (Q3-style trap mechanism)
//   fcn.300322a0 = CG_RegisterShader wrapper - does loading screen UI then
//                  ends with syscall(0x30, name, flag). NOT safe to call
//                  outside CG_RegisterCgameShaders (it has state machine logic).
//   Direct syscall IDs for shader registration : 0x30, 0x58 (tested by
//   diagnostic - whichever returns >0 is what we use).
constexpr uintptr_t CGAME_CG_DRAW2D_RVA            = 0x00019b10;
constexpr uintptr_t CGAME_CG_DRAW2D_CALLSITE_RVA   = 0x000357af;
constexpr uintptr_t CGAME_SYSCALL_PTR_RVA          = 0x00076898;
constexpr uintptr_t CGAME_RE_DRAWSTRETCHPIC_RVA    = 0x00032820;
// trap_R_SetColor(const float rgba[4]) : syscall 0x48. Definit la couleur/alpha
// 2D globale qui multiplie les DrawStretchPic suivants. NULL = blanc opaque.
// Confirme par desassemblage (objdump) du cgame : wrapper a cgame+0x32800.
constexpr uintptr_t CGAME_RE_SETCOLOR_RVA          = 0x00032800;
// CG_RegisterCgameShaders : called once from CG_Init to register all
// engine HUD shaders via fcn.300322a0. We hook its single call site to
// inject our own R_RegisterShader call in the right context.
constexpr uintptr_t CGAME_CG_REGSHADERS_RVA          = 0x00022080;
constexpr uintptr_t CGAME_CG_REGSHADERS_CALLSITE_RVA = 0x00024693;
// fcn.300322a0 : the engine's R_RegisterShader wrapper (cdecl, takes name+flag,
// returns handle). Safe to call ONLY when our hook trampoline is active (i.e.
// inside our extended CG_RegisterCgameShaders flow).
constexpr uintptr_t CGAME_R_REGSHADER_WRAP_RVA       = 0x000322a0;

// Q3-style engine syscall function pointer. Engine sets this via dllEntry()
// at cgame DLL load. cgame uses it to call engine functions by ID.
typedef intptr_t (*EngineSyscall_t)(intptr_t syscall_id, ...);

// Install the CG_Draw2D hook. Idempotent. Returns false if the call site
// signature doesn't match (e.g. different cgame build).
bool engine_2d_install_hook(HMODULE cgame_module);

// Register a shader by name, returning its handle. Wraps R_RegisterShader.
// Must be called AFTER the engine is fully initialized (typically from
// within a post-hud callback - never directly from DllMain).
//   name  : virtual path like "textures/cod1reloaded/avatar_test"
//           resolved by the engine against main/<name>.{tga,jpg,iwi,...}
//   flags : shader type (5 = MIP_2D / no mipmap, the usual choice for HUD)
// Returns 0 on failure, positive integer on success.
qhandle_t engine_2d_register_shader(const char* name, int flags);

// Diagnostic : try a SPECIFIC syscall ID rather than the default. Useful
// for triaging which syscall number corresponds to R_RegisterShader in
// this build of CoD1.
qhandle_t engine_2d_register_shader_via(const char* name, int flags, intptr_t syscall_id);

// Draw a 2D image filling its full UV (0,0)-(1,1) at the given screen rect.
// Coordinates are in the engine's virtual 640x480 space (auto-scaled to
// the actual viewport by the renderer).
void engine_2d_draw_pic(float x, float y, float w, float h, qhandle_t hShader);

// Draw with custom UV sub-rectangle.
void engine_2d_draw_stretch_pic(float x, float y, float w, float h,
                                float s1, float t1, float s2, float t2,
                                qhandle_t hShader);

// Definit la couleur/alpha 2D globale (composantes 0..1). Multiplie tous les
// DrawStretchPic suivants -> permet fade-in (alpha) et teinte (rgb).
void engine_2d_set_color(float r, float g, float b, float a);
// Remet la couleur a blanc opaque (RE_SetColor(NULL)). A appeler apres nos
// draws pour ne pas teinter le HUD vanilla.
void engine_2d_reset_color();

// Set the per-frame post-HUD callback. Pass nullptr to disable.
// The callback runs on the cgame's main thread, after CG_Draw2D completed
// drawing the vanilla HUD. The renderer's 2D state is set up - you can
// freely call engine_2d_draw_pic / draw_stretch_pic from here.
void engine_2d_set_post_hud_callback(post_hud_draw_callback_t cb);

// Frame heartbeat : GetTickCount() de la derniere execution du hook CG_Draw2D.
// Renvoie 0 si le hook n'a jamais tourne (pas encore en partie). CG_Draw2D ne
// s'execute QUE pendant une partie active -> permet de detecter "en partie" vs
// "menus" sans lire l'etat moteur. Lecture thread-safe (32-bit aligne).
DWORD engine_2d_last_hud_tick();

// Install the CG_RegisterCgameShaders hook. After this is called and
// CG_RegisterCgameShaders runs at least once (either naturally during
// CG_Init or triggered by engine_2d_force_register_shaders()), the
// post-register callback fires with a chance to register custom shaders.
typedef void (*post_register_shaders_callback_t)(void);
bool engine_2d_install_register_hook(HMODULE cgame_module);
void engine_2d_set_post_register_callback(post_register_shaders_callback_t cb);

// Manually trigger CG_RegisterCgameShaders. Use this if cgame's CG_Init
// has already run before our hook was installed (the typical case on
// first /devmap). Calls the original function via our trampoline, so
// the post-register callback fires.
void engine_2d_force_register_shaders();

}  // namespace patches

#endif
