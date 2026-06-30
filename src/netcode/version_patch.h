#ifndef COD1RELOADED_VERSION_PATCH_H
#define COD1RELOADED_VERSION_PATCH_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// RVAs vs image base 0x00400000. `push 0x005a60d0` @0x00439f14 = ptr to shortversion "1.5" default.
constexpr uintptr_t CODMP_PREFERRED_BASE = 0x00400000;
constexpr uintptr_t CODMP_SHORTVERSION_PUSH_OPCODE_RVA  = 0x00039f14; // opcode 0x68
constexpr uintptr_t CODMP_SHORTVERSION_PUSH_OPERAND_RVA = 0x00039f15; // 4 bytes to patch
constexpr uintptr_t CODMP_SHORTVERSION_ORIGINAL_PTR     = 0x005a60d0; // -> "1.5\0"

constexpr size_t SHORT_VERSION_MAX_LEN = 63;

// engine treats dvar as CVAR_ROM (flags 0x44) so never writes here
extern char g_short_version_buffer[SHORT_VERSION_MAX_LEN + 1];

bool apply_short_version_patch();

}  // namespace patches

#endif
