// draws natively inside the renderer via engine_2d (CG_Draw2D + RE_DrawStretchPic),
// not a Win32 overlay (got occluded by the GL surface). download PNG -> BGRA TGA in
// a pk3 -> R_RegisterShader -> RE_DrawStretchPic each frame after vanilla HUD.

#include "features/avatar_overlay.h"
#include "features/engine_2d.h"
#include "video/window_patch.h"
#include "core/logger.h"

#include <wininet.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace patches {

AvatarOverlayConfig g_avatar_overlay_config = {
    /* enable    */ false,
    /* test_url  */ "https://fpschallenge.b-cdn.net/public/avatar/Y8o0Wl0I6qDzNQcLvzgvkl8LSFrkRyTQ0AT1iRPc.png",
    /* x         */ 50,
    /* y         */ 50,
    /* width     */ 64,
    /* height    */ 64,
};

// 0-4 = allied, 5-9 = axis
constexpr int NUM_AVATARS = 10;
static const char* const g_avatar_urls[NUM_AVATARS] = {
    "https://fpschallenge.b-cdn.net/public/avatar/iRdUUsliaqzVD8QyoX1eqks1wkli4osjNifwmQO7.jpg",
    "https://fpschallenge.b-cdn.net/public/avatar/KBRiBY9yDj1XTmmgr5ZMFQngr2alGijG4cQOJXUZ.jpg",
    "https://fpschallenge.b-cdn.net/public/avatar/Kjt0AkwK5CAJNIy7eB8WJJTZkdkmppHgXEIrtiSq.png",
    "https://fpschallenge.b-cdn.net/public/avatar/orPCAeDmXsd4HkUYciC7So2sqlXywnP9alNIOfic.jpg",
    "https://fpschallenge.b-cdn.net/public/avatar/nJmtv1GWZk0SnYkW0xD2zVwuw2RfCf1CugaKPcKw.png",
    "https://fpschallenge.b-cdn.net/public/avatar/5ZmYGRPfGejsrQw2ZgkHjEIi0KkylyhWtlxXO5vh.jpg",
    "https://fpschallenge.b-cdn.net/public/avatar/ceogiQylyQBoaxUzu9Hecv5cgCkQNnYfawpqEgFd.jpg",
    "https://fpschallenge.b-cdn.net/public/avatar/770kvgqliD9dCIwUlYbICYrEG5y6VNgfdlGzTrv7.png",
    "https://fpschallenge.b-cdn.net/public/avatar/0dvTdM5SPRNQ8N1Ji9s9AkFMCvQsmqddDC5HKGyf.jpg",
    "https://fpschallenge.b-cdn.net/public/avatar/qbV818a8CvRtwgEH0es4FFbvngzLEGqwfatoJRmZ.jpg",
};

static const char* const g_ui_tga_names[] = {
    "textures/cod1reloaded/ui_panel.tga",
    "textures/cod1reloaded/ui_card.tga",
    "textures/cod1reloaded/ui_chip_blue.tga",
    "textures/cod1reloaded/ui_chip_red.tga",
    "textures/cod1reloaded/ui_shadow.tga",
    "textures/cod1reloaded/ui_scrim.tga",
    "textures/cod1reloaded/ui_glow.tga",
    "textures/cod1reloaded/ui_dot.tga",
    "textures/cod1reloaded/ui_box.tga",
    "textures/cod1reloaded/ui_frame.tga",
    "textures/cod1reloaded/ui_tri.tga",
};
static const char* const g_ui_shader_names[] = {
    "textures/cod1reloaded/ui_panel",
    "textures/cod1reloaded/ui_card",
    "textures/cod1reloaded/ui_chip_blue",
    "textures/cod1reloaded/ui_chip_red",
    "textures/cod1reloaded/ui_shadow",
    "textures/cod1reloaded/ui_scrim",
    "textures/cod1reloaded/ui_glow",
    "textures/cod1reloaded/ui_dot",
    "textures/cod1reloaded/ui_box",
    "textures/cod1reloaded/ui_frame",
    "textures/cod1reloaded/ui_tri",
};
constexpr int NUM_UI_SHADERS = 11;
enum { UI_PANEL = 0, UI_CARD = 1, UI_CHIP_BLUE = 2, UI_CHIP_RED = 3,
       UI_SHADOW = 4, UI_SCRIM = 5, UI_GLOW = 6, UI_DOT = 7, UI_BOX = 8,
       UI_FRAME = 9, UI_TRI = 10 };

// 0-9 + colon
constexpr int NUM_GLYPHS = 11;
constexpr int COLON_IDX  = 10;
static const wchar_t* const g_glyph_chars[NUM_GLYPHS] = {
    L"0", L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9", L":"
};

// placeholder names
constexpr int NUM_NAMES = NUM_AVATARS;
static const wchar_t* const g_name_strings[NUM_NAMES] = {
    L"JevierPotte", L"Quendrell", L"T30 Elo", L"LuckyLuc", L"SNELELE",
    L"UnitedG", L"Kimsa", L"CoCo", L"Hades", L"Fenubis"
};

constexpr int NUM_HUD = 1;
static const char* const g_hud_variants[NUM_HUD] = { "scoreboard" };
constexpr float HUD_PNG_W = 682.0f;       // source PNG dims, for UV calc on POT canvas
constexpr float HUD_PNG_H = 98.0f;
constexpr float HUD_CANVAS_W = 1024.0f;   // POT
constexpr float HUD_CANVAS_H = 128.0f;    // POT

constexpr int MAX_PK3_ENTRIES =
    NUM_AVATARS + NUM_UI_SHADERS + NUM_GLYPHS + NUM_NAMES + NUM_HUD;

