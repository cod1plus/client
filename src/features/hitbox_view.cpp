// hitbox_view.cpp - see hitbox_view.h
#include "features/hitbox_view.h"
#include "features/engine_2d.h"          // EngineSyscall_t, g_syscall_slot
#include "features/settings_menu.h"      // CODMP_CVAR_FINDVAR_VA
#include "netcode/protocol_patch.h"      // CODMP_CVAR_GET_VA
#include "ui/gl_overlay.h"
#include "core/logger.h"
#include "gameplay/hitbox_table.h"     // the boxes the server tests (same table as cod1plus.so)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !__has_include("ui/ui_draw.h")
// main: the overlay has no line primitive yet (it comes with the ui_draw split of the dev
// branch). The boxes are stored but never drawn there; the client-side box table
// (gameplay/hitbox_client) does not depend on any of this.
namespace patches { inline void ui_line(float, float, float, float, float, DWORD) {} }
#endif

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

struct Box { int id; DWORD t; float pos[3], ax[9], mins[3], maxs[3]; };   // pos = WORLD position of the bone
struct Target { int cn; DWORD t; int nparts, got; float org[3]; int n; Box b[32]; };   // boxes merged per bone id
Target g_t[4] = {};
bool   g_installed = false, g_registered = false;
DWORD  g_last_poll = 0;
int    g_cvar = 0;
unsigned g_rx = 0, g_bad = 0, g_calls = 0, g_nosc = 0;
struct EntNote { float pos[3]; float yaw; DWORD t; };
EntNote g_ent[64] = {};
struct SkelNote { float p[4][3]; DWORD t; };
SkelNote g_skel[8] = {};
// the client's own rendered skeleton of every player, world frames per bone (bone_probe)
struct PoseNote { DWORD t; int nbones; bool local; const hb_set_t* set; char model[64]; float f[HB_NBONES][12]; };
PoseNote g_pose[64] = {};

