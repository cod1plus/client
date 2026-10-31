// smodel_light.cpp - see smodel_light.h
//
// CoDMP.exe 1.5, 0x513d90..0x513dfe (per drawn static-model surface, inside the entity's surface loop):
//   mov eax, [esi+0x2b0]; cmp [eax+ebp], 0; jne next        ; cache slot of this surface already filled
//   cmp [edi], 0; je next                                    ; surface without vertices
//   ... (the entity's light environment once: 0x4311a0 + 0x4c23d0 into [esp+0x28])
//   test byte [esi+0xa0]; jne next
//   push &env; push esi(entity); push eax(+edx); mov eax, [esi+0x2b0]; lea ecx, [edi-0xc] (the surface)
//   0x513ded call 0x506ae0 ; add esp, 0xc ; mov [ecx+ebp], eax   ; the cache entry (0 = not cached)
// 0x506ae0: surfaces over 0x200 vertices (word [surface+0x12]) return 0 at once; the others get a pool entry
// (0x506420, size class by vertex count), a temp buffer of 24 bytes per vertex (0x19b0b10), their vertices
// transformed by the entity's axis, then lit one by one (0x506510 at 0x506cfe).
#include "performance/smodel_light.h"
#include "core/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

extern "C" {
volatile long cod1x_sl_calls = 0;      // surfaces handed to the lighting
volatile long cod1x_sl_verts = 0;      // their vertices (word [surface+0x12])
volatile long cod1x_sl_big = 0;        // of which surfaces over 0x200 vertices (not cached by the engine)
const void* cod1x_sl_target = (const void*)0x00506ae0;
volatile long cod1x_sl_flat = 0;       // cvar cod1x_static_flatlight (default 0 since the 1.6.13 release: it changes
                                       // how every map's static models are lit, stock maps included; bench 10-27: p99 3.67 / max 4.6 ms vs 4.04 / 34 ms with the cache - its 65 536 vertices overflow and relight on every turn): no per-vertex lighting of static-model surfaces at all - the
                                       // call returns 0 (no cache entry), every surface takes the path the > 512-vertex ones already take
volatile long cod1x_sl_flat_skipped = 0;
}