namespace {

qhandle_t g_ui_handles[NUM_UI_SHADERS] = {0};
qhandle_t g_glyph_handles[NUM_GLYPHS]  = {0};
qhandle_t g_name_handles[NUM_NAMES]    = {0};
qhandle_t g_hud_handles[NUM_HUD]       = {0};

bool get_hud_png_path(const char* variant, char* out, size_t n) {
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* slash = strrchr(exe, '\\');
    if (!slash) return false;
    *(slash + 1) = '\0';
    int w = snprintf(out, n, "%smain\\hud\\hud-%s.png", exe, variant);
    return w > 0 && (size_t)w < n;
}

void glyph_name(int i, char* out, size_t n, bool with_ext) {
    if (i == COLON_IDX)
        snprintf(out, n, "textures/cod1reloaded/glyph_colon%s", with_ext ? ".tga" : "");
    else
        snprintf(out, n, "textures/cod1reloaded/glyph_%d%s", i, with_ext ? ".tga" : "");
}

void player_name_tex(int i, char* out, size_t n, bool with_ext) {
    snprintf(out, n, "textures/cod1reloaded/name_%02d%s", i, with_ext ? ".tga" : "");
}

ULONG_PTR  g_gdiplus_token       = 0;
bool       g_gdiplus_initialized = false;
qhandle_t  g_test_shader         = 0;
bool       g_shader_ready        = false;
bool       g_setup_done          = false;
char       g_shader_name[128]    = {0};

bool init_gdiplus() {
    if (g_gdiplus_initialized) return true;
    Gdiplus::GdiplusStartupInput startup;
    if (Gdiplus::GdiplusStartup(&g_gdiplus_token, &startup, nullptr) != Gdiplus::Ok)
        return false;
    g_gdiplus_initialized = true;
    return true;
}

bool download_url_to_file(const char* url, const char* path) {
    HINTERNET hi = InternetOpenA("cod1reloaded/0.2",
                                  INTERNET_OPEN_TYPE_PRECONFIG,
                                  nullptr, nullptr, 0);
    if (!hi) return false;
    HINTERNET hu = InternetOpenUrlA(hi, url, nullptr, 0,
                                     INTERNET_FLAG_RELOAD |
                                     INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hu) { InternetCloseHandle(hi); return false; }

    FILE* f = fopen(path, "wb");
    if (!f) { InternetCloseHandle(hu); InternetCloseHandle(hi); return false; }

    char buf[8192];
    DWORD n;
    bool ok = true;
    size_t total = 0;
    while (InternetReadFile(hu, buf, sizeof(buf), &n) && n > 0) {
        if (fwrite(buf, 1, n, f) != n) { ok = false; break; }
        total += n;
    }
    fclose(f);
    InternetCloseHandle(hu);
    InternetCloseHandle(hi);
    if (ok) logger::logf("avatar_overlay: downloaded %zu bytes to %s",
                         total, path);
    return ok;
}

// crc32 poly 0xEDB88320, for the zip/pk3 format
uint32_t crc32_calc(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool inited = false;
    if (!inited) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
            table[i] = c;
        }
        inited = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFFu];
    return crc ^ 0xFFFFFFFFu;
}

#pragma pack(push, 1)
struct TGAHeader {
    uint8_t  idLength;
    uint8_t  colorMapType;
    uint8_t  imageType;
    uint16_t colorMapStart;
    uint16_t colorMapLength;
    uint8_t  colorMapDepth;
    uint16_t xOrigin;
    uint16_t yOrigin;
    uint16_t width;
    uint16_t height;
    uint8_t  pixelDepth;
    uint8_t  imageDescriptor;
};
#pragma pack(pop)

struct Pk3Entry {
    char     name[64];   // virtual path inside the pk3
    uint8_t* data;       // caller frees after pk3 is written
    size_t   size;
};

// uncompressed (STORE) multi-file zip
bool create_pk3_with_files(const char* pk3_path, const Pk3Entry* entries, int count) {
    if (count <= 0) return false;
    FILE* f = fopen(pk3_path, "wb");
    if (!f) return false;

    auto put32 = [&](uint32_t v) { fwrite(&v, 1, 4, f); };
    auto put16 = [&](uint16_t v) { fwrite(&v, 1, 2, f); };

    uint32_t local_offsets[MAX_PK3_ENTRIES] = {0};
    uint32_t crcs[MAX_PK3_ENTRIES]          = {0};

    for (int i = 0; i < count; i++) {  // local file headers
        local_offsets[i] = (uint32_t)ftell(f);
        crcs[i] = crc32_calc(entries[i].data, entries[i].size);
        const uint16_t name_len = (uint16_t)strlen(entries[i].name);

        put32(0x04034b50);
        put16(20); put16(0); put16(0); put16(0); put16(0);
        put32(crcs[i]);
        put32((uint32_t)entries[i].size);
        put32((uint32_t)entries[i].size);
        put16(name_len);
        put16(0);
        fwrite(entries[i].name, 1, name_len, f);
        fwrite(entries[i].data, 1, entries[i].size, f);
    }

    const uint32_t central_offset = (uint32_t)ftell(f);

    for (int i = 0; i < count; i++) {  // central directory
        const uint16_t name_len = (uint16_t)strlen(entries[i].name);
        put32(0x02014b50);
        put16(20); put16(20); put16(0); put16(0); put16(0); put16(0);
        put32(crcs[i]);
        put32((uint32_t)entries[i].size);
        put32((uint32_t)entries[i].size);
        put16(name_len);
        put16(0); put16(0); put16(0); put16(0);
        put32(0);
        put32(local_offsets[i]);
        fwrite(entries[i].name, 1, name_len, f);
    }

    const uint32_t central_size = (uint32_t)ftell(f) - central_offset;

    put32(0x06054b50);  // eocd
    put16(0); put16(0);
    put16((uint16_t)count);
    put16((uint16_t)count);
    put32(central_size);
    put32(central_offset);
    put16(0);

    fclose(f);
    return true;
}

void add_rounded_rect(Gdiplus::GraphicsPath& path,
                      float x, float y, float w, float h, float r) {
    path.Reset();
    if (r <= 0.5f) {  // sharp corners
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    if (r * 2.0f > w) r = w / 2.0f;
    if (r * 2.0f > h) r = h / 2.0f;
    const float d = r * 2.0f;
    path.AddArc(x,         y,         d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y,         d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d,   0.0f, 90.0f);
    path.AddArc(x,         y + h - d, d, d,  90.0f, 90.0f);
    path.CloseFigure();
}

// 32bpp bitmap -> top-down BGRA TGA (malloc'd)
bool bitmap_to_tga_memory(Gdiplus::Bitmap& bmp, uint8_t** out_buf, size_t* out_size) {
    *out_buf = nullptr; *out_size = 0;
    const UINT w = bmp.GetWidth();
    const UINT h = bmp.GetHeight();
    if (w == 0 || h == 0) return false;

    Gdiplus::Rect rect(0, 0, (INT)w, (INT)h);
    Gdiplus::BitmapData data;
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead,
                     PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
        return false;
    }
    const size_t buf_size = sizeof(TGAHeader) + (size_t)w * h * 4;
    uint8_t* buf = (uint8_t*)malloc(buf_size);
    if (!buf) { bmp.UnlockBits(&data); return false; }

    TGAHeader hdr = {};
    hdr.imageType       = 2;
    hdr.width           = (uint16_t)w;
    hdr.height          = (uint16_t)h;
    hdr.pixelDepth      = 32;
    hdr.imageDescriptor = 0x28;
    memcpy(buf, &hdr, sizeof(hdr));

    uint8_t* p = buf + sizeof(hdr);
    for (UINT row = 0; row < h; row++) {
        memcpy(p, (const uint8_t*)data.Scan0 + row * data.Stride, (size_t)w * 4);
        p += (size_t)w * 4;
    }
    bmp.UnlockBits(&data);
    *out_buf = buf; *out_size = buf_size;
    return true;
}