const hb_set_t* set_for_model(const char* name) {
    if (!name) return &HB_UNIVERSAL;
    if (!_strnicmp(name, "xmodel/", 7)) name += 7;
    for (unsigned k = 0; k < HB_NUM_LOOKUP; ++k)
        if (!_stricmp(HB_LOOKUP[k].xmodel, name)) return &HB_SETS[HB_LOOKUP[k].set];
    return &HB_UNIVERSAL;
}
// the box the server tests for bone `bi` of body `set`: the shipped one unless the set fixes it
bool tested_box(const hb_set_t* set, int bi, float mins[3], float maxs[3]) {
    memcpy(mins, HB_SHIPPED[bi], 12); memcpy(maxs, HB_SHIPPED[bi] + 3, 12);
    if (set) for (unsigned k = 0; k < set->nfix; ++k)
        if (set->fix[k].bone == bi) { memcpy(mins, set->fix[k].mins, 12); memcpy(maxs, set->fix[k].maxs, 12); break; }
    return maxs[0] - mins[0] > 0.01f || maxs[1] - mins[1] > 0.01f || maxs[2] - mins[2] > 0.01f;
}
// The server tests the head box on "bip01 head", the bone the head mesh is skinned to - not on
// the controller bone "head" the shipped table names (hitbox_fix.c: the head controller turns
// the box and not the visible head, 13 degrees measured while aim-walking).
int frame_bone(int b) {
    static int head = -2, bip = -2;
    if (head == -2) {
        head = bip = -1;
        for (int k = 0; k < HB_NBONES; ++k) {
            if (!strcmp(HB_BONE_NAMES[k], "head")) head = k;
            if (!strcmp(HB_BONE_NAMES[k], "bip01 head")) bip = k;
        }
    }
    return (b == head && bip >= 0) ? bip : b;
}
DWORD bone_color(const char* n) {
    if (!strcmp(n, "head") || !strcmp(n, "neck")) return 0xFFFF4040;
    if (!strncmp(n, "back_", 5)) return 0xFFFFE040;
    if (strstr(n, "thigh") || strstr(n, "calf") || strstr(n, "foot")) return 0xFF40A0FF;
    if (strstr(n, "finger")) return 0xFF35C7C0;
    return 0xFF40E040;
}
uintptr_t g_cgame_base = 0;

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
    // the engine's syscall entry for cgame: the slot at cgame+0x76898 (engine_2d.h), read
    // here directly - engine_2d only fills its own copy when the Discord hook is on
    EngineSyscall_t sc = g_cgame_base ? *(EngineSyscall_t*)(g_cgame_base + CGAME_SYSCALL_PTR_RVA) : nullptr;
    if (!sc) { g_nosc++; return 0; }
    char buf[1024];
    buf[0] = 0;
    sc(SYSCALL_ARGV, (intptr_t)0, (intptr_t)buf, (intptr_t)sizeof(buf));
    g_calls++;
    if (g_calls <= 12) logger::logf("hitbox_view: server command #%u argv0='%s'", g_calls, buf);
    if (buf[0] != 'h' || buf[1] != 'b' || buf[2] != 0) return 0;
    char a[6][64];
    for (int i = 1; i <= 6; ++i) { a[i - 1][0] = 0; sc(SYSCALL_ARGV, (intptr_t)i, (intptr_t)a[i - 1], (intptr_t)64); }
    const int cn = atoi(a[0]), part = atoi(a[1]), nparts = atoi(a[2]);
    Target* t = slot_for(cn);
    t->nparts = nparts;
    t->org[0] = (float)atof(a[3]); t->org[1] = (float)atof(a[4]); t->org[2] = (float)atof(a[5]);
    // the server sends the body in groups (head/torso, arms, legs), one group per command:
    // merge per bone id, each box stamped and placed in WORLD with this command's origin
    for (int i = 7; i < 40; ++i) {
        buf[0] = 0;
        sc(SYSCALL_ARGV, (intptr_t)i, (intptr_t)buf, (intptr_t)sizeof(buf));
        if (!buf[0]) break;
        Box nb;
        if (!parse_bone(buf, &nb)) { g_bad++; continue; }
        for (int a3 = 0; a3 < 3; ++a3) nb.pos[a3] += t->org[a3];
        nb.t = GetTickCount();
        Box* dst = nullptr;
        for (int k = 0; k < t->n; ++k) if (t->b[k].id == nb.id) { dst = &t->b[k]; break; }
        if (!dst) {
            if (t->n < 32) dst = &t->b[t->n++];
            else { dst = &t->b[0]; for (int k = 1; k < 32; ++k) if (t->b[k].t < dst->t) dst = &t->b[k]; }
        }
        *dst = nb;
    }
    t->got++;
    t->t = GetTickCount();
    g_rx++;
    if (g_rx <= 3)
        logger::logf("hitbox_view: hb cn=%d part %d/%d org %.1f %.1f %.1f -> %d boxes so far", cn, part, nparts,
                     t->org[0], t->org[1], t->org[2], t->n);
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

void hitbox_view_note_skeleton(const float* pelvis, const float* backup, const float* neck, const float* head) {
    // slot by pelvis position (one per model), else the oldest
    SkelNote* o = &g_skel[0];
    for (auto& s : g_skel) {
        if (s.t && fabsf(s.p[0][0] - pelvis[0]) < 3 && fabsf(s.p[0][1] - pelvis[1]) < 3) { o = &s; break; }
        if (s.t < o->t) o = &s;
    }
    memcpy(o->p[0], pelvis, 12); memcpy(o->p[1], backup, 12); memcpy(o->p[2], neck, 12); memcpy(o->p[3], head, 12);
    o->t = GetTickCount();
}

void hitbox_view_note_pose(int cn, const char* model, int nbones, const float* frames, bool local) {
    if (cn < 0 || cn >= 64 || !frames) return;
    PoseNote& p = g_pose[cn];
    p.local = local;
    if (model && strcmp(p.model, model)) { snprintf(p.model, sizeof(p.model), "%s", model); p.set = set_for_model(model); }
    else if (!model && !p.set) p.set = &HB_UNIVERSAL;
    p.nbones = nbones < HB_NBONES ? nbones : HB_NBONES;
    memcpy(p.f, frames, sizeof(float) * 12 * (size_t)p.nbones);
    p.t = GetTickCount();
}

