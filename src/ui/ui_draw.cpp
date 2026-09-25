// ui_draw.cpp - see ui_draw.h. Mechanics:
//
//   Text is rasterised by GDI into DIBs (white on black, ANTIALIASED_QUALITY),
//   converted to GL_ALPHA textures and cached per (string, size, weight, face). That
//   gives real Segoe UI with proper antialiasing for the cost of one texture per
//   distinct string - a settings menu has a few dozen.
//
//   Anti-aliased shapes (ui_aa_*) are plain geometry with a feathered rim: the shape is
//   filled inset by half a pixel and a one-pixel strip fades to transparent outside it.
//   Exact at any size, no texture, and GL 1.x immediate mode is all it needs - the engine
//   context offers no multisampling. The same rim, wide, is the soft drop shadow.
//
//   GL state: glPushAttrib(ALL)+glPushClientAttrib(ALL) around the pass. The
//   engine is fixed-function GL1.x, so immediate mode is both safe and simplest.

#include "ui/ui_draw.h"
#include "ui/gl_overlay.h"

#include <GL/gl.h>
#include <gdiplus.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191
#endif

namespace patches {

namespace {

int     g_vw = 0, g_vh = 0;         // viewport size of this pass
float   g_clip_xs = 1.0f;           // ui_clip_xscale: drawing x -> window x
float   g_alpha_mul = 1.0f;
float   g_dt = 0.016f;
std::map<long, float> g_anim;
HGLRC   g_glrc = NULL;              // vid_restart detector: context change = dead textures

// ---------------------------------------------------------------- text cache
struct TextTex { GLuint id = 0; int w = 0, h = 0; };
std::map<std::string, TextTex> g_text_cache;

const wchar_t* font_face(int font) {
    switch (font) {
    case UI_FONT_MONO: return L"Consolas";
    default:           return L"Segoe UI";
    }
}

TextTex make_text(const char* utf8, int px, int weight, int font) {
    TextTex out;
    wchar_t wide[512];
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 511);
    if (wn <= 0) return out;
    wide[511] = 0;

    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return out;
    HFONT hf = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           font_face(font));
    HGDIOBJ of = SelectObject(dc, hf);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, wide, wn - 1, &sz);
    int w = sz.cx + 2, h = sz.cy + 2;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;                    // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits) {
        HGDIOBJ ob = SelectObject(dc, bmp);
        RECT rc = { 0, 0, w, h };
        SetBkColor(dc, RGB(0, 0, 0));
        SetTextColor(dc, RGB(255, 255, 255));
        ExtTextOutW(dc, 1, 1, ETO_OPAQUE, &rc, wide, wn - 1, NULL);
        GdiFlush();

        // luminance of white-on-black == coverage == alpha
        unsigned char* a = (unsigned char*)malloc((size_t)w * h);
        const DWORD* p = (const DWORD*)bits;
        for (int i = 0; i < w * h; ++i) a[i] = (unsigned char)(p[i] & 0xff);

        glGenTextures(1, &out.id);
        glBindTexture(GL_TEXTURE_2D, out.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0,
                     GL_ALPHA, GL_UNSIGNED_BYTE, a);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        free(a);
        out.w = w; out.h = h;
        SelectObject(dc, ob);
    }
    if (bmp) DeleteObject(bmp);
    SelectObject(dc, of);
    DeleteObject(hf);
    DeleteDC(dc);
    return out;
}

ULONG_PTR g_gdiplus_token = 0;
bool      g_gdiplus_up    = false;

void ensure_gdiplus() {
    if (g_gdiplus_up) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_gdiplus_token, &in, nullptr) == Gdiplus::Ok)
        g_gdiplus_up = true;
}

// GDI+ locks 32bpp bitmaps as BGRA in memory (little-endian); GL wants RGBA - swap
// the R/B bytes per pixel once at load time rather than fighting GL_BGRA support
// across whatever ancient GL1.x driver this idTech3 build ends up on.
TextTex upload_bitmap(Gdiplus::Bitmap& bmp);

TextTex load_image(const char* path) {
    TextTex out;
    ensure_gdiplus();
    if (!g_gdiplus_up) return out;

    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    Gdiplus::Bitmap bmp(wpath);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return out;
    return upload_bitmap(bmp);
}