bool make_rounded_panel_tga(UINT size, float radius,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                            uint8_t** out_buf, size_t* out_size) {
    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics gfx(&bmp);
        gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath path;
        add_rounded_rect(path, 1.0f, 1.0f, (float)size - 2.0f, (float)size - 2.0f, radius);
        Gdiplus::SolidBrush brush(Gdiplus::Color(a, r, g, b));
        gfx.FillPath(&brush, &path);
    }
    return bitmap_to_tga_memory(bmp, out_buf, out_size);
}

bool make_rounded_frame_tga(UINT size, float radius, float stroke,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                            uint8_t** out_buf, size_t* out_size) {
    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics gfx(&bmp);
        gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath path;
        const float half = stroke / 2.0f;
        add_rounded_rect(path, half + 1.0f, half + 1.0f,
                         (float)size - stroke - 2.0f, (float)size - stroke - 2.0f, radius);
        Gdiplus::Pen pen(Gdiplus::Color(a, r, g, b), stroke);
        gfx.DrawPath(&pen, &path);
    }
    return bitmap_to_tga_memory(bmp, out_buf, out_size);
}

bool make_gradient_panel_tga(UINT size, float radius,
                             uint8_t a1, uint8_t r1, uint8_t g1, uint8_t b1,  // top
                             uint8_t a2, uint8_t r2, uint8_t g2, uint8_t b2,  // bottom
                             uint8_t rim_a,
                             uint8_t** out_buf, size_t* out_size) {
    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics gfx(&bmp);
        gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath path;
        add_rounded_rect(path, 1.0f, 1.0f, (float)size - 2.0f, (float)size - 2.0f, radius);
        Gdiplus::RectF rc(0.0f, 0.0f, (Gdiplus::REAL)size, (Gdiplus::REAL)size + 1.0f);
        Gdiplus::LinearGradientBrush grad(
            rc, Gdiplus::Color(a1, r1, g1, b1), Gdiplus::Color(a2, r2, g2, b2),
            Gdiplus::LinearGradientModeVertical);
        gfx.FillPath(&grad, &path);
        {  // top specular highlight
            gfx.SetClip(&path);
            const float hh = (float)size * 0.40f;
            Gdiplus::RectF hr(0.0f, -1.0f, (Gdiplus::REAL)size, hh + 1.0f);
            Gdiplus::LinearGradientBrush hl(
                hr, Gdiplus::Color(22, 255, 255, 255), Gdiplus::Color(0, 255, 255, 255),
                Gdiplus::LinearGradientModeVertical);
            gfx.FillRectangle(&hl, 0.0f, 0.0f, (Gdiplus::REAL)size, hh);
            gfx.ResetClip();
        }
        if (rim_a > 0) {
            Gdiplus::Pen rim(Gdiplus::Color(rim_a, 255, 255, 255), 1.25f);
            gfx.DrawPath(&rim, &path);
        }
    }
    return bitmap_to_tga_memory(bmp, out_buf, out_size);
}

// stacked rounded-rects (edge->center) = drop shadow that hugs the shape
bool make_soft_shadow_tga(UINT size, float radius,
                          uint8_t center_a,
                          uint8_t** out_buf, size_t* out_size) {
    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics gfx(&bmp);
        gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const int   rings     = 22;
        const float inset_max = (float)size * 0.22f;   // blur margin
        const float per = (float)center_a / (float)rings * 1.35f;
        for (int i = 0; i < rings; i++) {
            const float f = (float)i / (float)(rings - 1);   // 0 edge -> 1 center
            const float inset = inset_max * (1.0f - f);
            Gdiplus::GraphicsPath path;
            add_rounded_rect(path, inset, inset,
                             (float)size - 2.0f * inset, (float)size - 2.0f * inset,
                             radius);
            Gdiplus::SolidBrush b(Gdiplus::Color((BYTE)per, 0, 0, 0));
            gfx.FillPath(&b, &path);
        }
    }
    return bitmap_to_tga_memory(bmp, out_buf, out_size);
}

// caller deletes
Gdiplus::FontFamily* pick_ui_font() {
    const wchar_t* fams[] = {
        L"Bahnschrift SemiBold", L"Bahnschrift", L"Segoe UI Semibold",
        L"Segoe UI", L"Arial"
    };
    for (const wchar_t* fn : fams) {
        Gdiplus::FontFamily* f = new Gdiplus::FontFamily(fn);
        if (f->IsAvailable()) return f;
        delete f;
    }
    return new Gdiplus::FontFamily(L"Arial");
}

// white glyph in a POT cell; recolor via RE_SetColor at draw time
bool make_glyph_tga(const wchar_t* ch, uint32_t cell_w, uint32_t cell_h,
                    uint8_t** out_buf, size_t* out_size) {
    Gdiplus::Bitmap bmp(cell_w, cell_h, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics gfx(&bmp);
        gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::FontFamily* fam = pick_ui_font();
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF layout(0.0f, 0.0f, (Gdiplus::REAL)cell_w, (Gdiplus::REAL)cell_h);
        Gdiplus::GraphicsPath path;
        path.AddString(ch, -1, fam, Gdiplus::FontStyleBold,
                       (Gdiplus::REAL)(cell_h * 0.80f), layout, &fmt);
        Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
        gfx.FillPath(&white, &path);
        delete fam;
    }
    return bitmap_to_tga_memory(bmp, out_buf, out_size);
}

// white text in a POT cell; font_frac = font height / cell_h
bool make_text_tga(const wchar_t* text, uint32_t cell_w, uint32_t cell_h,
                   float font_frac, uint8_t** out_buf, size_t* out_size) {
    Gdiplus::Bitmap bmp(cell_w, cell_h, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics gfx(&bmp);
        gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::FontFamily* fam = pick_ui_font();
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::RectF layout(4.0f, 0.0f, (Gdiplus::REAL)cell_w - 8.0f, (Gdiplus::REAL)cell_h);
        Gdiplus::GraphicsPath path;
        path.AddString(text, -1, fam, Gdiplus::FontStyleBold,
                       (Gdiplus::REAL)(cell_h * font_frac), layout, &fmt);
        Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
        gfx.FillPath(&white, &path);
        delete fam;
    }
    return bitmap_to_tga_memory(bmp, out_buf, out_size);
}

