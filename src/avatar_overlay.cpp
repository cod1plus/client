// Avatar overlay (phase 2).
//
// Phase 1 used a Win32 overlay window which got occluded by the OpenGL
// surface in-game. Phase 2 draws NATIVELY inside the engine's renderer
// via the engine_2d module (hook on CG_Draw2D + RE_DrawStretchPic). The
// avatar appears as a real part of the HUD : correct z-order, scales
// with resolution, alpha-blended through OpenGL.
//
// Pipeline :
//   1) Download PNG via WinINet -> main/textures/cod1reloaded/avatar_*.png
//   2) Decode PNG via GDI+ -> raw BGRA pixels
//   3) Save as 32-bit BGRA TGA -> main/textures/cod1reloaded/avatar_*.tga
//   4) On first frame after hook: call R_RegisterShader("textures/cod1reloaded/avatar_*")
//      The engine resolves the TGA file, uploads to GPU, returns handle.
//   5) Every frame after vanilla HUD: RE_DrawStretchPic at the configured rect.

#include "avatar_overlay.h"
#include "engine_2d.h"
#include "window_patch.h"
#include "logger.h"

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

// 10-slot avatar URL list. Index 0-4 = allied team, 5-9 = axis team.
// Phase 3 will replace this with a per-server feed (server pushes URLs via
// configstring or a side-channel HTTP call).
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

// UI element shaders : composants CS-style (panneau arrondi + anneaux
// d'equipe) generes via GDI+ et bakes dans le meme pk3 que les avatars.
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

// Glyphes du timer / scores : 0-9 + ":" (11 textures), dessines via GDI+
// avec une police moderne. Index 0..9 = chiffre, index 10 = colon.
constexpr int NUM_GLYPHS = 11;
constexpr int COLON_IDX  = 10;
static const wchar_t* const g_glyph_chars[NUM_GLYPHS] = {
    L"0", L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9", L":"
};

// Noms des joueurs (faux pour l'instant ; rendus en textures via GDI+).
constexpr int NUM_NAMES = NUM_AVATARS;
static const wchar_t* const g_name_strings[NUM_NAMES] = {
    L"JevierPotte", L"Quendrell", L"T30 Elo", L"LuckyLuc", L"SNELELE",
    L"UnitedG", L"Kimsa", L"CoCo", L"Hades", L"Fenubis"
};

// HUD du haut : template "scoreboard-clean" (slots avatars + box scores),
// overlay (avatars + chiffres) par-dessus.
constexpr int NUM_HUD = 1;
static const char* const g_hud_variants[NUM_HUD] = { "scoreboard" };
// Dimensions de la source PNG (pour calcul des UV dans le canvas POT).
constexpr float HUD_PNG_W = 682.0f;
constexpr float HUD_PNG_H = 98.0f;
constexpr float HUD_CANVAS_W = 1024.0f;  // POT
constexpr float HUD_CANVAS_H = 128.0f;   // POT

// Max entries pk3 = avatars + UI + glyphes + noms + hud.
constexpr int MAX_PK3_ENTRIES =
    NUM_AVATARS + NUM_UI_SHADERS + NUM_GLYPHS + NUM_NAMES + NUM_HUD;

namespace {

// Handles, filled by on_post_register_shaders.
qhandle_t g_ui_handles[NUM_UI_SHADERS] = {0};
qhandle_t g_glyph_handles[NUM_GLYPHS]  = {0};
qhandle_t g_name_handles[NUM_NAMES]    = {0};
qhandle_t g_hud_handles[NUM_HUD]       = {0};

// Chemin de la source PNG d'un etat HUD : <jeu>/main/hud/hud-<variant>.png
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

// Construit le nom virtuel d'un glyphe (avec ou sans extension .tga).
void glyph_name(int i, char* out, size_t n, bool with_ext) {
    if (i == COLON_IDX)
        snprintf(out, n, "textures/cod1reloaded/glyph_colon%s", with_ext ? ".tga" : "");
    else
        snprintf(out, n, "textures/cod1reloaded/glyph_%d%s", i, with_ext ? ".tga" : "");
}

// Nom virtuel d'une texture de nom de joueur.
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

// Standard CRC32 (poly 0xEDB88320, init 0xFFFFFFFF, xorout 0xFFFFFFFF).
// Required for the ZIP/PK3 file format.
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

// 32-bit BGRA TGA header. CoD1's renderer reads TGA natively.
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
    uint8_t* data;       // raw file bytes (caller frees after pk3 is written)
    size_t   size;
};

