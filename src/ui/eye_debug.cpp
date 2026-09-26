// eye_debug.cpp - see eye_debug.h
#include "ui/eye_debug.h"
#include "ui/gl_overlay.h"
#include "core/logger.h"
#include "features/settings_menu.h"   // CODMP_CVAR_FINDVAR_VA
#include "netcode/protocol_patch.h"   // CODMP_CVAR_GET_VA

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace patches {

namespace {

typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
constexpr int CV_INT = 0x20;

// cgame_mp_x86.dll RVAs (relocates: always base + RVA)
// cg.predictedPlayerState: CG_OffsetFirstPersonView reads leanf at 0x20af54 (+0x40) and
// origin[2] at 0x20af30 (+0x1c) - the netfield offsets of playerState_t - so it starts at:
constexpr uintptr_t RVA_PS        = 0x20af14;
constexpr int PS_ORIGIN = 0x14, PS_LEANF = 0x40, PS_VIEWANGLES = 0xc0, PS_VIEWHEIGHT = 0xd0;
// cg.refdef (verified in the 2026-09 hitbox/X-ray work)
constexpr uintptr_t RVA_REF_FOVX  = 0x20d340;
constexpr uintptr_t RVA_REF_FOVY  = 0x20d344;
constexpr uintptr_t RVA_REF_ORG   = 0x20d348;
constexpr uintptr_t RVA_REF_AXIS  = 0x20d354;   // forward, left, up

bool  g_registered = false;
bool  g_active = false;
DWORD g_last_poll = 0, g_last_log = 0;

int cvar_int(const char* n) {
    void* c = ((Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA)(n);
    return c ? *(int*)((char*)c + CV_INT) : 0;
}

uintptr_t cgame() { return (uintptr_t)GetModuleHandleA("cgame_mp_x86.dll"); }

struct Eye {
    float feet[3];        // ps.origin
    float eye[3];         // the camera point
    float vh;             // viewHeightCurrent
    float leanf, frac;    // raw lean, GetLeanFraction
    float lateral;        // eye offset from the feet axis, along the view's right (+ = right)
};

// AddLeanToPosition(pos, yaw, leanf, 16, 20), cgame 0x30040df0 (same as CoD2's):
// frac = (2 - |l|) * l ; pos += right(0, yaw, 16 * frac) * frac * 20
bool read_eye(Eye* e) {
    const uintptr_t cg = cgame();
    if (!cg) return false;
    const char* ps = (const char*)(cg + RVA_PS);
    memcpy(e->feet, ps + PS_ORIGIN, 12);
    e->vh = *(const float*)(ps + PS_VIEWHEIGHT);
    e->leanf = *(const float*)(ps + PS_LEANF);
    const float yaw = *(const float*)(ps + PS_VIEWANGLES + 4);
    if (!(e->vh > 1.0f && e->vh < 100.0f)) return false;     // not spawned / garbage
    e->eye[0] = e->feet[0];
    e->eye[1] = e->feet[1];
    e->eye[2] = e->feet[2] + e->vh;
    e->frac = (2.0f - fabsf(e->leanf)) * e->leanf;
    const float d2r = 3.14159265f / 180.0f;
    const float sy = sinf(yaw * d2r), cy = cosf(yaw * d2r);
    e->lateral = 0;
    if (e->leanf != 0.0f) {
        const float roll = 16.0f * e->frac * d2r;
        const float sr = sinf(roll), cr = cosf(roll);
        const float right[3] = { cr * sy, -cr * cy, -sr };          // AngleVectors, pitch 0
        const float k = e->frac * 20.0f;
        for (int i = 0; i < 3; ++i) e->eye[i] += right[i] * k;
        e->lateral = (e->eye[0] - e->feet[0]) * sy + (e->eye[1] - e->feet[1]) * -cy;
    }
    return true;
}

struct Cam { float fovx, fovy, org[3], ax[9]; };
bool read_cam(Cam* c) {
    const uintptr_t cg = cgame();
    if (!cg) return false;
    c->fovx = *(const float*)(cg + RVA_REF_FOVX);
    c->fovy = *(const float*)(cg + RVA_REF_FOVY);
    memcpy(c->org, (const void*)(cg + RVA_REF_ORG), 12);
    memcpy(c->ax, (const void*)(cg + RVA_REF_AXIS), 36);
    return c->fovx > 1 && c->fovx < 179 && c->fovy > 1 && c->fovy < 179;
}

// same projection as the streamer X-ray (ui/streamer_hud.cpp draw_xray)
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

void seg(const Cam& c, const float a[3], const float b[3], float vw, float vh, float th, DWORD col) {
    float ax, ay, bx, by;
    if (project(c, a, vw, vh, &ax, &ay) && project(c, b, vw, vh, &bx, &by)) ui_line(ax, ay, bx, by, th, col);
}

float dist3(const float a[3], const float b[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

}  // namespace

void eye_debug_frame() {
    const DWORD now = GetTickCount();
    if (now - g_last_poll < 250) return;
    g_last_poll = now;
    if (!g_registered) {
        ((Cvar_Get_t)CODMP_CVAR_GET_VA)("player_debugEyePosition", "0", 0);
        g_registered = true;
    }
    const bool want = cvar_int("player_debugEyePosition") != 0 && cvar_int("sv_running") != 0 && cgame();
    if (want != g_active) {
        g_active = want;
        logger::logf("eye_debug: player_debugEyePosition %s%s", want ? "ON" : "off",
                     want ? " (local server) - cg_thirdPerson 1 shows the body, r_xdebug 1 the hit boxes" : "");
    }
    // first person: the rebuilt eye must BE the engine's camera - logged as the proof
    if (g_active && cvar_int("cg_thirdPerson") == 0 && now - g_last_log >= 2000) {
        Eye e;
        Cam c;
        if (read_eye(&e) && read_cam(&c)) {
            g_last_log = now;
            logger::logf("eye_debug: first person, rebuilt eye vs engine camera: %.2f u apart "
                         "(leanf %+.2f frac %+.2f, eye %.1f u %s of the feet, viewheight %.1f)",
                         dist3(e.eye, c.org), e.leanf, e.frac, fabsf(e.lateral),
                         e.lateral >= 0 ? "right" : "left", e.vh);
        }
    }
}

bool eye_debug_active() { return g_active; }

void eye_debug_draw(float vw, float vh) {
    Eye e;
    Cam c;
    if (!g_active || !read_eye(&e) || !read_cam(&c)) return;
    const DWORD Y = 0xFFFFD21F, Ydim = 0x99FFD21F, G = 0xAAB0B0B0;
    // the body's axis: a vertical from the feet up to eye height (grey)
    const float f0[3] = { e.feet[0], e.feet[1], e.feet[2] };
    const float f1[3] = { e.feet[0], e.feet[1], e.eye[2] };
    seg(c, f0, f1, vw, vh, 1.5f, G);
    // the lean: feet axis -> eye, at eye height (dim yellow)
    seg(c, f1, e.eye, vw, vh, 1.5f, Ydim);
    // the eye's vertical, floor to above the head: what crosses it is past the camera
    const float v0[3] = { e.eye[0], e.eye[1], e.feet[2] };
    const float v1[3] = { e.eye[0], e.eye[1], e.eye[2] + 16.0f };
    seg(c, v0, v1, vw, vh, 1.5f, Ydim);
    // the cross itself, 4 units along each world axis (cod2x: CL_AddDebugCrossPoint(p, 3))
    for (int a = 0; a < 3; ++a) {
        float p0[3] = { e.eye[0], e.eye[1], e.eye[2] }, p1[3] = { e.eye[0], e.eye[1], e.eye[2] };
        p0[a] -= 4.0f;
        p1[a] += 4.0f;
        seg(c, p0, p1, vw, vh, 3.0f, Y);
    }
    // readout
    char l1[160], l2[160];
    const char* stance = e.vh > 50 ? "stand" : e.vh > 25 ? "crouch" : "prone";
    snprintf(l1, sizeof(l1), "EYE  %s   leanf %+.2f   frac %+.2f   eye %.1f u %s of the feet", stance,
             e.leanf, e.frac, fabsf(e.lateral), e.leanf == 0 ? "" : e.lateral >= 0 ? "right" : "left");
    snprintf(l2, sizeof(l2), "yellow = camera point and its vertical - cg_thirdPerson 1, r_xdebug 1 for the hit boxes");
    ui_rect(16, 16, ui_text_font_width(15, 600, l2, UI_FONT_MONO) + 24, 58, 0xB0000000);
    ui_text_font(28, 22, 15, 600, 0xFFFFD21F, l1, UI_FONT_MONO);
    ui_text_font(28, 44, 15, 400, 0xFFD0D0D0, l2, UI_FONT_MONO);
}

}  // namespace patches