// vertical gradient dark(top) -> transparent(bottom)
bool make_scrim_tga(uint32_t w, uint32_t h, uint8_t** out_buf, size_t* out_size) {
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics gfx(&bmp);
        gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
        Gdiplus::RectF rc(0.0f, 0.0f, (Gdiplus::REAL)w, (Gdiplus::REAL)h + 1.0f);
        Gdiplus::LinearGradientBrush grad(
            rc, Gdiplus::Color(200, 6, 7, 10), Gdiplus::Color(0, 6, 7, 10),
            Gdiplus::LinearGradientModeVertical);
        gfx.FillRectangle(&grad, 0, 0, (INT)w, (INT)h);
    }
    return bitmap_to_tga_memory(bmp, out_buf, out_size);
}

// white radial glow, tinted at draw time
bool make_glow_tga(uint32_t size, uint8_t** out_buf, size_t* out_size) {
    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics gfx(&bmp);
        gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath path;
        path.AddEllipse(0.0f, 0.0f, (Gdiplus::REAL)size, (Gdiplus::REAL)size);
        Gdiplus::PathGradientBrush pg(&path);
        pg.SetCenterColor(Gdiplus::Color(255, 255, 255, 255));
        Gdiplus::Color surround[1] = { Gdiplus::Color(0, 255, 255, 255) };
        int n = 1;
        pg.SetSurroundColors(surround, &n);
        gfx.FillPath(&pg, &path);
    }
    return bitmap_to_tga_memory(bmp, out_buf, out_size);
}

// white up-triangle, tinted at draw time
bool make_triangle_tga(uint32_t size, uint8_t** out_buf, size_t* out_size) {
    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics gfx(&bmp);
        gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::PointF pts[3] = {
            Gdiplus::PointF((Gdiplus::REAL)size * 0.5f,  (Gdiplus::REAL)size * 0.16f),
            Gdiplus::PointF((Gdiplus::REAL)size * 0.86f, (Gdiplus::REAL)size * 0.84f),
            Gdiplus::PointF((Gdiplus::REAL)size * 0.14f, (Gdiplus::REAL)size * 0.84f)
        };
        Gdiplus::SolidBrush w(Gdiplus::Color(255, 255, 255, 255));
        gfx.FillPolygon(&w, pts, 3);
    }
    return bitmap_to_tga_memory(bmp, out_buf, out_size);
}

// decode -> POT 1024x1024 BGRA TGA (malloc'd). oversampled; mipmaps downsample crisply.
bool png_to_tga_memory(const char* img_path, uint8_t** out_buf, size_t* out_size) {
    *out_buf = nullptr;
    *out_size = 0;

    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, img_path, -1, wpath, MAX_PATH);

    Gdiplus::Bitmap src(wpath);
    if (src.GetLastStatus() != Gdiplus::Ok) return false;
    const UINT sw = src.GetWidth();
    const UINT sh = src.GetHeight();
    if (sw == 0 || sh == 0 || sw > 4096 || sh > 4096) return false;

    const UINT TGT = 1024;
    Gdiplus::Bitmap dst(TGT, TGT, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&dst);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));   // transparent base
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.DrawImage(&src, 0, 0, (INT)TGT, (INT)TGT);
    }

    Gdiplus::Rect rect(0, 0, (INT)TGT, (INT)TGT);
    Gdiplus::BitmapData data;
    if (dst.LockBits(&rect, Gdiplus::ImageLockModeRead,
                     PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
        return false;
    }

    const size_t buf_size = sizeof(TGAHeader) + TGT * TGT * 4;
    uint8_t* buf = (uint8_t*)malloc(buf_size);
    if (!buf) { dst.UnlockBits(&data); return false; }

    TGAHeader hdr = {};
    hdr.imageType       = 2;
    hdr.width           = (uint16_t)TGT;
    hdr.height          = (uint16_t)TGT;
    hdr.pixelDepth      = 32;
    hdr.imageDescriptor = 0x28;
    memcpy(buf, &hdr, sizeof(hdr));

    uint8_t* p = buf + sizeof(hdr);
    for (UINT row = 0; row < TGT; row++) {
        memcpy(p, (const uint8_t*)data.Scan0 + row * data.Stride, TGT * 4);
        p += TGT * 4;
    }
    dst.UnlockBits(&data);

    *out_buf  = buf;
    *out_size = buf_size;
    return true;
}

// PNG at native size into a transparent POT canvas (cw x ch); draw samples via UVs
bool png_to_pot_tga(const char* img_path, uint32_t cw, uint32_t ch,
                    uint8_t** out_buf, size_t* out_size) {
    *out_buf = nullptr;
    *out_size = 0;

    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, img_path, -1, wpath, MAX_PATH);

    Gdiplus::Bitmap src(wpath);
    if (src.GetLastStatus() != Gdiplus::Ok) return false;
    const UINT sw = src.GetWidth();
    const UINT sh = src.GetHeight();
    if (sw == 0 || sh == 0 || sw > cw || sh > ch) return false;

    Gdiplus::Bitmap dst(cw, ch, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&dst);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(&src, 0, 0, (INT)sw, (INT)sh);
    }
    return bitmap_to_tga_memory(dst, out_buf, out_size);
}

// solid-color 64x64 POT BGRA TGA (malloc'd)
bool make_solid_tga_memory(uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                           uint8_t** out_buf, size_t* out_size) {
    *out_buf = nullptr;
    *out_size = 0;
    const UINT TGT = 64;
    const size_t buf_size = sizeof(TGAHeader) + TGT * TGT * 4;
    uint8_t* buf = (uint8_t*)malloc(buf_size);
    if (!buf) return false;

    TGAHeader hdr = {};
    hdr.imageType       = 2;
    hdr.width           = (uint16_t)TGT;
    hdr.height          = (uint16_t)TGT;
    hdr.pixelDepth      = 32;
    hdr.imageDescriptor = 0x28;
    memcpy(buf, &hdr, sizeof(hdr));

    uint8_t* p = buf + sizeof(hdr);
    for (UINT i = 0; i < TGT * TGT; i++) {
        *p++ = b;  // BGRA
        *p++ = g;
        *p++ = r;
        *p++ = a;
    }
    *out_buf  = buf;
    *out_size = buf_size;
    return true;
}

