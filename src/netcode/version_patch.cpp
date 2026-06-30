// patch the shortversion Cvar_Get default ptr (push @0x00439f14) -> our buffer.
// not the "1.5" string itself: "win-x86" follows it with no padding.

#include "netcode/version_patch.h"
#include "core/logger.h"

#include <cstdio>
#include <cstring>

namespace patches {

alignas(16) char g_short_version_buffer[SHORT_VERSION_MAX_LEN + 1] = "1.6";

namespace {
bool g_applied = false;
}  // namespace

bool apply_short_version_patch() {
    if (g_applied) return true;

    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) {
        logger::logf("version_patch: GetModuleHandleA(NULL) returned null - aborted");
        return false;
    }

    const uintptr_t exe_base = (uintptr_t)exe;
    const uintptr_t opcode_addr  = exe_base + CODMP_SHORTVERSION_PUSH_OPCODE_RVA;
    const uintptr_t operand_addr = exe_base + CODMP_SHORTVERSION_PUSH_OPERAND_RVA;

    const uint8_t opcode = *(const uint8_t*)opcode_addr; // must be 0x68 (push imm32)
    if (opcode != 0x68) {
        logger::logf(
            "version_patch: opcode inattendu a CoDMP+0x%lx (0x%02x, attendu 0x68). "
            "Probablement pas CoDMP.exe 1.5 - patch annule.",
            (unsigned long)CODMP_SHORTVERSION_PUSH_OPCODE_RVA, opcode);
        return false;
    }

    // must still point to original 0x005a60d0, else wrong binary version
    const uint32_t current_ptr = *(const uint32_t*)operand_addr;
    if (current_ptr != CODMP_SHORTVERSION_ORIGINAL_PTR) {
        logger::logf(
            "version_patch: pointeur actuel 0x%08x != attendu 0x%08x - patch annule.",
            current_ptr, (unsigned)CODMP_SHORTVERSION_ORIGINAL_PTR);
        return false;
    }

    if (g_short_version_buffer[0] == '\0') {
        logger::logf("version_patch: buffer vide - patch annule");
        return false;
    }

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