// Same, from bytes in memory (a JPG read out of a pk3 by the engine's filesystem).
TextTex load_image_mem(const void* data, int len) {
    TextTex out;
    ensure_gdiplus();
    if (!g_gdiplus_up || !data || len <= 0) return out;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)len);
    if (!mem) return out;
    void* p = GlobalLock(mem);
    memcpy(p, data, (size_t)len);
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(mem, TRUE, &stream) != S_OK) { GlobalFree(mem); return out; }
    {
        Gdiplus::Bitmap bmp(stream);
        if (bmp.GetLastStatus() == Gdiplus::Ok)
            out = upload_bitmap(bmp);
    }
    stream->Release();                       // frees `mem` (fDeleteOnRelease)
    return out;
}

TextTex upload_bitmap(Gdiplus::Bitmap& bmp) {
    TextTex out;
    int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return out;

    Gdiplus::BitmapData bd;
    Gdiplus::Rect rc(0, 0, w, h);
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return out;

    unsigned char* px = (unsigned char*)malloc((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = (const unsigned char*)bd.Scan0 + (size_t)y * bd.Stride;
        unsigned char* dst = px + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];   // B -> R
            dst[x * 4 + 1] = src[x * 4 + 1];   // G
            dst[x * 4 + 2] = src[x * 4 + 0];   // R -> B
            dst[x * 4 + 3] = src[x * 4 + 3];   // A
        }
    }
    bmp.UnlockBits(&bd);

    glGenTextures(1, &out.id);
    glBindTexture(GL_TEXTURE_2D, out.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(px);
    out.w = w; out.h = h;
    return out;
}

const TextTex& get_text(const char* utf8, int px, int weight, int font = UI_FONT_SANS) {
    char key[560];
    snprintf(key, sizeof(key), "%d|%d|%d|%s", px, weight, font, utf8);
    auto it = g_text_cache.find(key);
    if (it != g_text_cache.end()) return it->second;
    return g_text_cache.emplace(key, make_text(utf8, px, weight, font)).first->second;
}