// Multi-file uncompressed ZIP/PK3. Same on-disk format as single-file
// version, just iterates over the entries. CoD1's pak loader handles
// arbitrary entry counts.
bool create_pk3_with_files(const char* pk3_path, const Pk3Entry* entries, int count) {
    if (count <= 0) return false;
    FILE* f = fopen(pk3_path, "wb");
    if (!f) return false;

    auto put32 = [&](uint32_t v) { fwrite(&v, 1, 4, f); };
    auto put16 = [&](uint16_t v) { fwrite(&v, 1, 2, f); };

    uint32_t local_offsets[MAX_PK3_ENTRIES] = {0};
    uint32_t crcs[MAX_PK3_ENTRIES]          = {0};

    // Local file headers
    for (int i = 0; i < count; i++) {
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

    // Central directory entries
    for (int i = 0; i < count; i++) {
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

    // EOCD
    put32(0x06054b50);
    put16(0); put16(0);
    put16((uint16_t)count);
    put16((uint16_t)count);
    put32(central_size);
    put32(central_offset);
    put16(0);

    fclose(f);
    return true;
}

// ---- GDI+ UI asset baker (CS-style dark glass) -------------------------
//
// On dessine les composants "modernes" (coins arrondis, anti-aliasing,
// alpha) dans des bitmaps GDI+, puis on les exporte en TGA BGRA. Le moteur
// ne fait que compositer ces quads -> son age n'a aucune importance.

// Construit un GraphicsPath rectangle a coins arrondis.
void add_rounded_rect(Gdiplus::GraphicsPath& path,
                      float x, float y, float w, float h, float r) {
    path.Reset();
    if (r <= 0.5f) {  // coins carrés (sharp)
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

// Verrouille un bitmap GDI+ 32bpp et l'ecrit en TGA BGRA top-down (malloc).
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

// Panneau rempli a coins arrondis (fond translucide). Pour la boite centrale
// et les cartes joueur. Dessine en 9-slice in-game pour scaler proprement.
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

// Anneau a coins arrondis (contour seul, centre transparent). Pour le cadre
// de couleur d'equipe autour des avatars.
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

// Panneau a coins arrondis avec DEGRADE vertical (glassmorphism) + liseré
// interne lumineux (le "bord de verre"). C'est ce qui fait le rendu propre.
bool make_gradient_panel_tga(UINT size, float radius,
                             uint8_t a1, uint8_t r1, uint8_t g1, uint8_t b1,  // haut
                             uint8_t a2, uint8_t r2, uint8_t g2, uint8_t b2,  // bas
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
        // Reflet specular en haut (verre éclairé du dessus) - subtil, propre.
        {
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
            // liseré fin (hairline) qui definit le bord proprement
            Gdiplus::Pen rim(Gdiplus::Color(rim_a, 255, 255, 255), 1.25f);
            gfx.DrawPath(&rim, &path);
        }
    }
    return bitmap_to_tga_memory(bmp, out_buf, out_size);
}

// Ombre portée SOFT qui suit la forme du rounded-rect (pas un halo radial).
// On empile N rounded-rects translucides du plus grand (bord, faible alpha)
// au plus petit (centre) -> degrade doux qui epouse la forme = vraie ombre.
bool make_soft_shadow_tga(UINT size, float radius,
                          uint8_t center_a,
                          uint8_t** out_buf, size_t* out_size) {
    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics gfx(&bmp);
        gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const int   rings     = 22;
        const float inset_max = (float)size * 0.22f;   // marge de flou
        // alpha par couche : l'empilement converge vers ~center_a au centre.
        const float per = (float)center_a / (float)rings * 1.35f;
        for (int i = 0; i < rings; i++) {
            const float f = (float)i / (float)(rings - 1);   // 0 bord -> 1 centre
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

// Choisit la meilleure police UI dispo (moderne, condensee). Caller delete.
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

// Dessine un glyphe (chiffre ou ":") centre dans une cellule POT, blanc,
// anti-aliase, fond transparent (rendu vectoriel = alpha propre). Blanc ->
// recolorable via RE_SetColor.
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

// Rend une chaine (nom de joueur) centree dans une cellule POT, blanc,
// vectoriel. font_frac = hauteur de police en fraction de cell_h.
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

// Scrim : degrade vertical sombre (haut) -> transparent (bas), plein rect.
// Assoit le HUD dans la scene (look broadcast).
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

// Glow radial blanc (centre opaque -> bords transparents). Teinte au draw
// via RE_SetColor pour un halo de couleur d'accent.
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

// Petit triangle plein pointant vers le HAUT, blanc (teinte au draw).
// Marqueur devant les sous-scores (look de la ref).
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

// ------------------------------------------------------------------------

// Decode image via GDI+ (PNG/JPG/GIF/etc), resize to POT 1024x1024, and
// write 32-bit BGRA TGA to a heap-allocated buffer. Caller frees.
// Display is ~76 px on 1080p so 1024 source = ~13x oversample per axis,
// trilinear mipmaps give a very crisp downsample.
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
        g.Clear(Gdiplus::Color(0, 0, 0, 0));   // base transparente
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // Avatars CARRES (coins droits) : on dessine l'image entiere.
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

// Charge un PNG (barre HUD) et le place a (0,0) dans un canvas POT
// transparent (cw x ch), a sa taille native. Le draw in-game utilisera des
// UV pour ne sampler que la zone image. POT obligatoire pour CoD1.
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
        g.DrawImage(&src, 0, 0, (INT)sw, (INT)sh);  // taille native, coin haut-gauche
    }
    return bitmap_to_tga_memory(dst, out_buf, out_size);
}

// Generate a solid-color 64x64 (POT) 32-bit BGRA TGA in memory. Caller frees.
// Used for the UI element shaders (team frames, central box).
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
        *p++ = b;  // TGA truecolor is BGRA
        *p++ = g;
        *p++ = r;
        *p++ = a;
    }
    *out_buf  = buf;
    *out_size = buf_size;
    return true;
}

// Single-file pk3 helper kept for backward compat / future single-shader use.
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

    // Local file header (30 bytes + filename)
    put32(0x04034b50);  // signature
    put16(20);          // version needed
    put16(0);           // flags
    put16(0);           // compression = STORE
    put16(0);           // mod time
    put16(0);           // mod date
    put32(crc);
    put32((uint32_t)file_size);  // compressed
    put32((uint32_t)file_size);  // uncompressed
    put16(name_len);
    put16(0);           // extra length
    fwrite(internal_name, 1, name_len, f);
    fwrite(file_data, 1, file_size, f);

    const uint32_t central_offset = (uint32_t)ftell(f);

    // Central directory entry (46 bytes + filename)
    put32(0x02014b50);
    put16(20);          // version made by
    put16(20);          // version needed
    put16(0);           // flags
    put16(0);           // compression
    put16(0);           // mod time
    put16(0);           // mod date
    put32(crc);
    put32((uint32_t)file_size);
    put32((uint32_t)file_size);
    put16(name_len);
    put16(0);           // extra
    put16(0);           // comment
    put16(0);           // disk
    put16(0);           // internal attrs
    put32(0);           // external attrs
    put32(local_offset);
    fwrite(internal_name, 1, name_len, f);

    const uint32_t central_size = (uint32_t)ftell(f) - central_offset;

    // End of Central Directory Record (22 bytes)
    put32(0x06054b50);
    put16(0);                   // disk number
    put16(0);                   // disk with central
    put16(1);                   // entries on this disk
    put16(1);                   // total entries
    put32(central_size);
    put32(central_offset);
    put16(0);                   // comment length

    fclose(f);
    return true;
}