// single-file variant, unused
bool create_pk3_with_file(const char* pk3_path,
                          const char* internal_name,
                          const uint8_t* file_data,
                          size_t file_size) {
    FILE* f = fopen(pk3_path, "wb");
    if (!f) return false;

    const uint32_t crc      = crc32_calc(file_data, file_size);
    const uint16_t name_len = (uint16_t)strlen(internal_name);
    const uint32_t local_offset = 0;

    auto put32 = [&](uint32_t v) { fwrite(&v, 1, 4, f); };
    auto put16 = [&](uint16_t v) { fwrite(&v, 1, 2, f); };

    // local file header
    put32(0x04034b50);
    put16(20);
    put16(0);
    put16(0);           // STORE
    put16(0);
    put16(0);
    put32(crc);
    put32((uint32_t)file_size);
    put32((uint32_t)file_size);
    put16(name_len);
    put16(0);
    fwrite(internal_name, 1, name_len, f);
    fwrite(file_data, 1, file_size, f);

    const uint32_t central_offset = (uint32_t)ftell(f);

    // central directory entry
    put32(0x02014b50);
    put16(20);
    put16(20);
    put16(0);
    put16(0);
    put16(0);
    put16(0);
    put32(crc);
    put32((uint32_t)file_size);
    put32((uint32_t)file_size);
    put16(name_len);
    put16(0);
    put16(0);
    put16(0);
    put16(0);
    put32(0);
    put32(local_offset);
    fwrite(internal_name, 1, name_len, f);

    const uint32_t central_size = (uint32_t)ftell(f) - central_offset;

    // eocd
    put32(0x06054b50);
    put16(0);
    put16(0);
    put16(1);
    put16(1);
    put32(central_size);
    put32(central_offset);
    put16(0);

    fclose(f);
    return true;
}

// PNG -> 128x128 POT BGRA TGA on disk (renderer rejects non-POT)
bool png_to_tga(const char* png_path, const char* tga_path) {
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, png_path, -1, wpath, MAX_PATH);

    Gdiplus::Bitmap src(wpath);
    if (src.GetLastStatus() != Gdiplus::Ok) {
        logger::logf("avatar_overlay: GDI+ PNG load failed (status=%d)",
                     (int)src.GetLastStatus());
        return false;
    }
    const UINT sw = src.GetWidth();
    const UINT sh = src.GetHeight();
    if (sw == 0 || sh == 0 || sw > 4096 || sh > 4096) {
        logger::logf("avatar_overlay: rejected suspicious dims %ux%u", sw, sh);
        return false;
    }

    const UINT TGT = 128;
    Gdiplus::Bitmap dst(TGT, TGT, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&dst);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.DrawImage(&src, 0, 0, (INT)TGT, (INT)TGT);
    }

    Gdiplus::Rect rect(0, 0, (INT)TGT, (INT)TGT);
    Gdiplus::BitmapData data;
    if (dst.LockBits(&rect, Gdiplus::ImageLockModeRead,
                     PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
        return false;
    }

    FILE* f = fopen(tga_path, "wb");
    if (!f) { dst.UnlockBits(&data); return false; }

    TGAHeader hdr = {};
    hdr.imageType       = 2;
    hdr.width           = (uint16_t)TGT;
    hdr.height          = (uint16_t)TGT;
    hdr.pixelDepth      = 32;
    hdr.imageDescriptor = 0x28;     // top-down, 8 alpha bits
    fwrite(&hdr, 1, sizeof(hdr), f);

    for (UINT row = 0; row < TGT; row++) {
        fwrite((const uint8_t*)data.Scan0 + row * data.Stride, 1, TGT * 4, f);
    }
    fclose(f);
    dst.UnlockBits(&data);
    logger::logf("avatar_overlay: TGA written %ux%u (source was %ux%u) to %s",
                 TGT, TGT, sw, sh, tga_path);
    return true;
}

// resolve + create <CoD1>/main/textures/cod1reloaded/
bool prepare_target_dir(char* out_dir, size_t out_size) {
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* slash = strrchr(exe, '\\');
    if (!slash) return false;
    *(slash + 1) = '\0';

    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%smain", exe);
    CreateDirectoryA(p, NULL);
    snprintf(p, sizeof(p), "%smain\\textures", exe);
    CreateDirectoryA(p, NULL);
    snprintf(p, sizeof(p), "%smain\\textures\\cod1reloaded", exe);
    CreateDirectoryA(p, NULL);

    int written = snprintf(out_dir, out_size,
                           "%smain\\textures\\cod1reloaded\\", exe);
    return written > 0 && (size_t)written < out_size;
}

static qhandle_t g_avatar_shaders[NUM_AVATARS] = {0};
static int       g_avatar_shaders_count = 0;

// diagnostic probes (cgame globals)
struct ShaderProbe {
    qhandle_t  handle;
    const char* label;
};
static ShaderProbe g_probes[6] = {};
static int         g_probe_count = 0;

// engine_2d calls this after CG_RegisterCgameShaders
void on_post_register_shaders() {
    if (g_avatar_shaders_count > 0) return;  // idempotent
    for (int i = 0; i < NUM_AVATARS; i++) {
        char name[64];
        snprintf(name, sizeof(name), "textures/cod1reloaded/avatar_%02d", i + 1);
        qhandle_t h = engine_2d_register_shader(name, 5);
        g_avatar_shaders[i] = h;
        if (h > 0) g_avatar_shaders_count++;
    }
    logger::logf("avatar_overlay: registered %d/%d avatars (handles: "
                 "%d %d %d %d %d | %d %d %d %d %d)",
                 g_avatar_shaders_count, NUM_AVATARS,
                 g_avatar_shaders[0], g_avatar_shaders[1], g_avatar_shaders[2],
                 g_avatar_shaders[3], g_avatar_shaders[4],
                 g_avatar_shaders[5], g_avatar_shaders[6], g_avatar_shaders[7],
                 g_avatar_shaders[8], g_avatar_shaders[9]);

    for (int i = 0; i < NUM_UI_SHADERS; i++) {
        g_ui_handles[i] = engine_2d_register_shader(g_ui_shader_names[i], 5);
    }
    logger::logf("avatar_overlay: UI shaders panel=%d card=%d chipB=%d shadow=%d scrim=%d glow=%d dot=%d",
                 g_ui_handles[UI_PANEL], g_ui_handles[UI_CARD], g_ui_handles[UI_CHIP_BLUE],
                 g_ui_handles[UI_SHADOW], g_ui_handles[UI_SCRIM], g_ui_handles[UI_GLOW],
                 g_ui_handles[UI_DOT]);

    int glyphs_ok = 0;
    for (int i = 0; i < NUM_GLYPHS; i++) {
        char name[64];
        glyph_name(i, name, sizeof(name), false);
        g_glyph_handles[i] = engine_2d_register_shader(name, 5);
        if (g_glyph_handles[i] > 0) glyphs_ok++;
    }
    logger::logf("avatar_overlay: glyphs registered %d/%d (0=%d 1=%d colon=%d)",
                 glyphs_ok, NUM_GLYPHS,
                 g_glyph_handles[0], g_glyph_handles[1], g_glyph_handles[COLON_IDX]);

    int names_ok = 0;
    for (int i = 0; i < NUM_NAMES; i++) {
        char name[64];
        player_name_tex(i, name, sizeof(name), false);
        g_name_handles[i] = engine_2d_register_shader(name, 5);
        if (g_name_handles[i] > 0) names_ok++;
    }
    logger::logf("avatar_overlay: names registered %d/%d", names_ok, NUM_NAMES);

    for (int i = 0; i < NUM_HUD; i++) {
        char nm[80];
        snprintf(nm, sizeof(nm), "textures/cod1reloaded/hud_%s", g_hud_variants[i]);
        g_hud_handles[i] = engine_2d_register_shader(nm, 5);
    }
    logger::logf("avatar_overlay: hud bar registered = %d", g_hud_handles[0]);
}

