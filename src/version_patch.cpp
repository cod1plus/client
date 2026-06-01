// Patch du dvar `shortversion` dans CoDMP.exe.
//
// Strategie : on ne touche pas a la string "1.5" en place (il n'y a pas de
// padding apres - "win-x86" est colle juste derriere). On modifie a la place
// le pointeur que pousse l'engine au moment d'appeler Cvar_Get :
//
//   0x00439f12  push 0x44              ; flags CVAR_ROM | ...
//   0x00439f14  push 0x005a60d0        ; <-- on patche les 4 bytes apres l'opcode 0x68
//   0x00439f19  push str.shortversion
//   0x00439f23  call Cvar_Get
//
// Apres le patch, l'engine enregistre le dvar avec notre string comme
// default value -> elle s'affiche partout ou shortversion est lu (menu,
// userinfo, console, etc.).

#include "version_patch.h"
#include "logger.h"

#include <cstdio>
#include <cstring>

namespace patches {

// Aligne sur 16 bytes pour eviter d'eventuels soucis de fetch CPU. Le contenu
// est initialise au demarrage depuis la config INI ; fallback = "1.5 reloaded".
alignas(16) char g_short_version_buffer[SHORT_VERSION_MAX_LEN + 1] = "1.5 reloaded";

namespace {
bool g_applied = false;
}  // namespace

bool apply_short_version_patch() {
    if (g_applied) return true;

    // CoDMP.exe est le process principal lui-meme.
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) {
        logger::logf("version_patch: GetModuleHandleA(NULL) returned null - aborted");
        return false;
    }

    const uintptr_t exe_base = (uintptr_t)exe;
    const uintptr_t opcode_addr  = exe_base + CODMP_SHORTVERSION_PUSH_OPCODE_RVA;
    const uintptr_t operand_addr = exe_base + CODMP_SHORTVERSION_PUSH_OPERAND_RVA;

    // Sanity check #1 : l'opcode doit etre 0x68 (push imm32)
    const uint8_t opcode = *(const uint8_t*)opcode_addr;
    if (opcode != 0x68) {
        logger::logf(
            "version_patch: opcode inattendu a CoDMP+0x%lx (0x%02x, attendu 0x68). "
            "Probablement pas CoDMP.exe 1.5 - patch annule.",
            (unsigned long)CODMP_SHORTVERSION_PUSH_OPCODE_RVA, opcode);
        return false;
    }

    // Sanity check #2 : l'operande actuel doit pointer vers la default value
    // d'origine 0x005a60d0 ("1.5"). Si non -> autre version du binaire.
    const uint32_t current_ptr = *(const uint32_t*)operand_addr;
    if (current_ptr != CODMP_SHORTVERSION_ORIGINAL_PTR) {
        logger::logf(
            "version_patch: pointeur actuel 0x%08x != attendu 0x%08x - patch annule.",
            current_ptr, (unsigned)CODMP_SHORTVERSION_ORIGINAL_PTR);
        return false;
    }

    // Sanity check #3 : notre buffer doit etre une string valide
    if (g_short_version_buffer[0] == '\0') {
        logger::logf("version_patch: buffer vide - patch annule");
        return false;
    }

    // Patch : redirige le push vers notre buffer
    const uint32_t new_ptr = (uint32_t)(uintptr_t)g_short_version_buffer;
    DWORD old_protect = 0;
    if (!VirtualProtect((void*)operand_addr, 4, PAGE_READWRITE, &old_protect)) {
        logger::logf("version_patch: VirtualProtect a echoue");
        return false;
    }
    *(uint32_t*)operand_addr = new_ptr;
    VirtualProtect((void*)operand_addr, 4, old_protect, &old_protect);

    g_applied = true;
    logger::logf(
        "version_patch: shortversion redirige -> \"%s\" (buf=0x%08x)",
        g_short_version_buffer, new_ptr);
    return true;
}

}  // namespace patches
