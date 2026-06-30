// redirect r_fullscreen default push ptr -> our "0" (windowed). pairs with window_patch.cpp

#include "video/fullscreen_patch.h"
#include "core/logger.h"

namespace patches {

FullscreenConfig g_fullscreen_config = {
    /* force_windowed_default */ true,
};

namespace {

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

    // opcode must be push imm32 (0x68)
    const uint8_t opcode = *(const uint8_t*)opcode_addr;
    if (opcode != 0x68) {
        logger::logf(
            "fullscreen_patch: opcode inattendu a CoDMP+0x%lx (0x%02x, attendu 0x68)",
            (unsigned long)CODMP_RFULLSCREEN_PUSH_OPCODE_RVA, opcode);
        return false;
    }

    // ptr must match the one observed during RE
    const uint32_t current_ptr = *(const uint32_t*)operand_addr;
    if (current_ptr != CODMP_RFULLSCREEN_ORIGINAL_PTR) {
        logger::logf(
            "fullscreen_patch: pointeur actuel 0x%08x != attendu 0x%08x",
            current_ptr, (unsigned)CODMP_RFULLSCREEN_ORIGINAL_PTR);
        return false;
    }

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