// Decode PNG via GDI+, RESIZE TO 128x128 POWER-OF-2 (CoD1's OpenGL renderer
// rejects non-POT textures), then write 32-bit BGRA TGA on disk.
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

    // Force POT 128x128 - high enough to recognize faces at the on-screen
    // display size (48x48). CoD1's OpenGL renderer requires POT textures.
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
    hdr.imageType       = 2;        // uncompressed truecolor
    hdr.width           = (uint16_t)TGT;
    hdr.height          = (uint16_t)TGT;
    hdr.pixelDepth      = 32;
    hdr.imageDescriptor = 0x28;     // bit 5 = top-down rows, bits 0-3 = alpha bits
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

// Resolve <CoD1>/main/textures/cod1reloaded/ - creates dirs if missing.
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

// Our custom shader handles, one per avatar slot. Set by the post-register
// callback after the engine has bootstrapped its own shader table.
static qhandle_t g_avatar_shaders[NUM_AVATARS] = {0};
static int       g_avatar_shaders_count = 0;

// Diagnostic shader handles for the test grid (read from cgame globals).
struct ShaderProbe {
    qhandle_t  handle;
    const char* label;
};
static ShaderProbe g_probes[6] = {};
static int         g_probe_count = 0;

// Called by engine_2d once CG_RegisterCgameShaders has finished. We register
// each of the NUM_AVATARS slots packed inside our pk3.
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

    // UI element shaders (panneau + anneaux d'equipe).
    for (int i = 0; i < NUM_UI_SHADERS; i++) {
        g_ui_handles[i] = engine_2d_register_shader(g_ui_shader_names[i], 5);
    }
    logger::logf("avatar_overlay: UI shaders panel=%d card=%d chipB=%d shadow=%d scrim=%d glow=%d dot=%d",
                 g_ui_handles[UI_PANEL], g_ui_handles[UI_CARD], g_ui_handles[UI_CHIP_BLUE],
                 g_ui_handles[UI_SHADOW], g_ui_handles[UI_SCRIM], g_ui_handles[UI_GLOW],
                 g_ui_handles[UI_DOT]);

    // Glyphes du timer / scores.
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

    // Noms des joueurs.
    int names_ok = 0;
    for (int i = 0; i < NUM_NAMES; i++) {
        char name[64];
        player_name_tex(i, name, sizeof(name), false);
        g_name_handles[i] = engine_2d_register_shader(name, 5);
        if (g_name_handles[i] > 0) names_ok++;
    }
    logger::logf("avatar_overlay: names registered %d/%d", names_ok, NUM_NAMES);

    // Barres HUD designees.
    for (int i = 0; i < NUM_HUD; i++) {
        char nm[80];
        snprintf(nm, sizeof(nm), "textures/cod1reloaded/hud_%s", g_hud_variants[i]);
        g_hud_handles[i] = engine_2d_register_shader(nm, 5);
    }
    logger::logf("avatar_overlay: hud bar registered = %d", g_hud_handles[0]);
}

