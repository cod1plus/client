// anim_clamp.cpp - out-of-range player animation indices fall back to animation 0 instead of
// dropping the player to the main menu. See anim_clamp.h. Mirror of the server patch in
// cod1plushookserver/src/anim_clamp.c.
//
// Where it bites: an index the client's animation table does not have. Live, when client and
// server register a slightly different count (modded PAM servers); and WATCHING A DEMO from the
// main menu: the demo carries the recorded server's indices (its mod's playeranim.script) while
// the client plays it with its own, shorter table -> "BG_GetAnimationForIndex: index out of
// bounds" -> back to the menu.
//
// Main/cgame_mp_x86.dll (md5 075e2af1) has FIVE such checks, each followed by its error block:
//   +0x3870  the lerp-frame animation setter (CG_SetLerpFrameAnimation): the entity's new
//            animation (ebx, toggle bit off) vs the count at [0x300fdda0] ->
//            "Player animation index out of range (%i): %i" (the 2026-07 report), error block
//            +0x38d6, 17 bytes. ebx and its stack copy [esp+0x3c] become 0: the setter's own
//            "animation 0" path (lerp animation = none), which the game takes for index 0
//            anyway; lf->animationNumber keeps the raw value, so the same out-of-range index
//            is not re-set every frame. THE one an entity's state reaches - demos included -
//            and it calls the two inlined flag readers below with the old and the new index;
//   +0x3580  BG_GetAnimationForIndex (no caller in this cgame: patched for completeness)
//            index in esi = the CALLER's register (not saved here)
//            mov eax,[0x300f0fbc]; cmp esi,[eax+0xb800]; jb ok; <error +0x358d, 15 bytes>
//            ok: mov ecx,[0x300f0fbc]; mov eax,esi; imul eax,0x5c; add eax,ecx; ret
//   +0x37e0, +0x3820  two copies of it inlined in flag readers: push esi; mov esi,eax; ...;
//            <error +0x37f8 / +0x3838, 15 bytes>; ok: imul esi,0x5c; read the entry; pop esi
//   +0x3430  BG_GetAnimString (no caller either): same shape, <error +0x343d, 13 bytes>
// The 2026-07 version wrote `xor esi,esi` over the first block and was REVERTED (camera / mouse
// glitches): esi there belongs to the caller, which still uses it (the old path never returned,
// so it never mattered). Now:
//   - first and last: the error block becomes `ret`. eax already holds the table pointer,
//     i.e. &animation[0] - exactly what the OK path returns for index 0 - and no register the
//     caller relies on is touched;
//   - the two inlined copies: `xor esi,esi` is safe THERE (esi is their own, pushed/popped).

#include "gameplay/anim_clamp.h"
#include "core/logger.h"

#include <cstdint>
#include <cstring>

namespace patches {

namespace {

constexpr uintptr_t CGAME_IMAGE_BASE = 0x30000000;   // the preferred base the error blocks' push imm32 assume

struct Site {
    uintptr_t rva;
    int len;
    int abs_off;          // the error message's `push imm32`: an absolute address, relocated when
                          // cgame does not get its preferred base (seen after a vid_restart that
                          // switched mod: 0x46f80000) - compared with the load's delta added
    uint8_t orig[17];     // error block (the call rel32s are position-relative: stable across loads)
    uint8_t patch[17];
};
const Site SITES[] = {
    { 0x38d6, 17, 3, { 0x53, 0x50, 0x68, 0x8c, 0xaa, 0x06, 0x30, 0x6a, 0x01, 0xe8, 0x2c, 0xe2, 0x01, 0x00, 0x83, 0xc4, 0x10 },
                  { 0x31, 0xdb, 0x89, 0x5c, 0x24, 0x3c, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },   // xor ebx,ebx; mov [esp+0x3c],ebx
    { 0x358d, 15, 1, { 0x68, 0xbc, 0xaa, 0x06, 0x30, 0x6a, 0x01, 0xe8, 0x77, 0xe5, 0x01, 0x00, 0x83, 0xc4, 0x08 },
                  { 0xc3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },   // ret
    { 0x37f8, 15, 1, { 0x68, 0xbc, 0xaa, 0x06, 0x30, 0x6a, 0x01, 0xe8, 0x0c, 0xe3, 0x01, 0x00, 0x83, 0xc4, 0x08 },
                  { 0x31, 0xf6, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },   // xor esi,esi
    { 0x3838, 15, 1, { 0x68, 0xbc, 0xaa, 0x06, 0x30, 0x6a, 0x01, 0xe8, 0xcc, 0xe2, 0x01, 0x00, 0x83, 0xc4, 0x08 },
                  { 0x31, 0xf6, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },   // xor esi,esi
    { 0x343d, 13, 1, { 0x68, 0xec, 0xaa, 0x06, 0x30, 0xe8, 0xd9, 0xdd, 0xff, 0xff, 0x83, 0xc4, 0x04 },
                  { 0xc3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },               // ret
};
constexpr int NSITES = sizeof(SITES) / sizeof(SITES[0]);

// the original block as it sits in memory for this load (its push imm32 relocated)
void expected_orig(const Site& st, uintptr_t base, uint8_t* out) {
    memcpy(out, st.orig, st.len);
    uint32_t imm;
    memcpy(&imm, out + st.abs_off, 4);
    imm += (uint32_t)(base - CGAME_IMAGE_BASE);
    memcpy(out + st.abs_off, &imm, 4);
}

}  // namespace

bool apply_anim_clamp(HMODULE cgame_module) {
    if (!cgame_module) return false;
    const uintptr_t base = (uintptr_t)cgame_module;
    // all or nothing: a different cgame build leaves every site untouched
    int already = 0;
    for (int i = 0; i < NSITES; ++i) {
        const uint8_t* site = (const uint8_t*)(base + SITES[i].rva);
        if (memcmp(site, SITES[i].patch, SITES[i].len) == 0) { ++already; continue; }
        uint8_t want[17];
        expected_orig(SITES[i], base, want);
        if (memcmp(site, want, SITES[i].len) != 0) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                logger::logf("  anim_clamp: cgame+0x%x = %02x %02x %02x %02x %02x %02x %02x (not the expected error block, base 0x%08x) - different cgame build, skip",
                             (unsigned)SITES[i].rva, site[0], site[1], site[2], site[3], site[4], site[5], site[6], (unsigned)base);
            }
            return false;
        }
    }
    if (already == NSITES) return true;
    for (int i = 0; i < NSITES; ++i) {
        uint8_t* site = (uint8_t*)(base + SITES[i].rva);
        if (memcmp(site, SITES[i].patch, SITES[i].len) == 0) continue;
        DWORD old = 0;
        if (!VirtualProtect(site, SITES[i].len, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(site, SITES[i].patch, SITES[i].len);
        VirtualProtect(site, SITES[i].len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, SITES[i].len);
    }
    logger::logf("  anim_clamp: out-of-range player animation indices fall back to animation 0 "
                 "(5 cgame checks, cgame at 0x%08x; no more drop to the menu, e.g. watching a demo recorded on a modded server)",
                 (unsigned)base);
    return true;
}

}  // namespace patches