void hitbox_view_note_entity(int cn, const float* pos, float yaw) {
    if (cn < 0 || cn >= 64 || !pos) return;
    memcpy(g_ent[cn].pos, pos, 12); g_ent[cn].yaw = yaw; g_ent[cn].t = GetTickCount();
}

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
    g_cgame_base = base;
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
    // DEVMAP ONLY: the boxes are drawn through walls, so the cvar only counts while the
    // server runs with cheats (sv_cheats 1: devmap, or a demo being played back).
    const int asked = cvar_int("cod1x_drawhitbox");
    const int v = (asked && cvar_int("sv_cheats")) ? asked : 0;
    {
        static int s_refused = 0;
        if (asked && !v && !s_refused) { s_refused = 1; logger::logf("hitbox_view: cod1x_drawhitbox %d ignored - needs a devmap (sv_cheats 1)", asked); }
        if (!asked) s_refused = 0;
    }
    {   // heartbeat while on: what is being received and drawn
        static DWORD s_hb = 0;
        if (v && now - s_hb > 5000) {
            s_hb = now;
            char line[200]; int n = snprintf(line, sizeof(line), "hitbox_view: on, %u server cmds seen (%u without syscall), %u hb rx (%u bad), targets:", g_calls, g_nosc, g_rx, g_bad);
            for (auto& t : g_t) if (t.n) n += snprintf(line + n, sizeof(line) - n, " cn%d:%d boxes %lums ago", t.cn, t.n, (unsigned long)(now - t.t));
            int np = 0; for (auto& p : g_pose) if (p.t && now - p.t < 300) np++;
            n += snprintf(line + n, sizeof(line) - n, " | %d player(s) drawn with client-side boxes", np);
            logger::logf("%s", line);
        }
    }
    if (v != g_cvar) {
        g_cvar = v;
        logger::logf("hitbox_view: cod1x_drawhitbox %d (%u box commands received so far, %u bad)", v, g_rx, g_bad);
    }
}

bool hitbox_view_enabled() { return g_cvar != 0; }

bool hitbox_view_active() {
    if (!g_cvar) return false;
    const DWORD now = GetTickCount();
    for (auto& p : g_pose) if (p.t && now - p.t < 300) return true;
    for (auto& t : g_t) if (t.n && now - t.t < 1200) return true;
    return false;
}

