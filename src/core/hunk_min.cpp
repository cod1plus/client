// hunk_min.cpp - see hunk_min.h.
//
// CoDMP.exe 1.5, Com_InitHunkMemory 0x433fd0: com_hunkMegs = Cvar_Get("com_hunkMegs", "128",
// ARCHIVE | LATCH); below the minimum - 1 on a dedicated server, 80 for the client (0x43401a:
// mov ecx, 0x50) - it allocates the minimum and says so ("Minimum com_hunkMegs is %i, allocating
// %i megs."); there is no maximum. The hunk is allocated once, at start, with the config of the mod
// the game starts in. mp_liberation needs 166.6 MB of it (hunkusage.dat), so the default 128 died
// loading the map: "recursive error after: Hunk_AllocAlign failed on 304124" (players, 11-03).
// THE FIX = the client minimum is 256: a config at 128 (or lower) gets 256, a higher value is kept.
// mp_coastal (406 MB) still needs com_hunkmegs 512 by hand.

#include "core/hunk_min.h"
#include "core/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace patches {

namespace {
constexpr uintptr_t VA_CLIENT_MIN = 0x0043401a;      // mov ecx, 80
constexpr uint32_t  MIN_MB = 256;
const uint8_t ORIG[5] = { 0xb9, 0x50, 0x00, 0x00, 0x00 };
}  // namespace

bool hunk_min_install() {
    uint8_t* p = (uint8_t*)VA_CLIENT_MIN;
    if (memcmp(p, ORIG, sizeof(ORIG)) != 0) {
        logger::logf("hunk_min: Com_InitHunkMemory's client minimum (0x%x) is not the 1.5 one - not installed", (unsigned)VA_CLIENT_MIN);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(ORIG), PAGE_EXECUTE_READWRITE, &old)) return false;
    *(uint32_t*)(p + 1) = MIN_MB;
    VirtualProtect(p, sizeof(ORIG), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(ORIG));
    logger::logf("hunk_min: com_hunkMegs at least %u MB (was 80; mp_liberation needs 167)", (unsigned)MIN_MB);
    return true;
}

}  // namespace patches