inline void set_color(DWORD c) {
    float a = ((c >> 24) & 0xff) * g_alpha_mul;
    glColor4ub((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff,
               (GLubyte)(a > 255 ? 255 : a));
}

// same, alpha scaled by `k` on top of the global multiplier (rims fade to k = 0)
inline void set_color_k(DWORD c, float k) {
    float a = ((c >> 24) & 0xff) * g_alpha_mul * k;
    glColor4ub((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff,
               (GLubyte)(a > 255 ? 255 : a < 0 ? 0 : a));
}

// ------------------------------------------------------------------ drawing
void arc_fan(float cx, float cy, float r, float a0, float a1) {
    for (int i = 0; i <= 6; ++i) {
        float a = a0 + (a1 - a0) * i / 6.0f;
        glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
    }
}

void draw_tex(const TextTex& t, float x, float y, float w, float h,
              float u0, float v0, float u1, float v1, DWORD rgba) {
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    set_color(rgba);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x, y);
    glTexCoord2f(u1, v0); glVertex2f(x + w, y);
    glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
    glTexCoord2f(u0, v1); glVertex2f(x, y + h);
    glEnd();
}

// ------------------------------------------------------- anti-aliased geometry
struct V2 { float x, y; };

// colour of a vertex: flat, or a vertical gradient between y0 (c0) and y1 (c1)
struct Paint {
    DWORD c0, c1;
    float y0, y1;
    DWORD at(float y) const {
        if (c0 == c1 || y1 <= y0) return c0;
        float t = (y - y0) / (y1 - y0);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        DWORD o = 0;
        for (int s = 0; s < 32; s += 8) {
            const float a = (float)((c0 >> s) & 0xff), b = (float)((c1 >> s) & 0xff);
            o |= (DWORD)(a + (b - a) * t + 0.5f) << s;
        }
        return o;
    }
};

// Offsets every vertex of a closed convex path along its outward miter by `d` pixels
// (negative = inward). Works for either winding.
void offset_path(const V2* p, int n, float d, V2* out) {
    float area = 0;
    for (int i = 0; i < n; ++i) {
        const V2& a = p[i]; const V2& b = p[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    const float s = area > 0 ? 1.0f : -1.0f;       // y down: positive area = clockwise on screen
    for (int i = 0; i < n; ++i) {
        const V2& a = p[(i + n - 1) % n]; const V2& b = p[i]; const V2& c = p[(i + 1) % n];
        float e0x = b.x - a.x, e0y = b.y - a.y, e1x = c.x - b.x, e1y = c.y - b.y;
        float l0 = sqrtf(e0x * e0x + e0y * e0y), l1 = sqrtf(e1x * e1x + e1y * e1y);
        if (l0 < 1e-4f) l0 = 1e-4f;
        if (l1 < 1e-4f) l1 = 1e-4f;
        // outward normal of an edge: (dy, -dx) for clockwise-on-screen paths
        const float n0x = s * e0y / l0, n0y = -s * e0x / l0;
        const float n1x = s * e1y / l1, n1y = -s * e1x / l1;
        float mx = n0x + n1x, my = n0y + n1y;
        float ml = sqrtf(mx * mx + my * my);
        if (ml < 1e-4f) { mx = n1x; my = n1y; ml = 1; }
        mx /= ml; my /= ml;
        float k = mx * n1x + my * n1y;                 // cos of the half angle
        if (k < 0.25f) k = 0.25f;                      // miter limit
        out[i].x = b.x + mx * d / k;
        out[i].y = b.y + my * d / k;
    }
}

// Fills a convex path with a feathered rim of `feather` pixels centred on its outline.
void aa_fill(const V2* p, int n, const Paint& paint, float feather = 1.0f) {
    if (n < 3) return;
    std::vector<V2> in((size_t)n), out((size_t)n);
    offset_path(p, n, -feather * 0.5f, in.data());
    offset_path(p, n, feather * 0.5f, out.data());
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_TRIANGLE_FAN);
    for (int i = 0; i < n; ++i) { set_color(paint.at(in[i].y)); glVertex2f(in[i].x, in[i].y); }
    glEnd();
    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= n; ++i) {
        const int k = i % n;
        set_color(paint.at(in[k].y));         glVertex2f(in[k].x, in[k].y);
        set_color_k(paint.at(out[k].y), 0);   glVertex2f(out[k].x, out[k].y);
    }
    glEnd();
}

// Strokes a closed convex path: a band of `th` pixels centred on it, feathered both sides.
void aa_stroke(const V2* p, int n, float th, DWORD c) {
    if (n < 2) return;
    float k = 1.0f;
    if (th < 1.0f) { k = th; th = 1.0f; }             // hairlines: fade instead of thinning
    std::vector<V2> r0((size_t)n), r1((size_t)n), r2((size_t)n), r3((size_t)n);
    offset_path(p, n, -th * 0.5f - 0.5f, r0.data());
    offset_path(p, n, -th * 0.5f + 0.5f, r1.data());
    offset_path(p, n, th * 0.5f - 0.5f, r2.data());
    offset_path(p, n, th * 0.5f + 0.5f, r3.data());
    glDisable(GL_TEXTURE_2D);
    const std::vector<V2>* ring[4] = { &r0, &r1, &r2, &r3 };
    const float alpha[4] = { 0, k, k, 0 };
    for (int band = 0; band < 3; ++band) {
        glBegin(GL_TRIANGLE_STRIP);
        for (int i = 0; i <= n; ++i) {
            const int j = i % n;
            set_color_k(c, alpha[band]);     glVertex2f((*ring[band])[j].x, (*ring[band])[j].y);
            set_color_k(c, alpha[band + 1]); glVertex2f((*ring[band + 1])[j].x, (*ring[band + 1])[j].y);
        }
        glEnd();
    }
}

int arc_steps(float r) {
    int n = (int)(r * 0.6f) + 3;
    return n > 16 ? 16 : n;
}

// Outline of a rounded rectangle, clockwise on screen from the top-left corner.
std::vector<V2> round_rect_path(float x, float y, float w, float h, float r) {
    std::vector<V2> p;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0.5f) {
        p = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } };
        return p;
    }
    const float PI = 3.14159265f;
    const int n = arc_steps(r);
    const float cx[4] = { x + r, x + w - r, x + w - r, x + r };
    const float cy[4] = { y + r, y + r, y + h - r, y + h - r };
    for (int c = 0; c < 4; ++c) {
        const float a0 = PI + c * PI / 2;
        for (int i = 0; i <= n; ++i) {
            const float a = a0 + (PI / 2) * i / n;
            p.push_back({ cx[c] + cosf(a) * r, cy[c] + sinf(a) * r });
        }
    }
    return p;
}