void hitbox_view_draw(float vw, float vh) {
    Cam c;
    if (!g_cvar || !read_cam(&c)) return;
    const DWORD now = GetTickCount();
    // MODE 1: the tested boxes on the body the client draws, every player, every frame
    const bool third = cvar_int("cg_thirdperson") != 0;       // your own body is only on screen then
    for (int cn = 0; cn < 64; ++cn) {
        const PoseNote& p = g_pose[cn];
        if (!p.t || now - p.t > 300) continue;
        if (p.local && !third) continue;
        for (int b = 0; b < p.nbones; ++b) {
            const char* nm = HB_BONE_NAMES[b];
            if (strstr(nm, "finger")) continue;                  // 30 boxes of 1-2 u: clutter
            float mn[3], mx[3];
            if (!tested_box(p.set, b, mn, mx)) continue;
            const float* f = p.f[frame_bone(b)];
            float corner[8][3];
            for (int k = 0; k < 8; ++k) {
                const float lx = (k & 1) ? mx[0] : mn[0], ly = (k & 2) ? mx[1] : mn[1], lz = (k & 4) ? mx[2] : mn[2];
                for (int a = 0; a < 3; ++a) corner[k][a] = f[9 + a] + lx * f[a] + ly * f[3 + a] + lz * f[6 + a];
            }
            static const int E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
            const DWORD col = bone_color(nm);
            for (const auto& e : E) seg(c, corner[e[0]], corner[e[1]], vw, vh, col);
        }
    }
    // MODE 2: the boxes the SERVER streams for the player looked at (hitbox_send), in white
    for (auto& t : g_t) {
        if (g_cvar < 2 || !t.n || now - t.t > 1200) continue;
        for (int i = 0; i < t.n; ++i) {
            const Box& b = t.b[i];
            if (now - b.t > 1200) continue;                       // this bone's group not refreshed lately
            const float* o = b.pos;
            float corner[8][3];
            for (int k = 0; k < 8; ++k) {
                const float lx = (k & 1) ? b.maxs[0] : b.mins[0];
                const float ly = (k & 2) ? b.maxs[1] : b.mins[1];
                const float lz = (k & 4) ? b.maxs[2] : b.mins[2];
                for (int a = 0; a < 3; ++a)
                    corner[k][a] = o[a] + lx * b.ax[a] + ly * b.ax[3 + a] + lz * b.ax[6 + a];
            }
            // ids = HB_BONE_NAMES order (hitbox_data.h): head 39, neck 33, back_up 25,
            // back_mid 19, back_low 7, pelvis 2
            DWORD col = 0xFF40E040;                              // ARGB green (clavicles / arms / hands)
            if (b.id == 39 || b.id == 33) col = 0xFFFF4040;      // head, neck: red
            else if (b.id == 25 || b.id == 19 || b.id == 7) col = 0xFFFFE040;   // spine: yellow
            else if (b.id == 3 || b.id == 4 || b.id == 16 || b.id == 17 || b.id == 22 || b.id == 23) col = 0xFF40A0FF;   // legs: blue
            col = 0xFFFFFFFF;                                    // server-side boxes: white, over the client's
            static const int E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
            for (const auto& e : E) seg(c, corner[e[0]], corner[e[1]], vw, vh, col);
        }
        // the origin the server placed the body at: a small cross on the ground
        const float a1[3] = { t.org[0] - 4, t.org[1], t.org[2] + 0.5f }, a2[3] = { t.org[0] + 4, t.org[1], t.org[2] + 0.5f };
        const float b1[3] = { t.org[0], t.org[1] - 4, t.org[2] + 0.5f }, b2[3] = { t.org[0], t.org[1] + 4, t.org[2] + 0.5f };
        seg(c, a1, a2, vw, vh, 0xFFFFFFFF); seg(c, b1, b2, vw, vh, 0xFFFFFFFF);
    }
    // the client's own es.pos of every player it drew lately: white axis 0..72 u plus a
    // 12 u tick along his view yaw. Server boxes off this axis = server-side placement;
    // drawn model off this axis = client-side rendering
    for (int cn = 0; cn < 64; ++cn) {
        const EntNote& e = g_ent[cn];
        if (!e.t || now - e.t > 400) continue;
        const float top[3] = { e.pos[0], e.pos[1], e.pos[2] + 72.0f };
        const float yr = e.yaw * 3.14159265f / 180.0f;
        const float tip[3] = { e.pos[0] + 12.0f * cosf(yr), e.pos[1] + 12.0f * sinf(yr), e.pos[2] + 0.5f };
        seg(c, e.pos, top, vw, vh, 0xFFFFFFFF);
        seg(c, e.pos, tip, vw, vh, 0xFFFFFFFF);
    }
    // MAGENTA crosses = the client's own rendered skeleton (pelvis, back_up, neck, head)
    for (auto& s : g_skel) {
        if (!s.t || now - s.t > 400) continue;
        for (int k = 0; k < 4; ++k) {
            const float* p = s.p[k];
            const float a1[3] = { p[0] - 2, p[1], p[2] }, a2[3] = { p[0] + 2, p[1], p[2] };
            const float b1[3] = { p[0], p[1] - 2, p[2] }, b2[3] = { p[0], p[1] + 2, p[2] };
            const float c1[3] = { p[0], p[1], p[2] - 2 }, c2[3] = { p[0], p[1], p[2] + 2 };
            seg(c, a1, a2, vw, vh, 0xFFFF40FF); seg(c, b1, b2, vw, vh, 0xFFFF40FF); seg(c, c1, c2, vw, vh, 0xFFFF40FF);
        }
        seg(c, s.p[0], s.p[1], vw, vh, 0xFFFF40FF); seg(c, s.p[1], s.p[2], vw, vh, 0xFFFF40FF); seg(c, s.p[2], s.p[3], vw, vh, 0xFFFF40FF);
    }
}
}  // namespace patches