double g_anim_start = -1.0;

double overlay_clock_sec() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return freq.QuadPart ? (double)c.QuadPart / (double)freq.QuadPart : 0.0;
}
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }
float ease_out_cubic(float x) { float u = 1.0f - x; return 1.0f - u * u * u; }
float ease_out_back(float x) {  // overshoot
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float u = x - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

// 9-slice draw; uv_corner = source corner frac [0..0.5], dst_corner = px
void draw_9slice(float x, float y, float w, float h,
                 float dst_corner, float uv_corner, qhandle_t handle) {
    if (handle <= 0) return;
    float c = dst_corner;
    if (c * 2.0f > w) c = w * 0.5f;
    if (c * 2.0f > h) c = h * 0.5f;
    const float u = uv_corner;
    const float xs[4] = { x, x + c, x + w - c, x + w };
    const float ss[4] = { 0.0f, u, 1.0f - u, 1.0f };
    const float ys[4] = { y, y + c, y + h - c, y + h };
    const float ts[4] = { 0.0f, u, 1.0f - u, 1.0f };
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            engine_2d_draw_stretch_pic(
                xs[col], ys[row], xs[col + 1] - xs[col], ys[row + 1] - ys[row],
                ss[col], ts[row], ss[col + 1], ts[row + 1], handle);
        }
    }
}

void draw_glyph(int idx, float x, float y, float w, float h) {
    if (idx < 0 || idx >= NUM_GLYPHS) return;
    if (g_glyph_handles[idx] > 0)
        engine_2d_draw_pic(x, y, w, h, g_glyph_handles[idx]);
}

// digits/":" centered on cx; advance = glyph_h * advance_ratio
void draw_number(const char* s, float cx, float top, float glyph_h,
                 float advance_ratio) {
    const float advance  = glyph_h * advance_ratio;
    const float glyph_w  = glyph_h * 0.5f;   // source cell 64x128
    int n = 0;
    for (const char* p = s; *p; ++p) n++;
    const float total = n * advance;
    float x = cx - total * 0.5f;
    for (const char* p = s; *p; ++p) {
        int idx = (*p == ':') ? COLON_IDX
                : (*p >= '0' && *p <= '9') ? (*p - '0')
                : -1;
        if (idx >= 0) {
            draw_glyph(idx, x + (advance - glyph_w) * 0.5f, top, glyph_w, glyph_h);
        }
        x += advance;
    }
}

// engine_2d calls this each frame after the vanilla HUD; 2D state is ready
void on_post_hud_draw() {
    if (!g_avatar_overlay_config.enable) return;

    // pre-registered handles from cgame globals (CG_RegisterCgameShaders)
    if (!g_shader_ready && g_setup_done) {
        HMODULE cgame = GetModuleHandleA("cgame_mp_x86.dll");
        if (cgame) {
            const uintptr_t base = (uintptr_t)cgame;
            qhandle_t h_backtile  = *(qhandle_t*)(base + 0x1d9650);
            qhandle_t h_noweapon  = *(qhandle_t*)(base + 0x1d9654);
            qhandle_t h_flare     = *(qhandle_t*)(base + 0x1d98d8);
            qhandle_t h_allied    = *(qhandle_t*)(base + 0x1d9620);
            qhandle_t h_axis      = *(qhandle_t*)(base + 0x1d961c);
            qhandle_t h_uicheck   = *(qhandle_t*)(base + 0x1d9d14);
            logger::logf("avatar_overlay: pre-registered handles: "
                         "backtile=%d noweapon=%d flare=%d allied=%d axis=%d uicheck=%d",
                         h_backtile, h_noweapon, h_flare,
                         h_allied, h_axis, h_uicheck);

            ShaderProbe candidates[] = {
                { h_allied,   "allied" },
                { h_axis,     "axis" },
                { h_noweapon, "noweap" },
                { h_flare,    "flare" },
                { h_uicheck,  "uicheck" },
                { h_backtile, "backtile" },
            };
            for (auto& c : candidates) {
                if (c.handle > 0 && g_probe_count < (int)(sizeof(g_probes)/sizeof(g_probes[0]))) {
                    g_probes[g_probe_count++] = c;
                }
            }
            if (g_probe_count > 0) g_test_shader = g_probes[0].handle;
        }
        g_shader_ready = true;
        logger::logf("avatar_overlay: %d valid probe shaders ready", g_probe_count);
    }

    // RE_DrawStretchPic uses pixel coords, not 640x480 virtual; layout from live viewport
    if (g_avatar_shaders_count > 0) {
        HWND hwnd = get_game_window();
        RECT rc = {};
        float vid_w = 1440.0f, vid_h = 1080.0f;  // fallback
        if (hwnd && GetClientRect(hwnd, &rc)) {
            vid_w = (float)(rc.right - rc.left);
            vid_h = (float)(rc.bottom - rc.top);
        }

        const float BW = vid_w * 0.50f;
        const float BH = BW * HUD_PNG_H / HUD_PNG_W;     // ratio 98/682
        const float bx = (vid_w - BW) * 0.5f;
        const float by = vid_h * 0.020f;

        const double now = overlay_clock_sec();
        if (g_anim_start < 0.0) g_anim_start = now;
        const float t = (float)(now - g_anim_start);
        const float a = ease_out_cubic(clampf(t / 0.5f, 0.0f, 1.0f));

        // image coords -> screen
        auto SX = [&](float ix) { return bx + (ix / HUD_PNG_W) * BW; };
        auto SY = [&](float iy) { return by + (iy / HUD_PNG_H) * BH; };

        if (g_hud_handles[0] > 0) {  // chassis
            engine_2d_set_color(1.0f, 1.0f, 1.0f, a);
            engine_2d_draw_stretch_pic(bx, by, BW, BH, 0.0f, 0.0f,
                                       HUD_PNG_W / HUD_CANVAS_W,
                                       HUD_PNG_H / HUD_CANVAS_H, g_hud_handles[0]);
        }

        // avatars in the 10 slots (interior y34..80, 2px inset)
        const float lefts[5]  = {  16.0f,  68.0f, 120.0f, 172.0f, 224.0f };
        const float rights[5] = { 410.0f, 462.0f, 514.0f, 566.0f, 618.0f };
        const float aw = (43.0f / HUD_PNG_W) * BW;
        const float ah = (46.0f / HUD_PNG_H) * BH;
        auto put_av = [&](float L, qhandle_t av) {
            if (av <= 0) return;
            engine_2d_set_color(1.0f, 1.0f, 1.0f, a);
            engine_2d_draw_pic(SX(L + 2.0f), SY(34.0f), aw, ah, av);
        };
        for (int i = 0; i < 5; i++) put_av(lefts[i],  g_avatar_shaders[i]);
        for (int i = 0; i < 5; i++) put_av(rights[i], g_avatar_shaders[5 + i]);

        // timer + scores in the central boxes; -0.40*h offsets the glyph's vertical bias
        auto num = [&](const char* s, float icx, float icy, float ih) {
            const float h = (ih / HUD_PNG_H) * BH;
            engine_2d_set_color(1.0f, 1.0f, 1.0f, a);
            draw_number(s, SX(icx), SY(icy) - h * 0.40f, h, 0.40f);
        };
        const float frac = t - (float)(int)t;
        const float pop  = 1.0f + 0.06f * (1.0f - clampf(frac / 0.18f, 0.0f, 1.0f));
        int rem = 99 - ((int)t % 100);
        char timer[8];
        snprintf(timer, sizeof(timer), "%d:%02d", rem / 60, rem % 60);
        num(timer, 340.0f, 32.5f, 20.0f * pop);   // timer
        num("9",   315.0f, 63.5f, 30.0f);          // axis score
        num("11",  366.0f, 63.5f, 30.0f);          // allies score

        engine_2d_reset_color();
    } else {
        // probe grid while shaders aren't ready
        const float size = 64.0f;
        const float pad  = 6.0f;
        float x = (float)g_avatar_overlay_config.x;
        const float y = (float)g_avatar_overlay_config.y;
        for (int i = 0; i < g_probe_count; i++) {
            engine_2d_draw_pic(x, y, size, size, g_probes[i].handle);
            x += size + pad;
        }
    }
}

}  // namespace