std::vector<V2> circle_path(float cx, float cy, float r) {
    int n = (int)(r * 1.5f) + 12;
    if (n > 96) n = 96;
    std::vector<V2> p((size_t)n);
    for (int i = 0; i < n; ++i) {
        const float a = 6.2831853f * i / n;
        p[i] = { cx + cosf(a) * r, cy + sinf(a) * r };
    }
    return p;
}

// ------------------------------------------------------------ raw textures
std::map<std::string, TextTex> g_tex;         // ui_tex_*: pixels handed over by the caller

}  // namespace

void ui_set_color(DWORD rgba) { set_color(rgba); }
void ui_frame_dt(float dt) { g_dt = dt; }

void ui_begin_2d(int vw, int vh) {
    g_vw = vw; g_vh = vh;
    // vid_restart destroys the GL context; every cached texture id then belongs to
    // a dead context and draws as garbage squares (seen live 2026-08-10: the whole
    // menu turned into black-and-white blocks). Forget the cache and re-rasterise
    // lazily in the new context.
    HGLRC rc = wglGetCurrentContext();
    if (rc != g_glrc) { g_glrc = rc; g_text_cache.clear(); g_tex.clear(); }
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, g_vw, g_vh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glViewport(0, 0, g_vw, g_vh);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);   glDisable(GL_SCISSOR_TEST);
    glDisable(GL_ALPHA_TEST); glDisable(GL_FOG);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void ui_end_2d() {
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
}

void ui_rect(float x, float y, float w, float h, DWORD rgba) {
    glDisable(GL_TEXTURE_2D);
    set_color(rgba);
    glBegin(GL_QUADS);
    glVertex2f(x, y); glVertex2f(x + w, y);
    glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();
}

void ui_rect_rounded(float x, float y, float w, float h, float r, DWORD rgba) {
    if (r <= 0.5f) { ui_rect(x, y, w, h, rgba); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    glDisable(GL_TEXTURE_2D);
    set_color(rgba);
    const float PI = 3.14159265f;
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(x + w / 2, y + h / 2);
    arc_fan(x + r,     y + r,     r, PI,          PI * 1.5f);   // top-left
    arc_fan(x + w - r, y + r,     r, PI * 1.5f,   PI * 2.0f);   // top-right
    arc_fan(x + w - r, y + h - r, r, 0,           PI * 0.5f);   // bottom-right
    arc_fan(x + r,     y + h - r, r, PI * 0.5f,   PI);          // bottom-left
    glVertex2f(x, y + r);                                       // close the fan
    glEnd();
}

void ui_rect_border(float x, float y, float w, float h, float r, float th, DWORD rgba) {
    // two rounded rects would need stenciling; a line loop is enough at 1-2px
    (void)r;
    glDisable(GL_TEXTURE_2D);
    set_color(rgba);
    glLineWidth(th);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y); glVertex2f(x + w, y);
    glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();
    glLineWidth(1.0f);
}

bool ui_image_known(const char* key) {
    char k[560];
    snprintf(k, sizeof(k), "mem:%s", key);
    return g_text_cache.find(k) != g_text_cache.end();
}

bool ui_image_load_mem(const char* key, const void* data, int len) {
    char k[560];
    snprintf(k, sizeof(k), "mem:%s", key);
    auto it = g_text_cache.find(k);
    if (it == g_text_cache.end())
        it = g_text_cache.emplace(k, load_image_mem(data, len)).first;   // a failure is cached too
    return it->second.id != 0;
}

bool ui_image_draw(float x, float y, float w, float h, const char* key, DWORD rgba) {
    char k[560];
    snprintf(k, sizeof(k), "mem:%s", key);
    auto it = g_text_cache.find(k);
    if (it == g_text_cache.end() || !it->second.id) return false;
    draw_tex(it->second, x, y, w, h, 0, 0, 1, 1, rgba);
    return true;
}

void ui_triangle(float x1, float y1, float x2, float y2, float x3, float y3, DWORD rgba) {
    glDisable(GL_TEXTURE_2D);
    set_color(rgba);
    glBegin(GL_TRIANGLES);
    glVertex2f(x1, y1); glVertex2f(x2, y2); glVertex2f(x3, y3);
    glEnd();
}

