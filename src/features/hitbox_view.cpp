// hitbox_view.cpp - see hitbox_view.h
#include "features/hitbox_view.h"
#include "features/engine_2d.h"          // EngineSyscall_t, g_syscall_slot
#include "features/settings_menu.h"      // CODMP_CVAR_FINDVAR_VA
#include "netcode/protocol_patch.h"      // CODMP_CVAR_GET_VA
#include "ui/gl_overlay.h"
#include "core/logger.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace patches {
extern EngineSyscall_t* g_syscall_slot;   // engine_2d.cpp

namespace {
typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
constexpr int CV_INT = 0x20;
constexpr uintptr_t CGAME_SERVERCMD_RVA      = 0x2f660;   // CG_ServerCommand
constexpr uintptr_t CGAME_SERVERCMD_CALL_RVA = 0x2fc96;   // its only call site
constexpr intptr_t  SYSCALL_ARGV             = 0xd;       // (n, buf, size), see 0x3002f674
// cg.refdef (same as ui/eye_debug.cpp)
constexpr uintptr_t RVA_REF_FOVX = 0x20d340, RVA_REF_FOVY = 0x20d344;
constexpr uintptr_t RVA_REF_ORG  = 0x20d348, RVA_REF_AXIS = 0x20d354;

struct Box { int id; float pos[3], ax[9], mins[3], maxs[3]; };
struct Target { int cn; DWORD t; int nparts, got; float org[3]; int n; Box b[24]; };
Target g_t[4] = {};
bool   g_installed = false, g_registered = false;
DWORD  g_last_poll = 0;
int    g_cvar = 0;
unsigned g_rx = 0, g_bad = 0;

int cvar_int(const char* n) {
    void* c = ((Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA)(n);
    return c ? *(int*)((char*)c + CV_INT) : 0;
}

Target* slot_for(int cn) {
    for (auto& t : g_t) if (t.cn == cn) return &t;
    Target* o = &g_t[0];
    for (auto& t : g_t) if (t.t < o->t) o = &t;
    memset(o, 0, sizeof(*o)); o->cn = cn;
    return o;
}

bool parse_bone(const char* tok, Box* b) {
    float v[18];
    int id;
    if (sscanf(tok, "%d,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &id,
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11],
               &v[12], &v[13], &v[14], &v[15], &v[16], &v[17]) != 19) return false;
    b->id = id;
    memcpy(b->pos, v, 12); memcpy(b->ax, v + 3, 36); memcpy(b->mins, v + 12, 12); memcpy(b->maxs, v + 15, 12);
    return true;
}

// argv(0) == "hb" -> consume. Runs on the cgame thread inside CG_ExecuteNewServerCommands.
extern "C" int __cdecl hb_intercept() {
    EngineSyscall_t sc = engine_2d_syscall();
    if (!sc) return 0;
    char buf[1024];
    buf[0] = 0;
    sc(SYSCALL_ARGV, (intptr_t)0, (intptr_t)buf, (intptr_t)sizeof(buf));
    if (buf[0] != 'h' || buf[1] != 'b' || buf[2] != 0) return 0;
    char a[6][64];
    for (int i = 1; i <= 6; ++i) { a[i - 1][0] = 0; sc(SYSCALL_ARGV, (intptr_t)i, (intptr_t)a[i - 1], (intptr_t)64); }
    const int cn = atoi(a[0]), part = atoi(a[1]), nparts = atoi(a[2]);
    Target* t = slot_for(cn);
    if (part == 0 || t->nparts != nparts) { t->n = 0; t->got = 0; }
    t->nparts = nparts;
    t->org[0] = (float)atof(a[3]); t->org[1] = (float)atof(a[4]); t->org[2] = (float)atof(a[5]);
    for (int i = 7; i < 40 && t->n < 24; ++i) {
        buf[0] = 0;
        sc(SYSCALL_ARGV, (intptr_t)i, (intptr_t)buf, (intptr_t)sizeof(buf));
        if (!buf[0]) break;
        if (parse_bone(buf, &t->b[t->n])) t->n++; else g_bad++;
    }
    t->got++;
    t->t = GetTickCount();
    g_rx++;
    return 1;   // consumed
}

void* g_orig_servercmd = nullptr;
// in place of `call CG_ServerCommand` (no args, no result used): ours first, then the
// original unless we consumed the command
extern "C" __attribute__((naked)) void hb_servercmd_wrapper() {
    __asm__ volatile(
        "call _hb_intercept\n\t"
        "testl %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "jmp *%0\n\t"
        "1: ret\n\t"
        : : "m"(g_orig_servercmd));
}

struct Cam { float fovx, fovy, org[3], ax[9]; };
bool read_cam(Cam* c) {
    const uintptr_t cg = (uintptr_t)GetModuleHandleA("cgame_mp_x86.dll");
    if (!cg) return false;
    c->fovx = *(const float*)(cg + RVA_REF_FOVX); c->fovy = *(const float*)(cg + RVA_REF_FOVY);
    memcpy(c->org, (const void*)(cg + RVA_REF_ORG), 12);
    memcpy(c->ax, (const void*)(cg + RVA_REF_AXIS), 36);
    return c->fovx > 1 && c->fovx < 179 && c->fovy > 1 && c->fovy < 179;
}
bool project(const Cam& c, const float p[3], float vw, float vh, float* sx, float* sy) {
    const float d[3] = { p[0] - c.org[0], p[1] - c.org[1], p[2] - c.org[2] };
    const float xf = d[0] * c.ax[0] + d[1] * c.ax[1] + d[2] * c.ax[2];
    const float yl = d[0] * c.ax[3] + d[1] * c.ax[4] + d[2] * c.ax[5];
    const float zu = d[0] * c.ax[6] + d[1] * c.ax[7] + d[2] * c.ax[8];
    if (xf < 1.0f) return false;
    const float tx = tanf(c.fovx * 3.14159265f / 360.0f), ty = tanf(c.fovy * 3.14159265f / 360.0f);
    *sx = vw / 2 - (yl / xf) / tx * (vw / 2);
    *sy = vh / 2 - (zu / xf) / ty * (vh / 2);
    return true;
}
void seg(const Cam& c, const float a[3], const float b[3], float vw, float vh, DWORD col) {
    float ax, ay, bx, by;
    if (project(c, a, vw, vh, &ax, &ay) && project(c, b, vw, vh, &bx, &by)) ui_line(ax, ay, bx, by, 1.5f, col);
}
DWORD color_for(int id) {
    // ABGR. head/neck red, spine yellow, clavicles/arms green, legs blue
    if (id == 38 || id == 37 || id == 24 + 100) return 0xFF4040FF;
    return 0;
}
}  // namespace