bool get_pk3_path(char* out, size_t out_size) {
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* slash = strrchr(exe, '\\');
    if (!slash) return false;
    *(slash + 1) = '\0';
    int n = snprintf(out, out_size, "%smain\\z_cod1reloaded.pk3", exe);
    return n > 0 && (size_t)n < out_size;
}

// FNV-1a of url list + version = pk3 cache key. bump version when pk3 layout/format changes.
constexpr uint8_t PK3_FORMAT_VERSION = 22;
uint32_t compute_url_list_hash() {
    uint32_t h = 0x811c9dc5u;
    h ^= PK3_FORMAT_VERSION;
    h *= 0x01000193u;
    for (int i = 0; i < NUM_AVATARS; i++) {
        for (const char* p = g_avatar_urls[i]; *p; p++) {
            h ^= (uint8_t)*p;
            h *= 0x01000193u;
        }
        h ^= 0xff;
        h *= 0x01000193u;
    }
    return h;
}

bool cache_hash_matches() {
    char pk3_path[MAX_PATH];
    if (!get_pk3_path(pk3_path, sizeof(pk3_path))) return false;
    if (GetFileAttributesA(pk3_path) == INVALID_FILE_ATTRIBUTES) return false;

    char hash_path[MAX_PATH];
    snprintf(hash_path, sizeof(hash_path), "%s.hash", pk3_path);
    FILE* f = fopen(hash_path, "rb");
    if (!f) return false;
    uint32_t stored = 0;
    bool ok = (fread(&stored, sizeof(stored), 1, f) == 1);
    fclose(f);
    return ok && (stored == compute_url_list_hash());
}

void cache_hash_save() {
    char pk3_path[MAX_PATH];
    if (!get_pk3_path(pk3_path, sizeof(pk3_path))) return;
    char hash_path[MAX_PATH];
    snprintf(hash_path, sizeof(hash_path), "%s.hash", pk3_path);
    FILE* f = fopen(hash_path, "wb");
    if (!f) return;
    uint32_t h = compute_url_list_hash();
    fwrite(&h, sizeof(h), 1, f);
    fclose(f);
}