void ui_line(float x1, float y1, float x2, float y2, float th, DWORD rgba) {
    // a quad along the segment: GL line widths above 1 are not guaranteed
    const float dx = x2 - x1, dy = y2 - y1;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    const float nx = -dy / len * th * 0.5f, ny = dx / len * th * 0.5f;
    glDisable(GL_TEXTURE_2D);
    set_color(rgba);
    glBegin(GL_QUADS);
    glVertex2f(x1 + nx, y1 + ny); glVertex2f(x2 + nx, y2 + ny);
    glVertex2f(x2 - nx, y2 - ny); glVertex2f(x1 - nx, y1 - ny);
    glEnd();
}

void ui_clip_push(float x, float y, float w, float h) {
    if (w < 0) w = 0; if (h < 0) h = 0;
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)(x * g_clip_xs), (GLint)(g_vh - (y + h)), (GLsizei)(w * g_clip_xs), (GLsizei)h);   // GL: y up
}
void ui_clip_xscale(float s) { g_clip_xs = s > 0 ? s : 1.0f; }
void ui_clip_pop() { glDisable(GL_SCISSOR_TEST); }

void ui_ellipse(float cx, float cy, float rx, float ry, float th, DWORD rgba) {
    glDisable(GL_TEXTURE_2D);
    set_color(rgba);
    glLineWidth(th);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 40; ++i) {
        float a = 2.0f * 3.14159265f * i / 40.0f;
        glVertex2f(cx + cosf(a) * rx, cy + sinf(a) * ry);
    }
    glEnd();
    glLineWidth(1.0f);
}

float ui_text(float x, float y, int px, int weight, DWORD rgba, const char* utf8) {
    const TextTex& t = get_text(utf8, px, weight);
    if (!t.id) return 0;
    draw_tex(t, x, y, (float)t.w, (float)t.h, 0, 0, 1, 1, rgba);
    return (float)t.w;
}

bool ui_image_cover(float x, float y, float w, float h, const char* path) {
    char key[560];
    snprintf(key, sizeof(key), "img:%s", path);
    auto it = g_text_cache.find(key);
    if (it == g_text_cache.end()) {
        it = g_text_cache.emplace(key, load_image(path)).first;
    }
    const TextTex& t = it->second;
    if (!t.id) return false;

    float img_ar = (float)t.w / t.h, box_ar = w / h;
    float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
    if (img_ar > box_ar) {                    // image wider than box: crop left/right
        float keep = box_ar / img_ar;
        u0 = (1.0f - keep) * 0.5f; u1 = 1.0f - u0;
    } else {                                   // image taller than box: crop top/bottom
        float keep = img_ar / box_ar;
        v0 = (1.0f - keep) * 0.5f; v1 = 1.0f - v0;
    }
    draw_tex(t, x, y, w, h, u0, v0, u1, v1, 0xFFFFFFFF);
    return true;
}

float ui_text_width(int px, int weight, const char* utf8) {
    return (float)get_text(utf8, px, weight).w;
}

float ui_smooth(long key, float target, float speed) {
    auto it = g_anim.find(key);
    if (it == g_anim.end()) it = g_anim.emplace(key, target).first;
    float k = 1.0f - expf(-speed * g_dt);
    it->second += (target - it->second) * k;
    if (it->second > target - 0.001f && it->second < target + 0.001f)
        it->second = target;
    return it->second;
}
void ui_anim_set(long key, float v) { g_anim[key] = v; }
void ui_alpha(float mul) { g_alpha_mul = mul < 0 ? 0 : (mul > 1 ? 1 : mul); }
float ui_alpha_get() { return g_alpha_mul; }

// ---------------------------------------------------------------- extended 2D
float ui_text_font(float x, float y, int px, int weight, DWORD rgba, const char* utf8, int font) {
    const TextTex& t = get_text(utf8, px, weight, font);
    if (!t.id) return 0;
    // whole pixels: a texel grid off by half a pixel is filtered into blurry type
    draw_tex(t, floorf(x + 0.5f), floorf(y + 0.5f), (float)t.w, (float)t.h, 0, 0, 1, 1, rgba);
    return (float)t.w;
}

