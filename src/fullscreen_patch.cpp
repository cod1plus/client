// Patch du default value de r_fullscreen dans CoDMP.exe.
//
// But : forcer le jeu a demarrer en mode windowed (r_fullscreen=0) pour
// permettre l'alt-tab instantane. Combine avec window_patch.cpp qui rend
// la fenetre borderless plein ecran, ca donne l'apparence du fullscreen
// avec la fluidite du windowed.
//
// Le patch redirige le pointeur du `push` vers notre propre string "0".

#include "fullscreen_patch.h"
#include "logger.h"

namespace patches {

FullscreenConfig g_fullscreen_config = {
    /* force_windowed_default */ true,
};

namespace {

// Notre default value de remplacement, allouee dans la .data de notre DLL.
// On garde un peu d'espace au cas ou on voudrait y ecrire autre chose plus
// tard (genre "0\n" pour des cas particuliers).
alignas(16) const char g_fullscreen_default_str[] = "0";

bool g_applied = false;

}  // namespace

bool apply_fullscreen_patch() {
    if (g_applied) return true;
    if (!g_fullscreen_config.force_windowed_default) {
        logger::logf("fullscreen_patch: disabled in config, skipping");
        return true;
    }

    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) {
        logger::logf("fullscreen_patch: GetModuleHandleA(NULL) returned null");
        return false;
    }

    const uintptr_t exe_base = (uintptr_t)exe;
    const uintptr_t opcode_addr  = exe_base + CODMP_RFULLSCREEN_PUSH_OPCODE_RVA;
    const uintptr_t operand_addr = exe_base + CODMP_RFULLSCREEN_PUSH_OPERAND_RVA;

    // Sanity #1 : opcode push imm32
    const uint8_t opcode = *(const uint8_t*)opcode_addr;
    if (opcode != 0x68) {
        logger::logf(
            "fullscreen_patch: opcode inattendu a CoDMP+0x%lx (0x%02x, attendu 0x68)",
            (unsigned long)CODMP_RFULLSCREEN_PUSH_OPCODE_RVA, opcode);
        return false;
    }

    // Sanity #2 : pointeur actuel = celui qu'on a observe en RE
    const uint32_t current_ptr = *(const uint32_t*)operand_addr;
    if (current_ptr != CODMP_RFULLSCREEN_ORIGINAL_PTR) {
        logger::logf(
            "fullscreen_patch: pointeur actuel 0x%08x != attendu 0x%08x",
            current_ptr, (unsigned)CODMP_RFULLSCREEN_ORIGINAL_PTR);
        return false;
    }

    // Patch : redirige vers notre "0"
    const uint32_t new_ptr = (uint32_t)(uintptr_t)&g_fullscreen_default_str[0];
    DWORD old_protect = 0;
    if (!VirtualProtect((void*)operand_addr, 4, PAGE_READWRITE, &old_protect)) {
        logger::logf("fullscreen_patch: VirtualProtect a echoue");
        return false;
    }
    *(uint32_t*)operand_addr = new_ptr;
    VirtualProtect((void*)operand_addr, 4, old_protect, &old_protect);

    g_applied = true;
    logger::logf(
        "fullscreen_patch: r_fullscreen default redirige -> \"%s\" (ptr=0x%08x)",
        g_fullscreen_default_str, new_ptr);
    return true;
}

}  // namespace patches
