// bone_probe.cpp - see bone_probe.h
//
// CoDMP.exe 0x403d4c:  mov edx,[esi+4] ; mov eax,[esi+8] ; push edx ; call 0x486d20 ; add esp,4
// 0x486d20 = the render skeleton builder (memory cod1-dobj-render-bone-hook): the DObj is
// the stack argument, the part bits pointer travels in EAX, ESI = the renderer's entity
// record (+4 = dobj, +8 = part bits) that also carries the placement of the model. After
// the builder returns, the world matrices sit at *(uint8_t**)(dobj + 4) + 0x30, one
// 0x40-byte matrix per bone (rows at +0/+0x10/+0x20, position at +0x30), bone count =
// byte dobj + 0x17. For the 80-bone MP body: pelvis = 1, spine (back_low) = 6,
// spine2 = 24, head = 38. The matrices are in MODEL space (X forward, Y left, Z up):
// the renderer places them with the entity record's origin and axis.
#include "gameplay/bone_probe.h"
#include "core/logger.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace patches {
namespace {
constexpr uintptr_t CALL_SITE = 0x403d53;          // e8 c8 2f 08 00 = call 0x486d20
constexpr uintptr_t BUILDER   = 0x486d20;
constexpr int NTRACK = 16;
struct Track { const void* dobj; DWORD last; };
Track g_track[NTRACK] = {};

void __cdecl bone_probe(const void* dobj, const void* rent) {
    const uint8_t* d = (const uint8_t*)dobj;
    const int count = d[0x17];
    if (count < 56 || count > 130) return;
    const uint8_t* base = *(const uint8_t* const*)(d + 4);
    if (!base) return;
    const DWORD now = GetTickCount();
    int slot = -1;
    for (int i = 0; i < NTRACK; ++i) if (g_track[i].dobj == dobj) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < NTRACK; ++i) if (g_track[i].last < g_track[slot].last) slot = i;
        g_track[slot].dobj = dobj; g_track[slot].last = 0;
    }
    if (now - g_track[slot].last < 1000) return;
    g_track[slot].last = now;
    const uint8_t* mats = base + 0x30;
    const int idx[4] = { 1, 6, 24, 38 };
    const char* nm[4] = { "pelvis", "back_low", "back_up", "head" };
    char line[400];
    int n = snprintf(line, sizeof(line), "bone_probe: dobj %p n=%d", dobj, count);
    for (int k = 0; k < 4; ++k) {
        if (idx[k] >= count) break;
        const float* m = (const float*)(mats + idx[k] * 0x40);
        const float* p = m + 12;
        if (!(fabsf(p[0]) < 1e5f && fabsf(p[1]) < 1e5f && fabsf(p[2]) < 1e5f)) continue;
        n += snprintf(line + n, sizeof(line) - n, " %s (%.1f %.1f %.1f) up(%.2f %.2f %.2f)",
                      nm[k], p[0], p[1], p[2], m[0], m[1], m[2]);
    }
    logger::logf("%s", line);
    /* the renderer's entity record: every dword that looks like a world coordinate or a
     * unit-axis component, with its offset - the placement (origin + axis) of the model */
    if (rent) {
        const float* f = (const float*)rent;
        n = snprintf(line, sizeof(line), "bone_probe: rent %p", rent);
        for (int i = 0; i < 48 && n < (int)sizeof(line) - 24; ++i) {
            const float v = f[i];
            if ((fabsf(v) > 50.0f && fabsf(v) < 20000.0f) || (fabsf(v) <= 1.0f && v != 0.0f && fabsf(v) > 0.001f))
                n += snprintf(line + n, sizeof(line) - n, " +%02x=%.2f", i * 4, v);
        }
        logger::logf("%s", line);
    }
}

const void* g_builder_ptr = (const void*)BUILDER;
void (__cdecl* g_probe_ptr)(const void*, const void*) = bone_probe;

// Called in place of 0x486d20: [esp] = return, [esp+4] = dobj, eax = part bits, esi =
// the renderer's entity record (callee-saved: still intact after the builder returns).
__attribute__((naked)) void stub() {
    __asm__ volatile(
        "pushl 4(%%esp)\n\t"          /* dobj */
        "call *%0\n\t"                /* original builder: eax + stack arg intact */
        "addl $4, %%esp\n\t"
        "pushl %%eax\n\t"             /* its return value */
        "pushl %%esi\n\t"             /* entity record */
        "pushl 12(%%esp)\n\t"         /* dobj again ([esp+4] before the two pushes) */
        "call *%1\n\t"
        "addl $8, %%esp\n\t"
        "popl %%eax\n\t"
        "ret\n\t"
        : : "m"(g_builder_ptr), "m"(g_probe_ptr));
}
}  // namespace

bool bone_probe_install() {
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        logger::logf("bone_probe: CoDMP.exe not at 0x400000 - not installed");
        return false;
    }
    uint8_t* p = (uint8_t*)CALL_SITE;
    const uint8_t expect[5] = { 0xe8, 0xc8, 0x2f, 0x08, 0x00 };
    if (memcmp(p, expect, 5) != 0) {
        logger::logf("bone_probe: bytes at 0x%x are %02x %02x %02x %02x %02x, not call 0x486d20 - not installed",
                     (unsigned)CALL_SITE, p[0], p[1], p[2], p[3], p[4]);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = (int32_t)((uintptr_t)&stub - (CALL_SITE + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logger::logf("bone_probe: render skeleton call at 0x%x -> stub %p (pelvis/back/head + placement of every drawn player, 1/s)",
                 (unsigned)CALL_SITE, (void*)&stub);
    return true;
}
}  // namespace patches