float ui_text_font_width(int px, int weight, const char* utf8, int font) {
    return (float)get_text(utf8, px, weight, font).w;
}

float ui_text_font_height(int px, int weight, int font) {
    return (float)get_text("Hg", px, weight, font).h;
}

float ui_text_tracked(float x, float y, int px, int weight, DWORD rgba, const char* s, float tracking, int font) {
    const float x0 = x;
    for (const char* c = s; *c; ++c) {
        char g[2] = { *c, 0 };
        // GDI pads each glyph texture by 2 px (see make_text): take it back per glyph
        x += ui_text_font(x, y, px, weight, rgba, g, font) - 2 + tracking;
    }
    return x - x0 - tracking;
}

float ui_text_tracked_width(int px, int weight, const char* s, float tracking, int font) {
    float w = 0;
    int n = 0;
    for (const char* c = s; *c; ++c, ++n) {
        char g[2] = { *c, 0 };
        w += ui_text_font_width(px, weight, g, font) - 2 + tracking;
    }
    return n ? w - tracking : 0;
}

void ui_aa_rect(float x, float y, float w, float h, float r, DWORD rgba) {
    if (w <= 0 || h <= 0) return;
    const std::vector<V2> p = round_rect_path(x, y, w, h, r);
    aa_fill(p.data(), (int)p.size(), Paint{ rgba, rgba, y, y + h });
}

void ui_aa_rect_vgrad(float x, float y, float w, float h, float r, DWORD top, DWORD bottom) {
    if (w <= 0 || h <= 0) return;
    const std::vector<V2> p = round_rect_path(x, y, w, h, r);
    aa_fill(p.data(), (int)p.size(), Paint{ top, bottom, y, y + h });
}

void ui_aa_stroke_rect(float x, float y, float w, float h, float r, float th, DWORD rgba) {
    if (w <= 0 || h <= 0) return;
    const std::vector<V2> p = round_rect_path(x, y, w, h, r);
    aa_stroke(p.data(), (int)p.size(), th, rgba);
}

void ui_aa_poly(const float* xy, int n, DWORD rgba) {
    std::vector<V2> p((size_t)n);
    for (int i = 0; i < n; ++i) p[i] = { xy[i * 2], xy[i * 2 + 1] };
    aa_fill(p.data(), n, Paint{ rgba, rgba, 0, 0 });
}

void ui_aa_poly_vgrad(const float* xy, int n, DWORD top, DWORD bottom) {
    std::vector<V2> p((size_t)n);
    float y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < n; ++i) {
        p[i] = { xy[i * 2], xy[i * 2 + 1] };
        if (p[i].y < y0) y0 = p[i].y;
        if (p[i].y > y1) y1 = p[i].y;
    }
    aa_fill(p.data(), n, Paint{ top, bottom, y0, y1 });
}

void ui_aa_poly_stroke(const float* xy, int n, float th, DWORD rgba) {
    std::vector<V2> p((size_t)n);
    for (int i = 0; i < n; ++i) p[i] = { xy[i * 2], xy[i * 2 + 1] };
    aa_stroke(p.data(), n, th, rgba);
}

void ui_aa_disc(float cx, float cy, float r, DWORD rgba) {
    if (r <= 0) return;
    const std::vector<V2> p = circle_path(cx, cy, r);
    aa_fill(p.data(), (int)p.size(), Paint{ rgba, rgba, 0, 0 });
}

void ui_aa_ring(float cx, float cy, float r, float th, DWORD rgba) {
    if (r <= 0) return;
    const std::vector<V2> p = circle_path(cx, cy, r);
    aa_stroke(p.data(), (int)p.size(), th, rgba);
}

void ui_aa_line(float x1, float y1, float x2, float y2, float th, DWORD rgba) {
    const float dx = x2 - x1, dy = y2 - y1;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float k = 1.0f;
    if (th < 1.0f) { k = th; th = 1.0f; }
    const float nx = -dy / len * th * 0.5f, ny = dx / len * th * 0.5f;
    const V2 p[4] = { { x1 + nx, y1 + ny }, { x2 + nx, y2 + ny }, { x2 - nx, y2 - ny }, { x1 - nx, y1 - ny } };
    aa_fill(p, 4, Paint{ (DWORD)((DWORD)(((rgba >> 24) & 0xff) * k) << 24) | (rgba & 0xFFFFFF), 0, 0, 0 });
}

