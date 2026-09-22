// See player_preview.h. Same projection math as the web viewers (hitbox_compare/
// hitreg_compare): fixed 3/4 camera, perspective f = 900/(900 + depth*zoom), flat
// shading from the face normal. Depth handled by painter's sort - the overlay's
// ortho pass has no usable depth buffer and 2908 triangles cost nothing to sort.

#include "ui/player_preview.h"
#include "ui/gl_overlay.h"
#include "ui/player_mesh.h"

#include <windows.h>
#include <GL/gl.h>
#include <cmath>
#include <algorithm>

namespace patches {

namespace {

constexpr float CAM_YAW = 0.62f, CAM_PITCH = 0.30f;
constexpr float MODEL_H = 72.0f;      // head-to-toe, model units (feet at z=0)
constexpr float MODEL_MID = 35.0f;    // vertical centre the camera orbits

// scratch buffers (menu-only path, plain statics are fine)
float s_wx[PM_NV], s_wy[PM_NV], s_wz[PM_NV];    // world (rotated) model space
float s_sx[PM_NV], s_sy[PM_NV], s_dd[PM_NV];    // screen + view depth
int   s_order[PM_NTRI];
float s_tdepth[PM_NTRI];

}  // namespace

void player_preview_draw(float cx, float gy, float h, float st, float t, float dir) {
    // -- pose: lerp two frames of the run cycle -----------------------------
    float ph = fmodf(t * (float)PM_FPS * 0.85f, (float)PM_RUN_FRAMES);
    if (ph < 0) ph += (float)PM_RUN_FRAMES;
    int f0 = (int)ph % PM_RUN_FRAMES;
    int f1 = (f0 + 1) % PM_RUN_FRAMES;
    float ft = ph - (float)(int)ph;
    const short* A = pm_run[f0];
    const short* B = pm_run[f1];

    // face the run direction: model +x forward -> world +-y (same as the site)
    const float rs = dir >= 0 ? 1.0f : -1.0f;     // sin(+-90deg)

    const float cy = cosf(CAM_YAW), sy = sinf(CAM_YAW);
    const float cp = cosf(CAM_PITCH), sp = sinf(CAM_PITCH);
    const float zoom = h / (MODEL_H * 0.95f);
    const float ycentre = gy - MODEL_MID * zoom;

    for (int i = 0; i < PM_NV; ++i) {
        float mx = (A[i * 3]     + (B[i * 3]     - A[i * 3])     * ft) * 0.01f;
        float my = (A[i * 3 + 1] + (B[i * 3 + 1] - A[i * 3 + 1]) * ft) * 0.01f;
        float mz = (A[i * 3 + 2] + (B[i * 3 + 2] - A[i * 3 + 2]) * ft) * 0.01f;
        float wx = -my * rs;                       // rotate +-90deg around z
        float wy =  mx * rs;
        s_wx[i] = wx; s_wy[i] = wy; s_wz[i] = mz;

        float px = wy * cy - wx * sy;              // lateral -> screen x base
        float py = wy * sy + wx * cy;              // view depth component
        float pz = mz - MODEL_MID;
        float zz = pz * cp - py * sp;
        float dd = pz * sp + py * cp;
        float f = 900.0f / (900.0f + dd * zoom);
        s_sx[i] = cx + px * zoom * f * st;         // stretch = screen-space x scale
        s_sy[i] = ycentre - zz * zoom * f;
        s_dd[i] = dd;
    }

    // -- painter order: far triangles first ---------------------------------
    for (int tI = 0; tI < PM_NTRI; ++tI) {
        const unsigned short* q = &pm_tris[tI * 3];
        s_tdepth[tI] = s_dd[q[0]] + s_dd[q[1]] + s_dd[q[2]];
        s_order[tI] = tI;
    }
    std::sort(s_order, s_order + PM_NTRI,
              [](int a, int b) { return s_tdepth[a] > s_tdepth[b]; });

    const GLubyte alpha = (GLubyte)(255.0f * ui_alpha_get());

    // soft ground shadow so the runner sits IN the scene instead of on it
    glDisable(GL_TEXTURE_2D);
    glColor4ub(0, 0, 0, (GLubyte)(alpha * 0.45f));
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, gy);
    for (int i = 0; i <= 24; ++i) {
        float a = 2.0f * 3.14159265f * (float)i / 24.0f;
        glVertex2f(cx + cosf(a) * 15.0f * zoom * st, gy + sinf(a) * 3.5f * zoom);
    }
    glEnd();

    // -- flat-shaded mesh, same light as the site: 0.30 + 0.70*|n.L| --------
    const float Lx = 0.5f, Ly = 0.3f, Lz = 0.81f;  // ~normalised already
    glBegin(GL_TRIANGLES);
    for (int k = 0; k < PM_NTRI; ++k) {
        const unsigned short* q = &pm_tris[s_order[k] * 3];
        int a = q[0], b = q[1], c = q[2];
        float ux = s_wx[b] - s_wx[a], uy = s_wy[b] - s_wy[a], uz = s_wz[b] - s_wz[a];
        float vx = s_wx[c] - s_wx[a], vy = s_wy[c] - s_wy[a], vz = s_wz[c] - s_wz[a];
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        float nl = sqrtf(nx * nx + ny * ny + nz * nz);
        float lit = 0.30f;
        if (nl > 1e-6f) {
            float d = (nx * Lx + ny * Ly + nz * Lz) / nl;
            if (d < 0) d = -d;                     // double-sided, winding-proof
            lit = 0.30f + 0.70f * d;
        }
        GLubyte g = (GLubyte)(222.0f * lit);       // 0.87 white, like the web export
        glColor4ub(g, g, g, alpha);
        glVertex2f(s_sx[a], s_sy[a]);
        glVertex2f(s_sx[b], s_sy[b]);
        glVertex2f(s_sx[c], s_sy[c]);
    }
    glEnd();
}