// downloads avatars, bakes all assets into one pk3, saves cache hash
DWORD WINAPI prepare_pk3_worker(LPVOID) {
    logger::logf("avatar_overlay: async pk3 worker started (%d slots)", NUM_AVATARS);

    if (!init_gdiplus()) {
        logger::logf("avatar_overlay: GDI+ init failed in worker");
        return 1;
    }

    char dir[MAX_PATH];
    if (!prepare_target_dir(dir, sizeof(dir))) return 1;

    Pk3Entry entries[MAX_PK3_ENTRIES] = {};
    int num_entries = 0;

    for (int i = 0; i < NUM_AVATARS; i++) {
        char tmp_path[MAX_PATH];
        snprintf(tmp_path, sizeof(tmp_path), "%savatar_%02d.dl", dir, i + 1);

        if (!download_url_to_file(g_avatar_urls[i], tmp_path)) {
            logger::logf("avatar_overlay: slot %d download failed (err=%lu)",
                         i + 1, GetLastError());
            continue;
        }

        uint8_t* tga_buf = nullptr;
        size_t   tga_size = 0;
        if (!png_to_tga_memory(tmp_path, &tga_buf, &tga_size)) {
            logger::logf("avatar_overlay: slot %d TGA convert failed", i + 1);
            DeleteFileA(tmp_path);
            continue;
        }
        DeleteFileA(tmp_path);

        snprintf(entries[num_entries].name, sizeof(entries[num_entries].name),
                 "textures/cod1reloaded/avatar_%02d.tga", i + 1);
        entries[num_entries].data = tga_buf;
        entries[num_entries].size = tga_size;
        num_entries++;
    }

    {  // baked UI shaders, no download
        const UINT UISZ = 512;          // POT
        const float RAD = 112.0f;       // corner radius
        struct { uint8_t* buf; size_t size; bool ok; } ui[NUM_UI_SHADERS] = {};
        ui[UI_PANEL].ok = make_gradient_panel_tga(
            UISZ, RAD, 252, 0x12, 0x12, 0x12, 255, 0x00, 0x00, 0x00, 28,
            &ui[UI_PANEL].buf, &ui[UI_PANEL].size);
        ui[UI_CARD].ok = make_gradient_panel_tga(
            UISZ, RAD, 246, 0x1E, 0x1E, 0x21, 250, 0x07, 0x07, 0x09, 30,
            &ui[UI_CARD].buf, &ui[UI_CARD].size);
        ui[UI_CHIP_BLUE].ok = make_gradient_panel_tga(
            UISZ, RAD, 255, 0x6A, 0xA8, 0xFF, 255, 0x2E, 0x6E, 0xE6, 0,
            &ui[UI_CHIP_BLUE].buf, &ui[UI_CHIP_BLUE].size);
        ui[UI_CHIP_RED].ok = make_gradient_panel_tga(
            UISZ, RAD, 255, 0xFF, 0x6E, 0x66, 255, 0xD8, 0x3A, 0x32, 0,
            &ui[UI_CHIP_RED].buf, &ui[UI_CHIP_RED].size);
        ui[UI_SHADOW].ok = make_soft_shadow_tga(
            UISZ, RAD, 200, &ui[UI_SHADOW].buf, &ui[UI_SHADOW].size);
        ui[UI_SCRIM].ok = make_scrim_tga(
            256, 256, &ui[UI_SCRIM].buf, &ui[UI_SCRIM].size);
        ui[UI_GLOW].ok = make_glow_tga(
            256, &ui[UI_GLOW].buf, &ui[UI_GLOW].size);
        ui[UI_DOT].ok = make_gradient_panel_tga(
            64, 31.0f, 255, 255, 255, 255, 255, 235, 235, 235, 0,
            &ui[UI_DOT].buf, &ui[UI_DOT].size);
        ui[UI_BOX].ok = make_gradient_panel_tga(  // radius 0: timer + scores box
            UISZ, 0.0f, 252, 0x16, 0x16, 0x16, 255, 0x00, 0x00, 0x00, 0,
            &ui[UI_BOX].buf, &ui[UI_BOX].size);
        ui[UI_FRAME].ok = make_rounded_frame_tga(
            UISZ, 0.0f, 26.0f, 255, 255, 255, 255,
            &ui[UI_FRAME].buf, &ui[UI_FRAME].size);
        ui[UI_TRI].ok = make_triangle_tga(64, &ui[UI_TRI].buf, &ui[UI_TRI].size);

        for (int i = 0; i < NUM_UI_SHADERS; i++) {
            if (!ui[i].ok) {
                logger::logf("avatar_overlay: UI gen %s failed", g_ui_tga_names[i]);
                continue;
            }
            snprintf(entries[num_entries].name, sizeof(entries[num_entries].name),
                     "%s", g_ui_tga_names[i]);
            entries[num_entries].data = ui[i].buf;
            entries[num_entries].size = ui[i].size;
            num_entries++;
        }
    }

    for (int i = 0; i < NUM_GLYPHS; i++) {  // glyphs, POT 128x256
        uint8_t* tga_buf = nullptr;
        size_t   tga_size = 0;
        if (!make_glyph_tga(g_glyph_chars[i], 128, 256, &tga_buf, &tga_size)) {
            logger::logf("avatar_overlay: glyph %d gen failed", i);
            continue;
        }
        glyph_name(i, entries[num_entries].name, sizeof(entries[num_entries].name), true);
        entries[num_entries].data = tga_buf;
        entries[num_entries].size = tga_size;
        num_entries++;
    }

    for (int i = 0; i < NUM_NAMES; i++) {  // names
        uint8_t* tga_buf = nullptr;
        size_t   tga_size = 0;
        if (!make_text_tga(g_name_strings[i], 512, 128, 0.62f, &tga_buf, &tga_size)) {
            logger::logf("avatar_overlay: name %d gen failed", i);
            continue;
        }
        player_name_tex(i, entries[num_entries].name, sizeof(entries[num_entries].name), true);
        entries[num_entries].data = tga_buf;
        entries[num_entries].size = tga_size;
        num_entries++;
    }

    for (int i = 0; i < NUM_HUD; i++) {  // hud bars from main/hud/
        char png[MAX_PATH];
        if (!get_hud_png_path(g_hud_variants[i], png, sizeof(png))) continue;
        uint8_t* tga_buf = nullptr;
        size_t   tga_size = 0;
        if (!png_to_pot_tga(png, (uint32_t)HUD_CANVAS_W, (uint32_t)HUD_CANVAS_H,
                            &tga_buf, &tga_size)) {
            logger::logf("avatar_overlay: hud '%s' load failed (%s)", g_hud_variants[i], png);
            continue;
        }
        snprintf(entries[num_entries].name, sizeof(entries[num_entries].name),
                 "textures/cod1reloaded/hud_%s.tga", g_hud_variants[i]);
        entries[num_entries].data = tga_buf;
        entries[num_entries].size = tga_size;
        num_entries++;
    }

    if (num_entries > 0) {
        char pk3_path[MAX_PATH];
        if (get_pk3_path(pk3_path, sizeof(pk3_path))) {
            if (create_pk3_with_files(pk3_path, entries, num_entries)) {
                cache_hash_save();
                logger::logf("avatar_overlay: pk3 baked with %d/%d avatars at %s",
                             num_entries, NUM_AVATARS, pk3_path);
            } else {
                logger::logf("avatar_overlay: pk3 write failed");
            }
        }
    } else {
        logger::logf("avatar_overlay: no avatars downloaded - pk3 NOT written");
    }

    for (int i = 0; i < num_entries; i++) free(entries[i].data);
    return 0;
}

void avatar_overlay_prepare_pk3_blocking() {
    if (!g_avatar_overlay_config.enable) return;

    if (cache_hash_matches()) {
        logger::logf("avatar_overlay: pk3 cache hit (url-list hash matches)");
        return;
    }

    // async: can't run GDI+/WinINet under DllMain's loader lock (they spawn
    // threads -> deadlock). worker races FS_InitFilesystem; if it loses, pk3
    // shows next launch (cache-hit).
    HANDLE h = CreateThread(NULL, 0, prepare_pk3_worker, NULL, 0, NULL);
    if (h) {
        CloseHandle(h);
        logger::logf("avatar_overlay: pk3 cache miss, async worker spawned");
    } else {
        logger::logf("avatar_overlay: CreateThread failed (err=%lu)",
                     GetLastError());
    }
}

void avatar_overlay_show_test() {
    if (!g_avatar_overlay_config.enable) return;
    if (g_setup_done) return;  // once per process

    HMODULE cgame = GetModuleHandleA("cgame_mp_x86.dll");
    if (!cgame) {
        logger::logf("avatar_overlay: cgame not loaded, cannot install hook");
        return;
    }
    if (!engine_2d_install_hook(cgame)) return;
    engine_2d_set_post_hud_callback(on_post_hud_draw);

    if (engine_2d_install_register_hook(cgame)) {
        engine_2d_set_post_register_callback(on_post_register_shaders);
    }

    g_setup_done = true;
    logger::logf("avatar_overlay: hooks installed - avatar will register on "
                 "next CG_Init (map load) and draw at (%d,%d) %dx%d",
                 g_avatar_overlay_config.x, g_avatar_overlay_config.y,
                 g_avatar_overlay_config.width, g_avatar_overlay_config.height);
}

void avatar_overlay_shutdown() {
    if (g_gdiplus_initialized) {
        Gdiplus::GdiplusShutdown(g_gdiplus_token);
        g_gdiplus_initialized = false;
    }
}

}  // namespace patches
