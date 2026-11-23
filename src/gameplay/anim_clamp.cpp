// anim_clamp.cpp - out-of-range player animation indices are played as animation 0 instead of
// dropping the player to the main menu. See anim_clamp.h. Mirror of the server patch in
// cod1plushookserver/src/anim_clamp.c.
//
// Where it bites: an index the client's animation table does not have. Live, when client and
// server register a slightly different count (modded PAM servers); and WATCHING A DEMO: the demo
// carries the recorded server's indices (its mod's playeranim.script) while the client plays it
// with its own, shorter table -> "Player animation index out of range (251): 251" -> menu.
//
// Main/cgame_mp_x86.dll (md5 075e2af1, image base 0x30000000):
//   +0x3e40  the lerp-frame runner, called by CG_PlayerAnimation (+0x5363 legs, +0x5373 torso)
//            with the entity's legsAnim / torsoAnim as its first stack argument; it calls the
//            setter below when the value changed, then reads lf->animation (+0x14) unless
//            (value & ~0x200) == 0;
//   +0x3870  the setter (CG_SetLerpFrameAnimation): (value & ~0x200) vs the count [0x300fdda0]
//            -> "Player animation index out of range (%i): %i", error block +0x38d6;
//   +0x37e0, +0x3820  two flag readers the setter calls with the old and the new index (their
//            own bounds check, error blocks +0x37f8 / +0x3838; esi is their own, pushed/popped);
//   +0x3580  BG_GetAnimationForIndex and +0x3430 BG_GetAnimString: no caller in this cgame.
//
// THE FIX = the runner's entry: an out-of-range value becomes 0 (toggle bit 0x200 kept) before
// anything reads it - exactly what the game does when the server sends animation 0 (no
// animation), a value the runner and the setter handle themselves. The setter stays vanilla.
//
// 2026-10-09: the previous version (10-06) zeroed the index INSIDE the setter's error block: the
// setter then left lf->animation = NULL while the runner still held the raw, non-zero value and
// read lf->animation->+0x44 -> access violation at cgame+0x3ec4 (two Windows "Application Error"
// reports), the game hung in exclusive fullscreen and the PC looked frozen. Never reached before
// because its byte check also failed whenever cgame was relocated (see abs_off).
//
// The four other checks keep their fallback (never reached once the entry is clamped; kept as a
// second line): the readers `xor esi,esi`, the uncalled ones `ret` (eax = &animation[0]).

#include "gameplay/anim_clamp.h"
#include "core/logger.h"

#include <cstdint>
#include <cstring>

