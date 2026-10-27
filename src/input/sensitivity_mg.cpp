// sensitivity_mg.cpp - see sensitivity_mg.h. Port of cod2x's sensitivity_MG (src/mss32/mouse.cpp:
// CoD2 1.3's CL_MouseMove has the same branch, eFlags 0x300 there).
//
// CoDMP.exe 1.5, CL_MouseMove 0x40bd60 (after its push esi: [esp+4] = mx, [esp+8] = my,
// [esp+0xc] = (sensitivity + rate * cl_mouseAccel) * cl.cgameSensitivity, [esp+0x10] = rate =
// mouse speed / frame msec; cl_mouseAccel = cvar_t* [0x189990c], value +0x1c):
//   0x40bec2  mov eax,[0x148bc40]; test ah,0xc0; je 0x40bee2 - cl.snap.ps.eFlags & 0xc000 = on a
//             turret (game.mp turret_use 0x53180 sets 0xc000, or 0x8000)
//   0x40becc  mx *= 2.5 (double 0x5a87e8); my += my; jmp 0x40bef6 - sensitivity left out
//   0x40bee2  mx *= accel sens; my *= accel sens (the normal path)
//   0x40bef6  fstp [esp+8] = my, then the yaw / pitch turn (m_yaw, m_pitch, turn-rate limits)
// THE FIX = 0x40becc jumps to a stub: f = rate * cl_mouseAccel + sensitivity_mg, mx *= f * 2.5,
// my *= f * 2 (cod2x's formula), back at 0x40bef6 with my in st(0) as the game leaves it.
// sensitivity_mg 1 with cl_mouseAccel 0 = the game's turret speed exactly; cl.cgameSensitivity
// stays out of it, as in the game.

#include "input/sensitivity_mg.h"
#include "core/logger.h"
#include "netcode/protocol_patch.h"      // CODMP_CVAR_GET_VA, CVAR_ARCHIVE

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace patches {

namespace {

constexpr uintptr_t VA_MG_TEST   = 0x0040bec2;
constexpr uintptr_t VA_MG_BRANCH = 0x0040becc;
constexpr uintptr_t VA_MG_BACK   = 0x0040bef6;
constexpr uintptr_t VA_ACCEL_LD  = 0x0040be35;   // mov ecx,[cl_mouseAccel]
constexpr uintptr_t VA_RATE_ST   = 0x0040be45;   // fstp [esp+0x10] = rate
constexpr uintptr_t VA_EFLAGS    = 0x0148bc40;   // cl.snap.ps.eFlags
constexpr uint32_t  EF_TURRET    = 0xc000;
const uint8_t MG_TEST[10]   = { 0xa1, 0x40, 0xbc, 0x48, 0x01, 0xf6, 0xc4, 0xc0, 0x74, 0x16 };
const uint8_t MG_BRANCH[22] = { 0xd9, 0x44, 0x24, 0x04,                     // fld [esp+4]
                                0xdc, 0x0d, 0xe8, 0x87, 0x5a, 0x00,         // fmul qword [0x5a87e8]
                                0xd9, 0x5c, 0x24, 0x04,                     // fstp [esp+4]
                                0xd9, 0x44, 0x24, 0x08,                     // fld [esp+8]
                                0xdc, 0xc0,                                 // fadd st0,st0
                                0xeb, 0x14 };                               // jmp 0x40bef6
const uint8_t ACCEL_LD[6] = { 0x8b, 0x0d, 0x0c, 0x99, 0x89, 0x01 };
const uint8_t RATE_ST[4]  = { 0xd9, 0x5c, 0x24, 0x10 };

constexpr int CV_VALUE = 0x1c;
typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);

volatile float g_sens = 1.0f;         // sensitivity_mg, clamped
uintptr_t g_back = VA_MG_BACK;
bool g_installed = false;
void* g_cv = nullptr;

// In place of the turret branch (jumped to, same stack; the x87 stack is empty here).
__attribute__((naked)) void stub() {
    __asm__ volatile(
        "flds 0x10(%%esp)\n\t"             /* rate */
        "movl 0x0189990c, %%eax\n\t"       /* cl_mouseAccel */
        "fmuls 0x1c(%%eax)\n\t"
        "fadds %0\n\t"                     /* f = rate * cl_mouseAccel + sensitivity_mg */
        "fld %%st(0)\n\t"
        "fmuls 4(%%esp)\n\t"
        "fmull 0x005a87e8\n\t"             /* the game's 2.5 */
        "fstps 4(%%esp)\n\t"               /* mx *= f * 2.5 */
        "fmuls 8(%%esp)\n\t"
        "fadd %%st(0), %%st\n\t"           /* st(0) = my * f * 2 */
        "jmp *%1\n\t"
        : : "m"(g_sens), "m"(g_back));
}

}  // namespace

bool sensitivity_mg_install() {
    if (memcmp((const void*)VA_MG_TEST, MG_TEST, sizeof(MG_TEST)) != 0 ||
        memcmp((const void*)VA_MG_BRANCH, MG_BRANCH, sizeof(MG_BRANCH)) != 0 ||
        memcmp((const void*)VA_ACCEL_LD, ACCEL_LD, sizeof(ACCEL_LD)) != 0 ||
        memcmp((const void*)VA_RATE_ST, RATE_ST, sizeof(RATE_ST)) != 0) {
        logger::logf("sensitivity_mg: CL_MouseMove's turret branch (0x%x) is not the 1.5 one - not installed", (unsigned)VA_MG_BRANCH);
        return false;
    }
    uint8_t* p = (uint8_t*)VA_MG_BRANCH;
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(MG_BRANCH), PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = 0xE9;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)&stub - (VA_MG_BRANCH + 5));
    memset(p + 5, 0x90, sizeof(MG_BRANCH) - 5);
    VirtualProtect(p, sizeof(MG_BRANCH), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(MG_BRANCH));
    g_installed = true;
    logger::logf("sensitivity_mg: mounted MG turned by sensitivity_mg (CL_MouseMove 0x%x; 1 = the game's speed)", (unsigned)VA_MG_BRANCH);
    return true;
}

void sensitivity_mg_tick() {
    if (!g_installed) return;
    if (!g_cv) {
        g_cv = ((Cvar_Get_t)CODMP_CVAR_GET_VA)("sensitivity_mg", "1", CVAR_ARCHIVE);
        if (!g_cv) return;
    }
    float v = *(const float*)((const uint8_t*)g_cv + CV_VALUE);
    if (!(v >= 0.01f)) v = 0.01f;            // NaN too
    if (v > 100.0f) v = 100.0f;
    if (v != g_sens) {
        g_sens = v;
        logger::logf("sensitivity_mg: %.3f", v);
    }
}

bool sensitivity_mg_active(float* kx, float* ky) {
    if (!(*(const volatile uint32_t*)VA_EFLAGS & EF_TURRET)) return false;
    const float s = g_installed ? g_sens : 1.0f;
    *kx = s * 2.5f;
    *ky = s * 2.0f;
    return true;
}

}  // namespace patches
