#ifndef COD1RELOADED_VERSION_PATCH_H
#define COD1RELOADED_VERSION_PATCH_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// CoDMP.exe RVAs (base d'apres preferred image base 0x00400000).
// Le `push 0x005a60d0` a `0x00439f14` charge le pointeur vers la default
// value "1.5" du dvar `shortversion`. On patche les 4 bytes apres
// l'opcode 0x68 pour rediriger vers notre propre string.
constexpr uintptr_t CODMP_PREFERRED_BASE = 0x00400000;
constexpr uintptr_t CODMP_SHORTVERSION_PUSH_OPCODE_RVA  = 0x00039f14; // l'opcode 0x68
constexpr uintptr_t CODMP_SHORTVERSION_PUSH_OPERAND_RVA = 0x00039f15; // les 4 bytes a patcher
constexpr uintptr_t CODMP_SHORTVERSION_ORIGINAL_PTR     = 0x005a60d0; // pointeur vers "1.5\0"

// Limite arbitraire pour eviter de stocker une string absurde.
constexpr size_t SHORT_VERSION_MAX_LEN = 63;

// Buffer contenant la nouvelle valeur de shortversion. Modifiable a chaud
// via la config INI au demarrage. L'engine n'a JAMAIS le droit d'ecrire
// dedans : on est read-only de son point de vue (CVAR_ROM via flags 0x44).
extern char g_short_version_buffer[SHORT_VERSION_MAX_LEN + 1];

// Patche le pointeur dans l'exe. Idempotent et avec sanity check.
// A appeler une seule fois apres que CoDMP.exe est mappe en memoire
// (DllMain est OK pour ca - l'exe est forcement deja la).
bool apply_short_version_patch();

}  // namespace patches

#endif