namespace patches {

namespace {

constexpr uintptr_t CGAME_IMAGE_BASE = 0x30000000;   // the preferred base the absolute operands assume
constexpr uintptr_t RVA_RUNNER       = 0x3e40;
constexpr uintptr_t RVA_ANIM_COUNT   = 0xfdda0;      // [0x300fdda0] = the registered animation count
// sub esp,0x10; push ebx; push ebp | mov ebp,[esp+0x1c] (checked, not moved; no jump lands inside)
const uint8_t RUNNER_PROLOGUE[9] = { 0x83, 0xec, 0x10, 0x53, 0x55, 0x8b, 0x6c, 0x24, 0x1c };

struct Site {
    uintptr_t rva;
    int len;
    int abs_off;          // the error message's `push imm32`: an absolute address, relocated when
                          // cgame does not get its preferred base (seen after a vid_restart that
                          // switched mod: 0x46f80000) - compared with the load's delta added
    uint8_t orig[15];     // error block (the call rel32s are position-relative: stable across loads)
    uint8_t patch[15];
};
const Site SITES[] = {
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

// the setter's error block, vanilla again: the 10-06 build had `xor ebx,ebx; mov [esp+0x3c],ebx`
// there (the crash above) - restored if found
constexpr uintptr_t RVA_SETTER_BLOCK = 0x38d6;
const uint8_t SETTER_ORIG[17] = { 0x53, 0x50, 0x68, 0x8c, 0xaa, 0x06, 0x30, 0x6a, 0x01, 0xe8, 0x2c, 0xe2, 0x01, 0x00, 0x83, 0xc4, 0x10 };
const uint8_t SETTER_OLD_PATCH[17] = { 0x31, 0xdb, 0x89, 0x5c, 0x24, 0x3c, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

uint8_t* g_stub = nullptr;            // one executable page, rewritten for each cgame load
volatile LONG g_hits = 0;             // values neutralised by the stub (incremented by it)

// the original bytes as they sit in memory for this load (the push imm32 at `off` relocated)
void relocated(const uint8_t* orig, int len, int off, uintptr_t base, uint8_t* out) {
    memcpy(out, orig, len);
    uint32_t imm;
    memcpy(&imm, out + off, 4);
    imm += (uint32_t)(base - CGAME_IMAGE_BASE);
    memcpy(out + off, &imm, 4);
}

bool write_code(uint8_t* at, const uint8_t* bytes, int len) {
    DWORD old = 0;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, len);
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return true;
}

// entry stub of the runner for a cgame at `base`:
//   push edx; mov edx,[esp+8]; and edx,~0x200; cmp edx,[count]; jb ok
//   and dword [esp+8],0x200; lock inc dword [g_hits]
//   ok: pop edx; sub esp,0x10; push ebx; push ebp; jmp runner+5
int build_stub(uint8_t* s, uintptr_t base) {
    int n = 0;
    auto b = [&](uint8_t v) { s[n++] = v; };
    auto d = [&](uint32_t v) { memcpy(s + n, &v, 4); n += 4; };
    b(0x52);                                              // push edx
    b(0x8b); b(0x54); b(0x24); b(0x08);                   // mov edx,[esp+8]   (the value)
    b(0x81); b(0xe2); d(0xfffffdffu);                     // and edx,~0x200
    b(0x3b); b(0x15); d((uint32_t)(base + RVA_ANIM_COUNT));   // cmp edx,[count]
    b(0x72); b(0x0f);                                     // jb ok (+15)
    b(0x81); b(0x64); b(0x24); b(0x08); d(0x200);         // and dword [esp+8],0x200   (8 bytes)
    b(0xf0); b(0xff); b(0x05); d((uint32_t)(uintptr_t)&g_hits);   // lock inc dword [g_hits] (7 bytes)
    b(0x5a);                                              // ok: pop edx
    b(0x83); b(0xec); b(0x10);                            // sub esp,0x10
    b(0x53); b(0x55);                                     // push ebx; push ebp
    b(0xe9);                                              // jmp runner+5
    const uint32_t rel = (uint32_t)(base + RVA_RUNNER + 5 - ((uintptr_t)s + n + 4));
    d(rel);
    return n;
}

}  // namespace

bool apply_anim_clamp(HMODULE cgame_module) {
    if (!cgame_module) return false;
    const uintptr_t base = (uintptr_t)cgame_module;
    uint8_t* runner = (uint8_t*)(base + RVA_RUNNER);

    const LONG hits = InterlockedExchange(&g_hits, 0);
    if (hits) logger::logf("  anim_clamp: %ld out-of-range animation value(s) played as animation 0 since the last cgame load", (long)hits);

    // the runner's entry now: vanilla, or already our jmp to the stub
    uint8_t jmp[5] = { 0xe9 };
    bool entry_ours = false;
    if (g_stub) {
        const int32_t rel = (int32_t)((uintptr_t)g_stub - ((uintptr_t)runner + 5));
        memcpy(jmp + 1, &rel, 4);
        entry_ours = memcmp(runner, jmp, 5) == 0;
    }
    if (entry_ours ? memcmp(runner + 5, RUNNER_PROLOGUE + 5, 4) != 0
                   : memcmp(runner, RUNNER_PROLOGUE, sizeof(RUNNER_PROLOGUE)) != 0) {
        logger::logf("  anim_clamp: cgame+0x%x = %02x %02x %02x %02x %02x (not the expected lerp runner, base 0x%08x) - different cgame build, skip",
                     (unsigned)RVA_RUNNER, runner[0], runner[1], runner[2], runner[3], runner[4], (unsigned)base);
        return false;
    }

    // every other site as expected (vanilla or ours), else nothing at all
    uint8_t want[17];
    for (int i = 0; i < NSITES; ++i) {
        const uint8_t* site = (const uint8_t*)(base + SITES[i].rva);
        if (memcmp(site, SITES[i].patch, SITES[i].len) == 0) continue;
        relocated(SITES[i].orig, SITES[i].len, SITES[i].abs_off, base, want);
        if (memcmp(site, want, SITES[i].len) != 0) {
            logger::logf("  anim_clamp: cgame+0x%x = %02x %02x %02x %02x %02x (not the expected error block, base 0x%08x) - different cgame build, skip",
                         (unsigned)SITES[i].rva, site[0], site[1], site[2], site[3], site[4], (unsigned)base);
            return false;
        }
    }
    uint8_t* setter = (uint8_t*)(base + RVA_SETTER_BLOCK);
    relocated(SETTER_ORIG, 17, 3, base, want);
    const bool setter_old_patch = memcmp(setter, SETTER_OLD_PATCH, 17) == 0;
    if (!setter_old_patch && memcmp(setter, want, 17) != 0) {
        logger::logf("  anim_clamp: cgame+0x%x = %02x %02x %02x (not the setter's error block, base 0x%08x) - different cgame build, skip",
                     (unsigned)RVA_SETTER_BLOCK, setter[0], setter[1], setter[2], (unsigned)base);
        return false;
    }

    // 1. the stub, built for this base (the count's address moves with cgame)
    if (!g_stub) {
        g_stub = (uint8_t*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_stub) { logger::logf("  anim_clamp: no stub memory - not installed"); return false; }
    }
    if (!entry_ours) {
        build_stub(g_stub, base);
        FlushInstructionCache(GetCurrentProcess(), g_stub, 64);
    }
    // 2. the setter back to vanilla if the 10-06 patch is there
    if (setter_old_patch && !write_code(setter, want, 17)) return false;
    // 3. second-line sites
    for (int i = 0; i < NSITES; ++i) {
        uint8_t* site = (uint8_t*)(base + SITES[i].rva);
        if (memcmp(site, SITES[i].patch, SITES[i].len) == 0) continue;
        if (!write_code(site, SITES[i].patch, SITES[i].len)) return false;
    }
    // 4. the runner's entry last: from here on every value it gets is in range
    if (!entry_ours) {
        const int32_t rel = (int32_t)((uintptr_t)g_stub - ((uintptr_t)runner + 5));
        memcpy(jmp + 1, &rel, 4);
        if (!write_code(runner, jmp, 5)) return false;
        logger::logf("  anim_clamp: out-of-range player animation values are played as animation 0 "
                     "(lerp runner entry cgame+0x%x, cgame at 0x%08x; no drop to the menu on a demo recorded on a modded server)",
                     (unsigned)RVA_RUNNER, (unsigned)base);
    }
    return true;
}

}  // namespace patches