namespace patches {
namespace {

constexpr uintptr_t VA_CALLSITE = 0x00513ded;   // E8 rel32 -> 0x506ae0
constexpr uintptr_t VA_LIGHT    = 0x00506ae0;
// RE_BeginFrame 0x4eadba: `mov edx, [0x199bbbc] (r_vbo_paranoia); cmp [edx+0x20], edi; je +0x16` - when the cvar is
// set, every frame calls 0x4ff490 then 0x515730(3) / 0x5122c0(3): round-robin over the GPU vertex buffers that
// glDeleteBuffersARB + glBufferSubDataARB them again (a 2003 guard against drivers losing VBO contents; 0x515730
// takes a 256 KB stack buffer). The static-model cache (r_smc_enable, its surfaces drawn from static VBOs) keeps
// being thrown away and rebuilt: per-vertex relighting, uploads, driver work on the game thread. The engine's
// default is 0; pak9.pk3 theendlesswinter_mp.cfg (an Activision developer config) writes `seta r_vbo_paranoia "1"`
// and players' configs inherited it. The `je` becomes `jmp`: never run, whatever the cvar says.
constexpr uintptr_t VA_PARANOIA = 0x004eadba;
const uint8_t PARANOIA_BYTES[11] = {0x8B, 0x15, 0xBC, 0xBB, 0x99, 0x01, 0x39, 0x7A, 0x20, 0x74, 0x16};

bool g_installed = false;
long g_rep[3] = {0, 0, 0};
long g_frame_calls = 0, g_frame_verts = 0, g_peak_calls = 0, g_peak_verts = 0;
int g_last_frame = -1;
ULONGLONG g_last_log = 0;

// Entered like the engine's function (eax = [entity+0x2b0], ecx = surface, three stack arguments, the
// caller's `add esp, 0xc` cleans them): count, then tail-jump to the engine with everything intact.
__attribute__((naked)) void light_hook() {
    __asm__ volatile(
        "cmpl $0, _cod1x_sl_flat\n\t"
        "je 2f\n\t"
        "incl _cod1x_sl_flat_skipped\n\t"
        "xorl %%eax, %%eax\n\t"              // "not cached": the flat per-entity light, like a big surface
        "ret\n\t"
        "2:\n\t"
        "pushl %%edx\n\t"
        "incl _cod1x_sl_calls\n\t"
        "movzwl 0x12(%%ecx), %%edx\n\t"
        "addl %%edx, _cod1x_sl_verts\n\t"
        "cmpl $0x200, %%edx\n\t"
        "jbe 1f\n\t"
        "incl _cod1x_sl_big\n\t"
        "1:\n\t"
        "popl %%edx\n\t"
        "jmp *_cod1x_sl_target\n\t"
        : : : "memory");
}

bool patch_callsite(uintptr_t site, uintptr_t expect, uintptr_t to) {
    uint8_t* p = (uint8_t*)site;
    if (p[0] != 0xE8) return false;
    int32_t rel; memcpy(&rel, p + 1, 4);
    if ((uintptr_t)(site + 5 + rel) != expect) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    rel = (int32_t)(to - (site + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

}  // namespace

void smodel_light_install() {
    if (g_installed) return;
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return;
    {
        uint8_t* p = (uint8_t*)VA_PARANOIA;
        DWORD old = 0;
        if (memcmp(p, PARANOIA_BYTES, sizeof PARANOIA_BYTES) == 0 && VirtualProtect(p + 9, 1, PAGE_EXECUTE_READWRITE, &old)) {
            p[9] = 0xEB;                                   // je -> jmp
            VirtualProtect(p + 9, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p + 9, 1);
            logger::logf("smodel_light: r_vbo_paranoia neutralised (RE_BeginFrame 0x%08x: no per-frame delete + re-upload of the GPU vertex buffers, "
                         "the static-model cache stays on the GPU)", (unsigned)VA_PARANOIA);
        } else {
            logger::logf("smodel_light: r_vbo_paranoia check at 0x%08x not as expected - left alone", (unsigned)VA_PARANOIA);
        }
    }
    if (!patch_callsite(VA_CALLSITE, VA_LIGHT, (uintptr_t)&light_hook)) {
        logger::logf("smodel_light: the surface lighting call at 0x%08x is not `call 0x%08x` - not counted", (unsigned)VA_CALLSITE, (unsigned)VA_LIGHT);
        return;
    }
    g_installed = true;
    logger::logf("smodel_light: DIAGNOSTIC - static-model surface lighting (0x%08x -> 0x%08x) counted, a line every 10 s", (unsigned)VA_CALLSITE, (unsigned)VA_LIGHT);
}

typedef void* (__cdecl* SlCvarGet_t)(const char*, const char*, int);
static void* g_cv_flat = nullptr;

void smodel_light_tick() {
    if (!g_installed) return;
    if (!g_cv_flat) g_cv_flat = ((SlCvarGet_t)0x0043b880)("cod1x_static_flatlight", "0", 0x01);   // Cvar_Get, CVAR_ARCHIVE
    if (g_cv_flat) cod1x_sl_flat = *(const int*)((const uint8_t*)g_cv_flat + 0x20) ? 1 : 0;
    const ULONGLONG now = GetTickCount64();
    if (g_last_log && now - g_last_log < 10000) return;
    const long cur[3] = {cod1x_sl_calls, cod1x_sl_verts, cod1x_sl_big};
    long d[3];
    for (int i = 0; i < 3; ++i) { d[i] = cur[i] - g_rep[i]; g_rep[i] = cur[i]; }
    if (g_last_log && d[0] > 0) {
        logger::logf("smodel_light: last 10 s - %ld static-model surfaces lit vertex by vertex, %ld vertices (%ld surfaces over 512 vertices, "
                     "which the engine refuses to cache: 0x506ae0 returns at once for them)", d[0], d[1], d[2]);
    }
    static long flat_rep = 0;
    const long fd = cod1x_sl_flat_skipped - flat_rep;
    flat_rep = cod1x_sl_flat_skipped;
    if (g_last_log && fd > 0) {
        logger::logf("smodel_light: last 10 s - cod1x_static_flatlight 1: %ld surface lightings skipped (flat per-entity light instead)", fd);
    }
    g_last_log = now;
}

}  // namespace patches
