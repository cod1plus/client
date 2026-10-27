// smodel_path.cpp - which backend path the static-model surfaces take, per frame.
//
// CoDMP.exe 1.5 renderer, static models (misc_model): at map load every static surface gets a type from its
// shader's flags ([surf+8] = shader, [shader+0x50] = flags, built in 0x4fbcb0: rgbGen lightingDiffuse = 0x102,
// a second texture bundle 0x400+, image animation 0x80000, tcGen/tcMod 0x100000<<bundle, deforms 0x40000 /
// 0x20000000), per GL path (Generic 0x512500, NV 0x512660, ATI, ARB 0x512a70):
//   (flags & 0x3ff5fd00) == 0 -> T2V3   (ARB type 0x11: its own static VBO, glColor per entity)
//   (flags & 0x3ff7fc00) == 0 -> T2N3V3 (ARB type 0x15: + normals, vertex-program lighting)
//   else type 0xb = RB_SurfaceStaticModel 0x514030 (memcpy into tess every frame).
// The smc cache (0x506ae0) gives a surface its lit colours, drawn by RB_SurfaceStaticModelCached 0x514150.
// rb_surfaceTable at 0x5b20f0: entries 11..23 are the static-model functions. This wraps all thirteen with
// counters (surfaces and vertices per entry), a line every 10 s, and logs every distinct shader that reaches
// the CPU path (entry 11) with its flags.
// The ARB VBO paths (T2V3 0x514250, T2N3V3 0x5144e0) draw one surface once per entity of the batch
// ([0x1bc8c3c] entities listed at [0x1bc8c44]): bind VBO/IBO, pointers, then per entity GL_State + vertex program
// parameters (0x4e5610) + glDrawElements(numIndexes [surf+0x10]). cod1x_sp_draws / _dverts count those draws.
// EXPERIMENT cod1x_sp_exp (bench only, the picture is wrong): 1 = every ARB VBO draw keeps its calls and state but
// draws 3 indices (per-draw cost stays, per-vertex cost goes), 2 = ARB VBO surfaces not drawn at all.
#include "performance/smodel_path.h"
#include "core/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
volatile long cod1x_sp_calls[13];
volatile long cod1x_sp_verts[13];
volatile long cod1x_sp_draws;    // ARB VBO paths: glDrawElements calls (surfaces x entities)
volatile long cod1x_sp_dverts;   // ARB VBO paths: vertices of those draws
volatile long long cod1x_sp_ticks;   // ARB VBO paths: QPC ticks inside the draw functions (main thread)
}

