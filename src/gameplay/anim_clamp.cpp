// anim_clamp.cpp - clamp BG_GetAnimationForIndex out-of-range instead of kicking.
// See anim_clamp.h. Mirror of the server patch in cod1plushookserver/src/anim_clamp.c.
//
// Target: Main/cgame_mp_x86.dll (the MP cgame). BG_GetAnimationForIndex bounds check:
//   cgame+0x3585  mov eax,[0x300f0fbc]           ; animation table
//   cgame+0x3585  cmp esi,[eax+0xb800]           ; index (esi) vs animCount (~251)
//   cgame+0x358b  jb  0x359c                     ; in-range -> OK
//   cgame+0x358d  push 0x3006aabc; push 1; call Com_Error; add esp,8   ; <-- error path
//   cgame+0x359c  mov ecx,[..]; mov eax,esi; imul eax,0x5c; add; ret   ; OK path uses esi
// Patch @+0x358d: replace the 15-byte error block with `xor esi,esi` + NOPs, so the
// OK path runs with esi=0 -> returns animation[0] (valid default pose), no kick.

#include "gameplay/anim_clamp.h"
#include "core/logger.h"

#include <cstdint>
#include <cstring>

namespace patches {

namespace {

constexpr uintptr_t CGAME_ANIMCHECK_RVA = 0x358d;   // start of the error block
constexpr int       PATCH_LEN           = 15;       // 0x359c - 0x358d

// original: push 0x3006aabc ; push 1 ; call rel32 ; add esp,8  (rel32 is position-
// relative -> stable across ASLR loads, safe to match exactly)
const uint8_t ORIG[PATCH_LEN] = {
    0x68, 0xbc, 0xaa, 0x06, 0x30,  // push 0x3006aabc
    0x6a, 0x01,                    // push 1
    0xe8, 0x77, 0xe5, 0x01, 0x00,  // call Com_Error
    0x83, 0xc4, 0x08               // add esp, 8
};
// replacement: xor esi,esi (clamp index to 0) + NOP padding
const uint8_t PATCH[PATCH_LEN] = {
    0x31, 0xf6,                                            // xor esi, esi
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,        // nop x13
    0x90, 0x90, 0x90
};

}  // namespace

bool apply_anim_clamp(HMODULE cgame_module) {
    if (!cgame_module) return false;
    uint8_t* site = (uint8_t*)((uintptr_t)cgame_module + CGAME_ANIMCHECK_RVA);

    if (memcmp(site, PATCH, PATCH_LEN) == 0) return true;    // already patched
    if (memcmp(site, ORIG, PATCH_LEN) != 0) {                // different cgame build
        static bool logged = false;
        if (!logged) {
            logged = true;
            logger::logf("  anim_clamp: cgame+0x%x = %02x %02x %02x (expected 68 bc aa) "
                         "- different cgame build, skip",
                         (unsigned)CGAME_ANIMCHECK_RVA, site[0], site[1], site[2]);
        }
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect(site, PATCH_LEN, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site, PATCH, PATCH_LEN);
    VirtualProtect(site, PATCH_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, PATCH_LEN);
    logger::logf("  anim_clamp: BG_GetAnimationForIndex out-of-range clamps to 0 "
                 "(no more animation-index kick)");
    return true;
}

}  // namespace patches
