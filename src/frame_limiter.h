#ifndef COD1RELOADED_FRAME_LIMITER_H
#define COD1RELOADED_FRAME_LIMITER_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// CoDMP.exe RVAs (preferred base 0x00400000).
//
// Sequence ciblee dans la boucle de frame limit a fcn.0043a3d0 :
//   0x0043a4f5: call fcn.00438a70   ; sys_milliseconds-like, en boucle
//   0x0043a4fa: ...
//   0x0043a515: cmp esi, edi
//   0x0043a517: jl 0x43a4f5         ; spin tant que elapsed < target
//
// Patch : on remplace l'OPERANDE du call (les 4 bytes apres le 0xE8) pour
// qu'il pointe vers notre frame_wait_replacement(). Cette fonction fait
// un spin-wait precis avec QueryPerformanceCounter (resolution us), puis
// retourne une valeur ms qui satisfait la condition de la boucle. Le
// moteur exit la boucle apres une seule iteration.
//
// La fcn.00438a70 originale est NON TOUCHEE - elle est appelee ailleurs.
// (CODMP_PREFERRED_BASE est defini dans version_patch.h)
constexpr uintptr_t CODMP_FRAME_LIMIT_CALL_OPCODE_RVA   = 0x0003a4f5; // 'E8'
constexpr uintptr_t CODMP_FRAME_LIMIT_CALL_OPERAND_RVA  = 0x0003a4f6; // offset 4 bytes
constexpr uintptr_t CODMP_FRAME_LIMIT_ORIGINAL_TARGET   = 0x00438a70; // fcn.00438a70

// Pointeur global du dvar com_maxfps (mov [data.01912acc], eax apres
// son Cvar_Get). On y lit le pointeur, puis on lit dvar->integer a +0x20.
constexpr uintptr_t CODMP_COM_MAXFPS_DVAR_SLOT_VA       = 0x01912acc;
constexpr uintptr_t CODMP_DVAR_INTEGER_OFFSET           = 0x20;

struct FrameLimiterConfig {
    bool enable;
    // Offset positif en us ajoute au deadline. Compense les overhead
    // de syscalls/scheduler ; 0 par defaut, peut etre baisse a -500 pour
    // viser 250.5 effectif si la mesure penche un poil sous 250.
    int  deadline_bias_us;
};

extern FrameLimiterConfig g_frame_limiter_config;

bool apply_frame_limiter_patch();

}  // namespace patches

#endif
