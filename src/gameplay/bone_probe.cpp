// bone_probe.cpp - see bone_probe.h
//
// CoDMP.exe 0x486d20 = the render skeleton builder (memory cod1-dobj-render-bone-hook):
//   int build(/*eax*/ poseMask, /*[esp+4]*/ DObj* dobj), prologue 83 EC 58 53 55 (5 clean
//   bytes), called from 8 sites. After it returns the matrices sit at
//   *(uint8_t**)(dobj + 4) + 0x30, one 0x40-byte matrix per bone (rows at +0/+0x10/+0x20 =
//   the bone's local axes, position at +0x30), bone count = byte dobj + 0x17, in MODEL
//   space (X forward, Y left, Z up).
// The ENTRY is detoured (not one call site), so every skeleton the client builds is seen -
// other players AND the local player's own body in third person.
//
// Who is it? CLIENT object map: idx = (i16)[0x8ef6f0 + 2 * handle], dobj = 0x8d6a88 + idx *
// 0x5c, and for players handle = entity number = client number (read live: handles 0, 3,
// 4, 7 = the bots, 105 bones = body 80 + head + hat; 0x8d6178 is the SERVER's map, empty
// on a client).
// Where is it drawn? At the usual call site (0x403d53) ESI is the renderer's entity record
// ([esi+4] = dobj, [esi+0x3c] = the centity_t): lerpOrigin at cent + 0x1f8, yaw at + 0x208.
// The local player is placed with the predicted playerState (cgame + 0x20af14: origin
// +0x14, viewangles +0xc0, clientNum +0xac). Other call sites fall back to cg_entities
// (base learned from the first record seen, stride 0x228).
//
// Every call hands hitbox_view the bone frames in WORLD space, so the tested hit boxes are
// drawn on the body the client draws, for everyone on screen, with no server traffic.
#include "gameplay/bone_probe.h"
#include "features/hitbox_view.h"
#include "gameplay/hitbox_client.h"
#include "core/logger.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace patches {
namespace {
constexpr uintptr_t BUILDER     = 0x486d20;
constexpr uintptr_t DOBJ_TABLE  = 0x8ef6f0;       // CLIENT object map, i16 per handle (0x8d6178 is the server's)
constexpr uintptr_t DOBJ_ARRAY  = 0x8d6a88;       // stride 0x5c
constexpr uintptr_t DOBJ_SIZE   = 0x5c;
constexpr uintptr_t RVA_PS      = 0x20af14;       // cg.predictedPlayerState
constexpr int PS_ORIGIN = 0x14, PS_VIEWANGLES = 0xc0, PS_CLIENTNUM = 0xac;
constexpr uintptr_t CENT_SIZE   = 0x228;

static bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

// client number of the player this DObj belongs to, or -1 (weapons, world models...)
static int player_of(const uint8_t* d) {
    const uintptr_t off = (uintptr_t)d - DOBJ_ARRAY;
    if ((uintptr_t)d < DOBJ_ARRAY || off % DOBJ_SIZE) return -1;
    const int idx = (int)(off / DOBJ_SIZE);
    if (idx <= 0 || idx > 0x7fff) return -1;
    const int16_t* tab = (const int16_t*)DOBJ_TABLE;
    for (int h = 0; h < 64; ++h) if (tab[h] == idx) return h;
    return -1;
}

// The body model's name: scan the DObj for a pointer to a struct whose first dword points
// to text containing "playerbody" (the XModel name). The offset is cached once found.
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
    if (g_name_off >= 0) return name_at(g_name_off);
    for (int off = 0; off <= 0x58; off += 4) {
        const char* s = name_at(off);
        if (s) { g_name_off = off; logger::logf("bone_probe: body model name found at dobj+0x%x: %s", off, s); return s; }
    }
    return nullptr;
}

uintptr_t g_cents = 0;                // cg_entities, learned from the first renderer record
unsigned  g_seen[64] = {};            // log once per client and placement source