namespace patches {
namespace {

constexpr uintptr_t TABLE = 0x005b20f0;       // rb_surfaceTable
constexpr int FIRST = 11, COUNT = 13;         // types 0xb..0x17
constexpr uint32_t MASK_T2V3 = 0x3ff5fd00;
constexpr uint32_t MASK_T2N3V3 = 0x3ff7fc00;
constexpr uintptr_t BATCH_COUNT = 0x01bc8c3c;   // entities drawing the current static surface
constexpr uintptr_t CVAR_GET = 0x0043b880;
typedef void (__cdecl* Surf_t)(void*);
typedef void* (__cdecl* CvarGet_t)(const char*, const char*, int);

const char* const kNames[COUNT] = {"Static(CPU)", "Cached", "Cached", "Cached", "Cached", "T2V3_Gen", "T2V3_ARB",
                                   "ATI", "T2V3_NV", "T2N3V3_Gen", "T2N3V3_ARB", "ATI", "T2N3V3_NV"};
Surf_t g_orig[COUNT];
bool g_installed = false;
long g_rep_c[COUNT], g_rep_v[COUNT];
ULONGLONG g_last_log = 0;
uintptr_t g_seen[512];
int g_logged = 0;
void* g_cv_exp = nullptr;
volatile int g_exp = 0;

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

void probe(const uint8_t* surf) {
    const uintptr_t sh = *(const uintptr_t*)(surf + 8);
    unsigned h = (unsigned)((sh >> 3) * 2654435761u) & 511u;
    for (int k = 0; k < 512; ++k, h = (h + 1) & 511u) {
        if (g_seen[h] == sh) return;
        if (g_seen[h] == 0) { g_seen[h] = sh; break; }
    }
    if (g_logged >= 120 || !readable((const void*)sh, 0x60)) return;
    ++g_logged;
    char name[72] = "?";
    const char* n = (const char*)sh;
    int k = 0;
    for (; k < 64 && n[k] && n[k] >= 32 && n[k] < 127; ++k) name[k] = n[k];
    name[k] = 0;
    const uint32_t fl = *(const uint32_t*)(sh + 0x50);
    logger::logf("smodel_path: CPU-path shader '%s' flags 0x%08x - blocks T2V3 0x%08x, blocks T2N3V3 0x%08x (surface %u verts)",
                 name, fl, fl & MASK_T2V3, fl & MASK_T2N3V3, (unsigned)*(const uint16_t*)(surf + 0x12));
}

template <int I>
void __cdecl wrap(void* surf) {
    uint8_t* s = (uint8_t*)surf;
    InterlockedIncrement(&cod1x_sp_calls[I]);
    InterlockedExchangeAdd(&cod1x_sp_verts[I], *(const uint16_t*)(s + 0x12));
    if (I == 0) probe(s);
    if (I == 6 || I == 10) {   // T2V3_ARB, T2N3V3_ARB
        const int ents = *(const int*)BATCH_COUNT;
        InterlockedExchangeAdd(&cod1x_sp_draws, ents);
        InterlockedExchangeAdd(&cod1x_sp_dverts, ents * (long)*(const uint16_t*)(s + 0x12));
        const int exp = g_exp;
        if (exp == 2) return;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        if (exp == 1) {
            uint16_t* ni = (uint16_t*)(s + 0x10);
            const uint16_t keep = *ni;
            if (keep > 3) *ni = 3;
            g_orig[I](surf);
            *ni = keep;
        } else {
            g_orig[I](surf);
        }
        QueryPerformanceCounter(&t1);
        cod1x_sp_ticks += t1.QuadPart - t0.QuadPart;
        return;
    }
    g_orig[I](surf);
}

template <int... Is> struct Seq {};
template <int N, int... Is> struct Gen : Gen<N - 1, N - 1, Is...> {};
template <int... Is> struct Gen<0, Is...> { using type = Seq<Is...>; };
template <int... Is> void fill(Surf_t* out, Seq<Is...>) { ((out[Is] = &wrap<Is>), ...); }

}  // namespace

void smodel_path_install() {
    if (g_installed) return;
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return;
    uintptr_t* tab = (uintptr_t*)TABLE;
    if (tab[FIRST] != 0x00514030 || tab[FIRST + 1] != 0x00514150 || tab[FIRST + 6] != 0x00514250) {
        logger::logf("smodel_path: rb_surfaceTable at 0x%08x not as expected - not installed", (unsigned)TABLE);
        return;
    }
    Surf_t wraps[COUNT];
    fill(wraps, Gen<COUNT>::type{});
    DWORD old = 0;
    if (!VirtualProtect(tab + FIRST, COUNT * 4, PAGE_READWRITE, &old)) return;
    for (int i = 0; i < COUNT; ++i) {
        g_orig[i] = (Surf_t)tab[FIRST + i];
        tab[FIRST + i] = (uintptr_t)wraps[i];
    }
    VirtualProtect(tab + FIRST, COUNT * 4, old, &old);
    g_installed = true;
    logger::logf("smodel_path: DIAGNOSTIC - static-model backend paths counted (rb_surfaceTable[11..23]), a line every 10 s");
}

// main thread (bench.cpp, each frame): the experiment cvar
void smodel_path_frame() {
    if (!g_installed) return;
    if (!g_cv_exp) g_cv_exp = ((CvarGet_t)CVAR_GET)("cod1x_sp_exp", "0", 0);
    const int v = g_cv_exp ? *(const int*)((const uint8_t*)g_cv_exp + 0x20) : 0;
    if (v != g_exp) {
        g_exp = v;
        logger::logf("smodel_path: EXPERIMENT cod1x_sp_exp %d (%s)", v,
                     v == 1 ? "ARB VBO draws cut to 3 indices" : v == 2 ? "ARB VBO surfaces skipped" : "off");
    }
}

void smodel_path_tick() {
    if (!g_installed) return;
    const ULONGLONG now = GetTickCount64();
    if (g_last_log && now - g_last_log < 10000) return;
    char buf[1024];
    int len = 0;
    long tot = 0;
    for (int i = 0; i < COUNT; ++i) {
        const long c = cod1x_sp_calls[i], v = cod1x_sp_verts[i];
        const long dc = c - g_rep_c[i], dv = v - g_rep_v[i];
        g_rep_c[i] = c; g_rep_v[i] = v;
        tot += dc;
        if (dc > 0 && len < (int)sizeof buf - 64)
            len += snprintf(buf + len, sizeof buf - len, " %s[0x%x] %ld surf / %ld k verts;", kNames[i], FIRST + i, dc, dv / 1000);
    }
    if (g_last_log && tot > 0) logger::logf("smodel_path: last 10 s -%s", buf);
    g_last_log = now;
}

}  // namespace patches