// --- Animation : horloge QPC + easing (style CS2) -----------------------
double g_anim_start = -1.0;  // instant de demarrage de l'anim d'entree

double overlay_clock_sec() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return freq.QuadPart ? (double)c.QuadPart / (double)freq.QuadPart : 0.0;
}
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }
// Deceleration douce (fin de course).
float ease_out_cubic(float x) { float u = 1.0f - x; return 1.0f - u * u * u; }
// Depassement leger facon ressort (le "pop" new-gen).
float ease_out_back(float x) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float u = x - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

// Dessine une texture en 9-slice : 4 coins a taille fixe, 4 bords etires
// sur un axe, centre etire sur les deux. Les coins arrondis ne se deforment
// JAMAIS quelle que soit la taille du panneau. uv_corner = fraction [0..0.5]
// du coin dans la texture source ; dst_corner = taille du coin a l'ecran (px).
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

// Dessine un glyphe (idx 0..9 = chiffre, COLON_IDX = ":").
void draw_glyph(int idx, float x, float y, float w, float h) {
    if (idx < 0 || idx >= NUM_GLYPHS) return;
    if (g_glyph_handles[idx] > 0)
        engine_2d_draw_pic(x, y, w, h, g_glyph_handles[idx]);
}

// Dessine une chaine de chiffres/":" en monospace, centree horizontalement
// sur cx, top = bord haut. glyph_h = hauteur des glyphes ; l'avance entre
// glyphes = glyph_h * advance_ratio (les cellules sources font 64x128).
void draw_number(const char* s, float cx, float top, float glyph_h,
                 float advance_ratio) {
    const float advance  = glyph_h * advance_ratio;
    const float glyph_w  = glyph_h * 0.5f;   // cellule source 64x128 = ratio 0.5
    int n = 0;
    for (const char* p = s; *p; ++p) n++;
    const float total = n * advance;
    float x = cx - total * 0.5f;
    for (const char* p = s; *p; ++p) {
        int idx = (*p == ':') ? COLON_IDX
                : (*p >= '0' && *p <= '9') ? (*p - '0')
                : -1;
        if (idx >= 0) {
            // glyphe centre dans sa case d'avance
            draw_glyph(idx, x + (advance - glyph_w) * 0.5f, top, glyph_w, glyph_h);
        }
        x += advance;
    }
}