__attribute__((force_align_arg_pointer)) void __cdecl bone_probe(const void* dobj, const void* esi) {
    const uint8_t* d = (const uint8_t*)dobj;
    const int count = d[0x17];
    if (count < 56 || count > 130) return;
    const int cn = player_of(d);
    if (cn < 0) return;
    hitbox_client_patch(dobj);        // this body's boxes = the server's, for the impacts the client predicts
    const uint8_t* mats = *(const uint8_t* const*)(d + 4);
    if (!mats) return;
    mats += 0x30;

    const uintptr_t cg = (uintptr_t)GetModuleHandleA("cgame_mp_x86.dll");
    const int local_cn = cg ? *(const int*)(cg + RVA_PS + PS_CLIENTNUM) : -1;
    const bool local = cn == local_cn;
    float org[3] = { 0, 0, 0 }, yawdeg = 0;
    int src = 0;
    // 1. the renderer's entity record of this very call
    const uint8_t* cent = nullptr;
    if (readable(esi, 0x40) && *(const void* const*)((const uint8_t*)esi + 4) == dobj) {
        const uint8_t* c = *(const uint8_t* const*)((const uint8_t*)esi + 0x3c);
        if (readable(c, 0x210) && *(const int*)c == cn) {
            cent = c; src = 1;
            if (!local && !g_cents) {
                g_cents = (uintptr_t)c - (uintptr_t)cn * CENT_SIZE;
                logger::logf("bone_probe: cg_entities at %p (from cn %d)", (void*)g_cents, cn);
            }
        }
    }
    // 2. the local player: the predicted playerState
    if (!cent && local && cg) {
        memcpy(org, (const void*)(cg + RVA_PS + PS_ORIGIN), 12);
        yawdeg = *(const float*)(cg + RVA_PS + PS_VIEWANGLES + 4);
        src = 2;
    }
    // 3. another call site: cg_entities[cn]
    if (!cent && src == 0 && g_cents) {
        const uint8_t* c = (const uint8_t*)(g_cents + (uintptr_t)cn * CENT_SIZE);
        if (readable(c, 0x210) && *(const int*)c == cn) { cent = c; src = 3; }
    }
    if (cent) { memcpy(org, cent + 0x1f8, 12); yawdeg = *(const float*)(cent + 0x208); }
    if (src == 0) return;
    if (!(g_seen[cn] & (1u << src))) {
        g_seen[cn] |= 1u << src;
        logger::logf("bone_probe: cn %d%s placed by %s (origin %.1f %.1f %.1f yaw %.0f, %d bones)", cn, local ? " (you)" : "",
                     src == 1 ? "the renderer record" : src == 2 ? "the predicted playerState" : "cg_entities",
                     org[0], org[1], org[2], yawdeg, count);
    }
    const float yaw = yawdeg * 3.14159265f / 180.0f;
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
    hitbox_view_note_pose(cn, body_name(d), n, &frames[0][0], local);
}

void* g_tramp = nullptr;              // stolen prologue + jmp BUILDER+5
void (__cdecl* g_probe_ptr)(const void*, const void*) = bone_probe;

// In place of the builder's entry: [esp] = return, [esp+4] = dobj, eax = pose mask, esi =
// whatever the caller holds (the renderer's entity record at the usual call site).
// Every register and the flags are restored around the probe: the builder is called with a
// custom convention (argument in EAX) from 8 sites, and a caller may count on registers a
// plain cdecl callee would be free to clobber.
__attribute__((naked)) void stub() {
    __asm__ volatile(
        "pushl 4(%%esp)\n\t"          /* dobj */
        "call *%0\n\t"                /* original builder through the trampoline: eax + stack arg intact */
        "addl $4, %%esp\n\t"
        "pushal\n\t"                  /* eax (the result) included */
        "pushfl\n\t"
        "pushl %%esi\n\t"
        "pushl 44(%%esp)\n\t"         /* dobj: [esp+4] before pushal (32) + pushfl (4) + esi (4) */
        "call *%1\n\t"
        "addl $8, %%esp\n\t"
        "popfl\n\t"
        "popal\n\t"
        "ret\n\t"
        : : "m"(g_tramp), "m"(g_probe_ptr));
}
}  // namespace

bool bone_probe_install() {
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        logger::logf("bone_probe: CoDMP.exe not at 0x400000 - not installed");
        return false;
    }
    uint8_t* p = (uint8_t*)BUILDER;
    const uint8_t expect[5] = { 0x83, 0xec, 0x58, 0x53, 0x55 };      // sub esp,0x58; push ebx; push ebp
    if (memcmp(p, expect, 5) != 0) {
        logger::logf("bone_probe: prologue at 0x%x is %02x %02x %02x %02x %02x - not installed",
                     (unsigned)BUILDER, p[0], p[1], p[2], p[3], p[4]);
        return false;
    }
    uint8_t* t = (uint8_t*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return false;
    memcpy(t, p, 5);
    t[5] = 0xe9;
    const int32_t back = (int32_t)((BUILDER + 5) - ((uintptr_t)t + 10));
    memcpy(t + 6, &back, 4);
    g_tramp = t;
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = (int32_t)((uintptr_t)&stub - (BUILDER + 5));
    p[0] = 0xe9;
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logger::logf("bone_probe: skeleton builder 0x%x detoured -> stub %p (every player's bone frames, your own body included, -> hitbox_view)",
                 (unsigned)BUILDER, (void*)&stub);
    return true;
}
}  // namespace patches
