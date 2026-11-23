// smodel_lod.cpp - see smodel_lod.h
#include "performance/smodel_lod.h"
#include "core/logger.h"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

extern "C" {
volatile long cod1x_lod_detail_calls = 0;   // detail-cluster distance requests
volatile long cod1x_lod_other_calls = 0;    // everything else (engine value)
}

namespace patches {
namespace {

constexpr uintptr_t VA_CALLSITE = 0x00513d1f;   // E8 rel32 -> 0x4e0f10, esi = the static model
constexpr uintptr_t VA_LODDIST  = 0x004e0f10;
constexpr uintptr_t VIEWORG     = 0x0199d530;   // tr.viewParms origin (float[3])
constexpr uintptr_t LOD_FACTOR  = 0x0199d674;   // r_lodscale-derived factor
constexpr uintptr_t LOD_BIAS    = 0x0199d670;
constexpr uintptr_t CVAR_GET    = 0x0043b880;

typedef void* (__cdecl* CvarGet_t)(const char*, const char*, int);

bool g_installed = false;
void* g_cv_scale = nullptr;
float g_scale = 1.0f;
long g_rep[2] = {0, 0};
ULONGLONG g_last_log = 0;

// per XModel pointer: 1 = detail cluster, 2 = other (cache of the name test; models live for the whole map)
constexpr int CACHE = 1024;
uintptr_t g_cache_key[CACHE];
uint8_t g_cache_val[CACHE];

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

bool is_detail(uintptr_t xmodel) {
    const unsigned h = (unsigned)((xmodel >> 4) * 2654435761u) % CACHE;
    for (int k = 0; k < 8; ++k) {
        const unsigned i = (h + k) % CACHE;
        if (g_cache_key[i] == xmodel) return g_cache_val[i] == 1;
        if (g_cache_key[i] == 0) {
            bool det = false;
            if (readable((const void*)xmodel, 4)) {
                const char* name = *(const char* const*)xmodel;
                if (readable(name, 1)) {
                    // "<map>_dt" + digit, anywhere in the (short) name
                    for (int j = 0; j < 60 && name[j] && readable(name + j, 4); ++j) {
                        if (name[j] == '_' && name[j + 1] == 'd' && name[j + 2] == 't' && name[j + 3] >= '0' && name[j + 3] <= '9') {
                            det = true;
                            break;
                        }
                    }
                }
            }
            g_cache_key[i] = xmodel;
            g_cache_val[i] = det ? 1 : 2;
            return det;
        }
    }
    return false;   // cache full around this slot: engine behaviour
}

extern "C" float __cdecl cod1x_lod_dist(const uint8_t* sm) {
    const float* vo = (const float*)VIEWORG;
    const float* o = (const float*)(sm + 0x44);
    const float dx = vo[0] - o[0], dy = vo[1] - o[1], dz = vo[2] - o[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    float factor = *(const float*)LOD_FACTOR;
    const float bias = *(const float*)LOD_BIAS;
    if (factor == 0.0f) {
        const uintptr_t info = *(const uintptr_t*)(sm + 0x98);
        const uintptr_t xm = info ? *(const uintptr_t*)(info + 8) : 0;   // the engine's own pointers (0x513d19/0x513d25): no check per call
        if (xm && is_detail(xm)) {
            factor = g_scale;
            InterlockedIncrement(&cod1x_lod_detail_calls);
        } else {
            InterlockedIncrement(&cod1x_lod_other_calls);
        }
    } else {
        InterlockedIncrement(&cod1x_lod_other_calls);
    }
    d = d * factor + bias;
    const float scale = *(const float*)(sm + 0x40);
    if (scale != 0.0f) d /= scale;
    return d;
}

// entered like 0x4e0f10: esi = the static model, the float result in st(0); every register but eax/ecx/edx kept
__attribute__((naked)) void lod_dist_hook() {
    __asm__ volatile(
        "pushl %%ecx\n\t"
        "pushl %%edx\n\t"
        "pushl %%esi\n\t"
        "call _cod1x_lod_dist\n\t"
        "addl $4, %%esp\n\t"
        "popl %%edx\n\t"
        "popl %%ecx\n\t"
        "ret\n\t"
        : : : "memory");
}

}  // namespace

void smodel_lod_install() {
    if (g_installed) return;
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return;
    uint8_t* p = (uint8_t*)VA_CALLSITE;
    if (p[0] != 0xE8) return;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    if ((uintptr_t)(VA_CALLSITE + 5 + rel) != VA_LODDIST) {
        logger::logf("smodel_lod: the call at 0x%08x is not `call 0x%08x` - not installed", (unsigned)VA_CALLSITE, (unsigned)VA_LODDIST);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    rel = (int32_t)((uintptr_t)&lod_dist_hook - (VA_CALLSITE + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    g_installed = true;
    logger::logf("smodel_lod: detail clusters (<map>_dtNNN) culled at their LOD distance even with r_lodscale 0 (cod1x_detail_scale %.2f); "
                 "every other model unchanged", g_scale);
}

void smodel_lod_tick() {
    if (!g_installed) return;
    const ULONGLONG now = GetTickCount64();
    if (g_last_log && now - g_last_log < 10000) return;
    const long cur[2] = {cod1x_lod_detail_calls, cod1x_lod_other_calls};
    const long dd = cur[0] - g_rep[0], dother = cur[1] - g_rep[1];
    g_rep[0] = cur[0]; g_rep[1] = cur[1];
    if (g_last_log && dd > 0) {
        logger::logf("smodel_lod: last 10 s - %ld detail-cluster distance tests (plain distance x %.2f), %ld other static models (engine value)",
                     dd, g_scale, dother);
    }
    g_last_log = now;
}

// the scale cvar is read on the main thread (bench.cpp calls this each frame)
void smodel_lod_frame() {
    if (!g_installed) return;
    if (!g_cv_scale) g_cv_scale = ((CvarGet_t)CVAR_GET)("cod1x_detail_scale", "1", 0x01);
    if (g_cv_scale) {
        const char* s = *(const char* const*)((const uint8_t*)g_cv_scale + 4);   // cvar_t.string
        (void)s;
        const float v = *(const float*)((const uint8_t*)g_cv_scale + 0x1c);       // cvar_t.value
        g_scale = (v > 0.0f && v < 100.0f) ? v : 1.0f;
    }
}

}  // namespace patches