void ui_shadow(float x, float y, float w, float h, float r, float blur, DWORD rgba) {
    if (w <= 0 || h <= 0 || blur <= 0) return;
    // two rims: a wide one for the soft falloff, a tight one for the contact shadow
    const std::vector<V2> p = round_rect_path(x, y, w, h, r + blur * 0.5f);
    const DWORD half = ((DWORD)(((rgba >> 24) & 0xff) * 0.55f) << 24) | (rgba & 0xFFFFFF);
    aa_fill(p.data(), (int)p.size(), Paint{ half, half, 0, 0 }, blur * 2.0f);
    const std::vector<V2> q = round_rect_path(x, y, w, h, r);
    aa_fill(q.data(), (int)q.size(), Paint{ half, half, 0, 0 }, blur * 0.8f);
}

void ui_rect_vgrad(float x, float y, float w, float h, DWORD top, DWORD bottom) {
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    set_color(top);    glVertex2f(x, y); glVertex2f(x + w, y);
    set_color(bottom); glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();
}

void ui_rect_hgrad(float x, float y, float w, float h, DWORD left, DWORD right) {
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    set_color(left);  glVertex2f(x, y);
    set_color(right); glVertex2f(x + w, y); glVertex2f(x + w, y + h);
    set_color(left);  glVertex2f(x, y + h);
    glEnd();
}

bool ui_tex_known(const char* key) { return g_tex.find(key) != g_tex.end(); }

bool ui_tex_upload(const char* key, int w, int h, const void* rgba, bool repeat, bool mipmap) {
    TextTex t;
    if (rgba && w > 0 && h > 0) {
        glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        if (mipmap) {
            // the chain by hand (box filter, colour weighted by alpha): no GLU, no FBO
            std::vector<unsigned char> cur((const unsigned char*)rgba, (const unsigned char*)rgba + (size_t)w * h * 4);
            int cw = w, ch = h, level = 0;
            while (cw > 1 || ch > 1) {
                const int nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
                std::vector<unsigned char> nxt((size_t)nw * nh * 4);
                for (int y = 0; y < nh; ++y)
                    for (int x = 0; x < nw; ++x) {
                        float acc[4] = { 0, 0, 0, 0 };
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx) {
                                const int sx = x * 2 + dx < cw ? x * 2 + dx : cw - 1;
                                const int sy = y * 2 + dy < ch ? y * 2 + dy : ch - 1;
                                const unsigned char* s = &cur[((size_t)sy * cw + sx) * 4];
                                const float a = s[3];
                                acc[0] += s[0] * a; acc[1] += s[1] * a; acc[2] += s[2] * a; acc[3] += a;
                            }
                        unsigned char* d = &nxt[((size_t)y * nw + x) * 4];
                        for (int c = 0; c < 3; ++c) d[c] = (unsigned char)(acc[3] > 0 ? acc[c] / acc[3] : 0);
                        d[3] = (unsigned char)(acc[3] / 4 + 0.5f);
                    }
                cur.swap(nxt);
                cw = nw; ch = nh;
                glTexImage2D(GL_TEXTURE_2D, ++level, GL_RGBA, cw, ch, 0, GL_RGBA, GL_UNSIGNED_BYTE, cur.data());
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        t.w = w; t.h = h;
    }
    auto it = g_tex.find(key);
    if (it != g_tex.end()) {
        if (it->second.id) glDeleteTextures(1, &it->second.id);
        it->second = t;
    } else {
        g_tex.emplace(key, t);                    // a failure (no pixels) is remembered too
    }
    return t.id != 0;
}

bool ui_tex_size(const char* key, int* w, int* h) {
    auto it = g_tex.find(key);
    if (it == g_tex.end() || !it->second.id) return false;
    if (w) *w = it->second.w;
    if (h) *h = it->second.h;
    return true;
}

bool ui_tex_draw(const char* key, float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1, DWORD rgba) {
    auto it = g_tex.find(key);
    if (it == g_tex.end() || !it->second.id) return false;
    draw_tex(it->second, x, y, w, h, u0, v0, u1, v1, rgba);
    return true;
}

}  // namespace patches