// ---------------------------------------------------------------- hero (home screen)
void player_hero_draw(float cx, float gy, float h, float yaw, float t, float alpha_mul) {
    // standing pose, breathing: the chest rises ~0.6% and the whole figure sways a
    // hair around its facing - enough to read as alive, not enough to read as motion
    const short* A = pm_stand;
    const float breathe = 1.0f + 0.006f * sinf(t * 1.7f);
    const float sway = 0.02f * sinf(t * 0.9f);
    const float cyw = cosf(yaw + sway), syw = sinf(yaw + sway);
    const float CAM_PITCH_H = 0.14f;
    const float cp = cosf(CAM_PITCH_H), sp = sinf(CAM_PITCH_H);
    const float zoom = h / (MODEL_H * 0.98f);
    const float ycentre = gy - MODEL_MID * zoom;

    for (int i = 0; i < PM_NV; ++i) {
        const float mx = A[i * 3] * 0.01f, my = A[i * 3 + 1] * 0.01f;
        float mz = A[i * 3 + 2] * 0.01f;
        mz = mz > 30.0f ? 30.0f + (mz - 30.0f) * breathe : mz;   // breathe above the hips
        // model x forward: yaw 0 faces the camera
        const float wx = mx * cyw - my * syw;      // depth (towards the camera = -)
        const float wy = mx * syw + my * cyw;      // lateral
        s_wx[i] = wx; s_wy[i] = wy; s_wz[i] = mz;
        const float px = wy, py = -wx;
        const float pz = mz - MODEL_MID;
        const float zz = pz * cp - py * sp;
        const float dd = pz * sp + py * cp;
        const float f = 1400.0f / (1400.0f + dd * zoom);
        s_sx[i] = cx + px * zoom * f;
        s_sy[i] = ycentre - zz * zoom * f;
        s_dd[i] = dd;
    }
    for (int tI = 0; tI < PM_NTRI; ++tI) {
        const unsigned short* q = &pm_tris[tI * 3];
        s_tdepth[tI] = s_dd[q[0]] + s_dd[q[1]] + s_dd[q[2]];
        s_order[tI] = tI;
    }
    std::sort(s_order, s_order + PM_NTRI, [](int a, int b) { return s_tdepth[a] > s_tdepth[b]; });

    const float A0 = ui_alpha_get() * alpha_mul;
    glDisable(GL_TEXTURE_2D);

    // light pool + contact shadow on the ground: the figure stands IN a place
    for (int r = 6; r >= 1; --r) {
        const float k = r / 6.0f;
        glColor4ub(255, 255, 255, (GLubyte)(255 * A0 * 0.035f * (1.0f - k)));
        glBegin(GL_TRIANGLE_FAN);
        glVertex2f(cx, gy);
        for (int i = 0; i <= 40; ++i) {
            const float a = 6.2831853f * i / 40.0f;
            glVertex2f(cx + cosf(a) * 62.0f * zoom * k, gy + sinf(a) * 9.0f * zoom * k);
        }
        glEnd();
    }
    glColor4ub(0, 0, 0, (GLubyte)(255 * A0 * 0.6f));
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, gy);
    for (int i = 0; i <= 32; ++i) {
        const float a = 6.2831853f * i / 32.0f;
        glVertex2f(cx + cosf(a) * 16.0f * zoom, gy + sinf(a) * 3.2f * zoom);
    }
    glEnd();

    // key light high right, fill from the left, rim from behind: normals in the
    // rotated (world) frame, the camera looks along +py = -wx
    const float Kx = -0.45f, Ky = 0.55f, Kz = 0.70f;          // key
    const float Vx = 1.0f,   Vy = 0.0f,  Vz = -sp;              // towards the camera
    glBegin(GL_TRIANGLES);
    for (int k = 0; k < PM_NTRI; ++k) {
        const unsigned short* q = &pm_tris[s_order[k] * 3];
        const int a = q[0], b = q[1], c = q[2];
        const float ux = s_wx[b] - s_wx[a], uy = s_wy[b] - s_wy[a], uz = s_wz[b] - s_wz[a];
        const float vx = s_wx[c] - s_wx[a], vy = s_wy[c] - s_wy[a], vz = s_wz[c] - s_wz[a];
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const float nl = sqrtf(nx * nx + ny * ny + nz * nz);
        float lit = 0.18f, rim = 0.0f;
        if (nl > 1e-6f) {
            nx /= nl; ny /= nl; nz /= nl;
            float d = nx * Kx + ny * Ky + nz * Kz; if (d < 0) d = -d;
            lit = 0.14f + 0.60f * d;
            float f = nx * Vx + ny * Vy + nz * Vz; if (f < 0) f = -f;
            rim = (1.0f - f); rim = rim * rim * rim * 0.70f;        // edge-on faces glow
        }
        float g = 172.0f * lit + 255.0f * rim; if (g > 255) g = 255;   // moody: lit by the rim, not the key
        glColor4ub((GLubyte)g, (GLubyte)g, (GLubyte)g, (GLubyte)(255 * A0));
        glVertex2f(s_sx[a], s_sy[a]);
        glVertex2f(s_sx[b], s_sy[b]);
        glVertex2f(s_sx[c], s_sy[c]);
    }
    glEnd();
}

}  // namespace patches
