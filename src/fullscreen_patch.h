#ifndef COD1RELOADED_FULLSCREEN_PATCH_H
#define COD1RELOADED_FULLSCREEN_PATCH_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// CoDMP.exe RVAs (base d'apres preferred image base 0x00400000).
//
// Sequence ciblee a 0x0046c254 :
//   push 0x21                ; flags
//   push data.005a7c54       ; default value -> "1\0\0\0" puis padding
//   push str.r_fullscreen
//   call fcn.0043b880        ; Cvar_Get
//
// On patche les 4 bytes apres l'opcode 0x68 a 0x0046c256 pour rediriger
// vers notre propre string "0", ce qui change le default a windowed.
constexpr uintptr_t CODMP_RFULLSCREEN_PUSH_OPCODE_RVA  = 0x0006c256; // l'opcode 0x68
constexpr uintptr_t CODMP_RFULLSCREEN_PUSH_OPERAND_RVA = 0x0006c257; // les 4 bytes a patcher
constexpr uintptr_t CODMP_RFULLSCREEN_ORIGINAL_PTR     = 0x005a7c54; // pointeur vers "1\0"

struct FullscreenConfig {
    // Si true, on force le default a "0" (windowed). Combine avec le
    // window_patch borderless ca donne un "fake fullscreen" qui permet
    // l'alt-tab instantane.
    // Note : si l'utilisateur a `seta r_fullscreen "1"` dans son
    // config_mp.cfg, sa valeur prime sur notre default (CVAR_ARCHIVE).
    // Faut alors set r_fullscreen 0 une fois en console pour que la
    // sauvegarde soit ecrasee.
    bool force_windowed_default;
};

extern FullscreenConfig g_fullscreen_config;

// Patch idempotent applique au DllMain (CoDMP deja mappe).
bool apply_fullscreen_patch();

}  // namespace patches

#endif
