// bone_probe.cpp - see bone_probe.h
//
// CoDMP.exe 0x403d4c:  mov edx,[esi+4] ; mov eax,[esi+8] ; push edx ; call 0x486d20 ; add esp,4
// 0x486d20 = the render skeleton builder (memory cod1-dobj-render-bone-hook): the DObj is
// the stack argument, the part bits pointer travels in EAX, ESI = the renderer's entity
// record (+4 = dobj, +8 = part bits, +0x3c = the centity_t drawn). After the builder returns
// the world matrices sit at *(uint8_t**)(dobj + 4) + 0x30, one 0x40-byte matrix per bone
// (rows at +0/+0x10/+0x20 = the bone's local axes, position at +0x30), bone count = byte
// dobj + 0x17, in MODEL space (X forward, Y left, Z up): the renderer places them with the
// entity's lerpOrigin (cent + 0x1f8) and yaw (cent + 0x208).
//
// Every call (= every frame, for every player drawn) hands hitbox_view the 80 bone frames
// in WORLD space plus the body model's name, so the tested hit boxes can be drawn on the
// body the client draws, for every player on screen, without any server traffic.
#include "gameplay/bone_probe.h"
#include "features/hitbox_view.h"
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

static bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

// The body model's name from the DObj: the DObj holds pointers to its XModels (body, head,
// helmet, weapon); an XModel starts with the pointer to its name ("playerbody_..." on the
// server's copy, memory cod1-hitbox-boxes-fix). Found by scanning the 0x5c-byte DObj for a
// pointer whose first dword points to text containing "playerbody"; the offset is cached.
static int g_name_off = -1;
static const char* body_name(const uint8_t* d) {
    auto name_at = [&](int off) -> const char* {
        const uintptr_t p = *(const uintptr_t*)(d + off);
        if (p < 0x10000 || (p & 3) || !readable((const void*)p, 8)) return nullptr;
        const char* s = *(const char* const*)p;
        if (!readable(s, 64)) return nullptr;
        for (int i = 0; i < 64; ++i) {
            const char c = s[i];
            if (c == 0) return (i > 10 && strstr(s, "playerbody")) ? s : nullptr;
            if (c < 0x20 || c > 0x7e) return nullptr;
        }
        return nullptr;
    };
    if (g_name_off >= 0) {
        const char* s = name_at(g_name_off);
        if (s) return s;
    }
    for (int off = 0; off <= 0x58; off += 4) {
        const char* s = name_at(off);
        if (s) { if (g_name_off != off) { g_name_off = off; logger::logf("bone_probe: body model name found at dobj+0x%x: %s", off, s); } return s; }
    }
    // not this DObj's model (head / weapon DObjs share the pass): scan the pointed structs one level deeper
    for (int off = 0; off <= 0x58; off += 4) {
        const uintptr_t p = *(const uintptr_t*)(d + off);
        if (p < 0x10000 || (p & 3) || !readable((const void*)p, 0x40)) continue;
        for (int k = 0; k < 0x40; k += 4) {
            const uintptr_t q = *(const uintptr_t*)(p + k);
            if (q < 0x10000 || (q & 3) || !readable((const void*)q, 8)) continue;
            const char* s = *(const char* const*)q;
            if (!readable(s, 64)) continue;
            bool ok = false;
            for (int i = 0; i < 64; ++i) { const char c = s[i]; if (c == 0) { ok = i > 10; break; } if (c < 0x20 || c > 0x7e) break; }
            if (ok && strstr(s, "playerbody")) return s;
        }
    }
    return nullptr;
}

void __cdecl bone_probe(const void* dobj, const void* rent) {
    const uint8_t* d = (const uint8_t*)dobj;
    const int count = d[0x17];
    if (count < 56 || count > 130 || !rent) return;
    const uint8_t* mats = *(const uint8_t* const*)(d + 4);
    if (!mats) return;
    mats += 0x30;
    const uint8_t* cent = *(const uint8_t* const*)((const uint8_t*)rent + 0x3c);
    if (!readable(cent, 0x210)) return;
    const int cn = *(const int*)cent;                         // currentState.number
    if (cn < 0 || cn >= 64) return;
    const float* org = (const float*)(cent + 0x1f8);
    const float yaw = *(const float*)(cent + 0x208) * 3.14159265f / 180.0f;
    const float cy = cosf(yaw), sy = sinf(yaw);
    static float frames[128][12];
    const int n = count < 128 ? count : 128;
    for (int b = 0; b < n; ++b) {
        const float* m = (const float*)(mats + b * 0x40);
        float* f = frames[b];
        for (int r = 0; r < 3; ++r) {                        // rows = local axes -> rotate by yaw
            const float x = m[r * 4], y = m[r * 4 + 1], z = m[r * 4 + 2];
            f[r * 3] = cy * x - sy * y; f[r * 3 + 1] = sy * x + cy * y; f[r * 3 + 2] = z;
        }
        f[9]  = org[0] + cy * m[12] - sy * m[13];
        f[10] = org[1] + sy * m[12] + cy * m[13];
        f[11] = org[2] + m[14];
    }
    hitbox_view_note_pose(cn, body_name(d), n, &frames[0][0]);
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
    logger::logf("bone_probe: render skeleton call at 0x%x -> stub %p (world bone frames of every drawn player -> hitbox_view)",
                 (unsigned)CALL_SITE, (void*)&stub);
    return true;
}
}  // namespace patches