bool hitbox_view_install(HMODULE cgame) {
    if (!cgame) return false;
    const uintptr_t base = (uintptr_t)cgame;
    const uintptr_t site = base + CGAME_SERVERCMD_CALL_RVA;
    const uintptr_t target = base + CGAME_SERVERCMD_RVA;
    const uint8_t op = *(const uint8_t*)site;
    if (op != 0xE8) { logger::logf("hitbox_view: call site opcode 0x%02x, expected E8 - not installed", op); return false; }
    const int32_t off = *(const int32_t*)(site + 1);
    const uintptr_t cur = site + 5 + (intptr_t)off;
    if (cur == (uintptr_t)&hb_servercmd_wrapper) return true;      // this cgame load already hooked
    if (cur != target) { logger::logf("hitbox_view: call site targets 0x%x, expected 0x%x - not installed", (unsigned)cur, (unsigned)target); return false; }
    g_orig_servercmd = (void*)target;
    const int32_t rel = (int32_t)((uintptr_t)&hb_servercmd_wrapper - (site + 5));
    DWORD old = 0;
    if (!VirtualProtect((void*)(site + 1), 4, PAGE_READWRITE, &old)) return false;
    *(int32_t*)(site + 1) = rel;
    VirtualProtect((void*)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    g_installed = true;
    logger::logf("hitbox_view: CG_ServerCommand call site hooked (server 'hb' boxes; draw with cod1x_drawhitbox 1)");
    return true;
}

void hitbox_view_frame() {
    const DWORD now = GetTickCount();
    if (now - g_last_poll < 250) return;
    g_last_poll = now;
    if (!g_registered) {
        ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_drawhitbox", "0", 0);
        g_registered = true;
    }
    const int v = cvar_int("cod1x_drawhitbox");
    if (v != g_cvar) {
        g_cvar = v;
        logger::logf("hitbox_view: cod1x_drawhitbox %d (%u box commands received so far, %u bad)", v, g_rx, g_bad);
    }
}

bool hitbox_view_active() {
    if (!g_cvar) return false;
    const DWORD now = GetTickCount();
    for (auto& t : g_t) if (t.n && now - t.t < 600) return true;
    return false;
}

void hitbox_view_draw(float vw, float vh) {
    Cam c;
    if (!g_cvar || !read_cam(&c)) return;
    const DWORD now = GetTickCount();
    for (auto& t : g_t) {
        if (!t.n || now - t.t > 600) continue;
        for (int i = 0; i < t.n; ++i) {
            const Box& b = t.b[i];
            const float o[3] = { t.org[0] + b.pos[0], t.org[1] + b.pos[1], t.org[2] + b.pos[2] };
            float corner[8][3];
            for (int k = 0; k < 8; ++k) {
                const float lx = (k & 1) ? b.maxs[0] : b.mins[0];
                const float ly = (k & 2) ? b.maxs[1] : b.mins[1];
                const float lz = (k & 4) ? b.maxs[2] : b.mins[2];
                for (int a = 0; a < 3; ++a)
                    corner[k][a] = o[a] + lx * b.ax[a] + ly * b.ax[3 + a] + lz * b.ax[6 + a];
            }
            DWORD col = 0xFF40E040;                              // green (limbs / other)
            if (b.id == 38 || b.id == 37) col = 0xFF4040FF;      // head, neck: red
            else if (b.id == 24 || b.id == 18 || b.id == 6) col = 0xFF40E0FF;   // spine: yellow
            static const int E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
            for (const auto& e : E) seg(c, corner[e[0]], corner[e[1]], vw, vh, col);
        }
        // the origin the server placed the body at: a small cross on the ground
        const float a1[3] = { t.org[0] - 4, t.org[1], t.org[2] + 0.5f }, a2[3] = { t.org[0] + 4, t.org[1], t.org[2] + 0.5f };
        const float b1[3] = { t.org[0], t.org[1] - 4, t.org[2] + 0.5f }, b2[3] = { t.org[0], t.org[1] + 4, t.org[2] + 0.5f };
        seg(c, a1, a2, vw, vh, 0xFFFFFFFF); seg(c, b1, b2, vw, vh, 0xFFFFFFFF);
    }
}
}  // namespace patches