// Called by engine_2d each frame, AFTER the vanilla HUD has been drawn.
// Renderer's 2D state is set up, we can safely call DrawStretchPic.
void on_post_hud_draw() {
    if (!g_avatar_overlay_config.enable) return;

    // Read pre-registered shader handles from cgame globals (set by the
    // engine during CG_RegisterCgameShaders at init). RE_DrawStretchPic
    // with any valid handle proves the draw path works.
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

            // Build a probe list for visual identification of textures.
            // Only valid (>0) handles will be drawn.
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

    // Ready-up layout : 5 allied avatars + 5 axis avatars in a single row,
    // centered at the top of the actual game viewport.
    //
    // RE_DrawStretchPic takes PIXEL coordinates (not 640x480 virtual),
    // so we must compute the layout from the live viewport size. We get
    // that via GetClientRect on the game window (cached by window_patch).
    // Sizing/spacing scale with viewport height for consistency across
    // resolutions (1080p, 1440p, ultrawide, ...).
    if (g_avatar_shaders_count > 0) {
        HWND hwnd = get_game_window();
        RECT rc = {};
        float vid_w = 1440.0f, vid_h = 1080.0f;  // sensible fallback
        if (hwnd && GetClientRect(hwnd, &rc)) {
            vid_w = (float)(rc.right - rc.left);
            vid_h = (float)(rc.bottom - rc.top);
        }

        // === HUD du HAUT : template scoreboard-clean + overlay ===========
        // Le template fournit les 10 slots avatars (accents Axis/Allies) + le
        // module central (box timer + 2 box scores). On overlay les avatars
        // dans les slots et les chiffres dans les box.
        const float BW = vid_w * 0.50f;                  // largeur du HUD (compact)
        const float BH = BW * HUD_PNG_H / HUD_PNG_W;     // ratio 98/682
        const float bx = (vid_w - BW) * 0.5f;
        const float by = vid_h * 0.020f;

        // Fade-in global.
        const double now = overlay_clock_sec();
        if (g_anim_start < 0.0) g_anim_start = now;
        const float t = (float)(now - g_anim_start);
        const float a = ease_out_cubic(clampf(t / 0.5f, 0.0f, 1.0f));

        // Mapping coords image (826x111) -> ecran.
        auto SX = [&](float ix) { return bx + (ix / HUD_PNG_W) * BW; };
        auto SY = [&](float iy) { return by + (iy / HUD_PNG_H) * BH; };

        // 1) Template (chassis).
        if (g_hud_handles[0] > 0) {
            engine_2d_set_color(1.0f, 1.0f, 1.0f, a);
            engine_2d_draw_stretch_pic(bx, by, BW, BH, 0.0f, 0.0f,
                                       HUD_PNG_W / HUD_CANVAS_W,
                                       HUD_PNG_H / HUD_CANVAS_H, g_hud_handles[0]);
        }

        // 2) Avatars dans les 10 slots (cadres : interieur y34..80, inset 2px).
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

        // 3) Chiffres (timer LIVE + scores) dans les box centrales.
        // top = centre - 0.40*h : compense le biais vertical du glyphe (le
        // chiffre est legerement haut dans sa case a cause du jambage bas).
        auto num = [&](const char* s, float icx, float icy, float ih) {
            const float h = (ih / HUD_PNG_H) * BH;
            engine_2d_set_color(1.0f, 1.0f, 1.0f, a);
            // avance 0.40 : chiffres resserres (tabulaire), pas espaces.
            draw_number(s, SX(icx), SY(icy) - h * 0.40f, h, 0.40f);
        };
        const float frac = t - (float)(int)t;
        const float pop  = 1.0f + 0.06f * (1.0f - clampf(frac / 0.18f, 0.0f, 1.0f));
        int rem = 99 - ((int)t % 100);
        char timer[8];
        snprintf(timer, sizeof(timer), "%d:%02d", rem / 60, rem % 60);
        num(timer, 340.0f, 32.5f, 20.0f * pop);   // box timer (haut, plus gros)
        num("9",   315.0f, 63.5f, 30.0f);          // score gauche (Axis)
        num("11",  366.0f, 63.5f, 30.0f);          // score droite (Allies)

        // Remettre blanc opaque pour ne pas teinter le HUD vanilla suivant.
        engine_2d_reset_color();
    } else {
        // Diagnostic fallback while custom shader isn't ready : probe grid.
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

// Resolve <CoD1>/main/z_cod1reloaded.pk3 path.
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

// FNV-1a 32-bit hash of the URL list + a format version byte. Used as
// cache key : when ANY url changes (or we bump FORMAT_VERSION below
// because the pk3 layout / texture size changed), the hash differs and
// the pk3 is rebuilt.
//
// Bump FORMAT_VERSION whenever the pk3 contents/format changes (e.g.
// switching texture resolution from 32 to 128).
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

// Worker thread : downloads all N avatars, converts to 32x32 TGAs in memory,
// bundles them into a single multi-file pk3, and saves the URL-list hash
// for future cache validation.
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

    // UI element shaders (CS-style rounded panel + team rings), GDI+ baked.
    // Always generated, no download needed.
    {
        const UINT UISZ = 512;          // POT (obligatoire CoD1) + haute def
        const float RAD = 112.0f;       // rayon des coins (proportionnel a 512)
        struct { uint8_t* buf; size_t size; bool ok; } ui[NUM_UI_SHADERS] = {};
        // Panneau arrondi NOIR pur neutre (bord avatar). Pas de teinte bleue.
        ui[UI_PANEL].ok = make_gradient_panel_tga(
            UISZ, RAD, 252, 0x12, 0x12, 0x12, 255, 0x00, 0x00, 0x00, 28,
            &ui[UI_PANEL].buf, &ui[UI_PANEL].size);
        // Carte joueur : noir un poil plus clair, liseré fin.
        ui[UI_CARD].ok = make_gradient_panel_tga(
            UISZ, RAD, 246, 0x1E, 0x1E, 0x21, 250, 0x07, 0x07, 0x09, 30,
            &ui[UI_CARD].buf, &ui[UI_CARD].size);
        // Chips/accents : degrade couleur d'equipe (clair -> fonce).
        ui[UI_CHIP_BLUE].ok = make_gradient_panel_tga(
            UISZ, RAD, 255, 0x6A, 0xA8, 0xFF, 255, 0x2E, 0x6E, 0xE6, 0,
            &ui[UI_CHIP_BLUE].buf, &ui[UI_CHIP_BLUE].size);
        ui[UI_CHIP_RED].ok = make_gradient_panel_tga(
            UISZ, RAD, 255, 0xFF, 0x6E, 0x66, 255, 0xD8, 0x3A, 0x32, 0,
            &ui[UI_CHIP_RED].buf, &ui[UI_CHIP_RED].size);
        // Ombre douce (depth), plus marquee.
        ui[UI_SHADOW].ok = make_soft_shadow_tga(
            UISZ, RAD, 200, &ui[UI_SHADOW].buf, &ui[UI_SHADOW].size);
        // Scrim (degrade vertical) pour asseoir le HUD.
        ui[UI_SCRIM].ok = make_scrim_tga(
            256, 256, &ui[UI_SCRIM].buf, &ui[UI_SCRIM].size);
        // Glow radial blanc (teinte au draw).
        ui[UI_GLOW].ok = make_glow_tga(
            256, &ui[UI_GLOW].buf, &ui[UI_GLOW].size);
        // Pastille ronde (pips de round), blanche -> teintee au draw.
        ui[UI_DOT].ok = make_gradient_panel_tga(
            64, 31.0f, 255, 255, 255, 255, 255, 235, 235, 235, 0,
            &ui[UI_DOT].buf, &ui[UI_DOT].size);
        // Box NOIR pur a coins CARRES (radius 0) : timer + scores.
        ui[UI_BOX].ok = make_gradient_panel_tga(
            UISZ, 0.0f, 252, 0x16, 0x16, 0x16, 255, 0x00, 0x00, 0x00, 0,
            &ui[UI_BOX].buf, &ui[UI_BOX].size);
        // Cadre BLANC CARRE (anneau, coins droits) -> teinte au draw.
        ui[UI_FRAME].ok = make_rounded_frame_tga(
            UISZ, 0.0f, 26.0f, 255, 255, 255, 255,
            &ui[UI_FRAME].buf, &ui[UI_FRAME].size);
        // Petit triangle blanc -> teinte au draw (marqueur sous-scores).
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

    // Glyphes 0-9 + ":" (cellules POT 128x256), pour le timer et les scores.
    for (int i = 0; i < NUM_GLYPHS; i++) {
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

    // Noms des joueurs (cellules POT 256x64), rendus via GDI+.
    for (int i = 0; i < NUM_NAMES; i++) {
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

    // Barres HUD designees (PNG importes depuis <jeu>/main/hud/).
    for (int i = 0; i < NUM_HUD; i++) {
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

    // Cache check : the .pk3.hash sidecar stores the FNV-1a hash of the
    // current URL list. If it matches, the pk3 is still valid for these
    // exact URLs. Any URL change -> hash mismatch -> redownload.
    if (cache_hash_matches()) {
        logger::logf("avatar_overlay: pk3 cache hit (url-list hash matches)");
        return;
    }

    // No cache - spawn async worker. We CAN'T do GDI+ / WinINet here
    // synchronously because DllMain holds the loader lock and those APIs
    // create threads internally -> deadlock.
    //
    // Trade-off : the thread runs after DllMain returns. If it finishes
    // BEFORE the engine's FS_InitFilesystem (~100-300ms after main() runs),
    // pk3 is in the scan -> avatar shows from this launch. Otherwise the
    // pk3 lands too late and the avatar shows from the NEXT launch (where
    // it'll be cache-hit).
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
    if (g_setup_done) return;  // run once per process

    // pk3 was already baked at DllMain time by avatar_overlay_prepare_pk3_blocking().
    // The engine has scanned it during its own pak init, so R_RegisterShader
    // for "textures/cod1reloaded/avatar_test" will find it from the in-memory
    // pak table.
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
