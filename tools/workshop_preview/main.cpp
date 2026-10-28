// workshop_preview - renders the Workshop tab (src/ui/workshop_tab.cpp) offline to a PNG, with
// the drawing code the DLL runs, against a real catalog: the backend (features/workshop.cpp)
// loads it and installs into the exe's folder, as it would into the game's.
//
//     workshop_preview <catalog url> <out.png> [width height] [--install] [--mouse x y]
//
// The rail and the title around the tab are a plain copy of modern_menu_draw's layout (no
// rail icons): only the tab body is the shipped code. --install clicks DOWNLOAD on the
// selected item, writes <out>.progress.png during the transfer, then out.png once it is done.
#include <windows.h>
#include <GL/gl.h>
#include <gdiplus.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ui/gl_overlay.h"
#include "ui/ui_draw.h"
#include "ui/workshop_tab.h"
#include "features/workshop.h"

// ---- what the tab needs from the rest of the DLL ----
namespace logger {
void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
}  // namespace logger
namespace patches {
UiInput g_in;
const UiInput& ui_input() { return g_in; }
bool ui_menu_visible(const char*) { return true; }
HWND overlay_game_window() { return nullptr; }
void ui_menu_hide(const char*) {}
}  // namespace patches

namespace {

typedef void (APIENTRY* GenFB)(GLsizei, GLuint*);
typedef void (APIENTRY* BindFB)(GLenum, GLuint);
typedef void (APIENTRY* StorageRB)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY* AttachRB)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY* StatusFB)(GLenum);

bool save_png(const char* path, int w, int h) {
    std::vector<unsigned char> px((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    Gdiplus::BitmapData bd;
    Gdiplus::Rect rc(0, 0, w, h);
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    for (int y = 0; y < h; ++y) {
        const unsigned char* s = &px[(size_t)(h - 1 - y) * w * 4];
        unsigned char* d = (unsigned char*)bd.Scan0 + (size_t)y * bd.Stride;
        for (int x = 0; x < w; ++x) {
            d[x * 4 + 0] = s[x * 4 + 2]; d[x * 4 + 1] = s[x * 4 + 1]; d[x * 4 + 2] = s[x * 4 + 0];
            d[x * 4 + 3] = 255;
        }
    }
    bmp.UnlockBits(&bd);
    UINT n = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&n, &size);
    std::vector<unsigned char> buf(size);
    Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(n, size, codecs);
    for (UINT i = 0; i < n; ++i)
        if (!wcscmp(codecs[i].MimeType, L"image/png")) {
            wchar_t wpath[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
            const bool ok = bmp.Save(wpath, &codecs[i].Clsid, nullptr) == Gdiplus::Ok;
            printf("%s %s (%dx%d)\n", ok ? "wrote" : "FAILED", path, w, h);
            return ok;
        }
    return false;
}

// modern_menu_draw's frame: rail (280 px, pill on Workshop) + title, then the tab body
void draw_menu(float sw, float sh) {
    using namespace patches;
    ui_rect(0, 0, sw, sh, UI_BG);
    const float rail = 280;
    ui_rect(0, 0, rail, sh, UI_PANEL);
    ui_rect(rail, 0, 1, sh, 0xFF2A2A2A);
    ui_text(36, 40, 36, 600, UI_TEXT, "1.6X");
    ui_text(38, 88, 11, 600, UI_MUTED, "C O D 1  R E L O A D E D");
    ui_rect(36, 118, rail - 72, 1, 0xFF232323);
    static const char* tabs[] = { "Display", "Mouse", "Keys", "Game", "Inspect", "Files", "Demos", "Workshop", "About" };
    const int sel = 7;
    ui_rect_rounded(20, 150 + sel * 54 - 6, rail - 40, 46, 10, UI_ACCENT);
    for (int i = 0; i < 9; ++i)
        ui_text(72, 150 + i * 54 + 4, 18, 600, i == sel ? 0xFF0A0A0A : UI_MUTED, tabs[i]);
    const float pw = sw - rail, px = rail + 24, py = 40;
    ui_text(px + 36, py + 10, 30, 600, UI_TEXT, "Workshop");
    ui_text(px + 36, py + 52, 14, 400, UI_MUTED, "Maps and mods from the 1.6X catalog. One click: downloaded, verified, installed.");
    ui_rect(px + 36, py + 84, pw - 120, 1, 0xFF2A2A2A);
    workshop_tab_draw(px + 36, py + 122, pw - 120, sh - 36);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: workshop_preview <catalog url> <out.png> [w h] [--install] [--mouse x y]\n"); return 2; }
    int W = 1920, H = 1080;
    bool do_install = false;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--install")) do_install = true;
        else if (!strcmp(argv[i], "--mouse") && i + 2 < argc) { patches::g_in.mx = (float)atof(argv[i + 1]); patches::g_in.my = (float)atof(argv[i + 2]); i += 2; }
        else if (i + 1 < argc && atoi(argv[i]) > 0) { W = atoi(argv[i]); H = atoi(argv[i + 1]); ++i; }
    }
    snprintf(patches::g_workshop_config.manifest_url, sizeof(patches::g_workshop_config.manifest_url), "%s", argv[1]);

    Gdiplus::GdiplusStartupInput gin;
    ULONG_PTR gtok = 0;
    Gdiplus::GdiplusStartup(&gtok, &gin, nullptr);
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "workshop_preview";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("workshop_preview", "workshop_preview", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                  PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0 };
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc);
    wglMakeCurrent(dc, rc);
    GenFB genFB = (GenFB)wglGetProcAddress("glGenFramebuffersEXT");
    BindFB bindFB = (BindFB)wglGetProcAddress("glBindFramebufferEXT");
    GenFB genRB = (GenFB)wglGetProcAddress("glGenRenderbuffersEXT");
    BindFB bindRB = (BindFB)wglGetProcAddress("glBindRenderbufferEXT");
    StorageRB storRB = (StorageRB)wglGetProcAddress("glRenderbufferStorageEXT");
    AttachRB attRB = (AttachRB)wglGetProcAddress("glFramebufferRenderbufferEXT");
    StatusFB status = (StatusFB)wglGetProcAddress("glCheckFramebufferStatusEXT");
    if (!genFB || !bindFB || !genRB || !bindRB || !storRB || !attRB || !status) { fprintf(stderr, "no FBO support\n"); return 1; }
    GLuint fb = 0, cb = 0;
    genFB(1, &fb); bindFB(0x8D40, fb);
    genRB(1, &cb); bindRB(0x8D41, cb);
    storRB(0x8D41, 0x8058 /* GL_RGBA8 */, W, H);
    attRB(0x8D40, 0x8CE0, 0x8D41, cb);
    if (status(0x8D40) != 0x8CD5) { fprintf(stderr, "framebuffer incomplete\n"); return 1; }

    auto frame = [&]() {
        glViewport(0, 0, W, H);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        patches::ui_frame_dt(1.0f / 60);
        patches::ui_begin_2d(W, H);
        patches::ui_alpha(1.0f);
        draw_menu((float)W, (float)H);
        patches::ui_end_2d();
        glFinish();
        Sleep(16);
    };
    // the first frame starts the load; wait for it (catalog, states, pictures)
    patches::WsCatalogInfo info;
    std::vector<patches::WsItem> items;
    frame();
    for (int t = 0; t < 30000; t += 16) {
        patches::workshop_snapshot(info, items);
        if (!info.busy) break;
        frame();
    }
    for (int f = 0; f < 40; ++f) frame();
    std::string out = argv[2];
    if (do_install && !items.empty()) {
        if (!save_png((out + ".before.png").c_str(), W, H)) return 1;
        patches::workshop_install(items[0].id);
        bool shot = false;
        for (int t = 0; t < 120000; t += 16) {
            frame();
            patches::workshop_snapshot(info, items);
            const patches::WsItem& it = items[0];
            if (!shot && it.job == patches::WSJ_DOWNLOADING && it.progress > 0.4f) {
                frame();                       // a frame drawn with this state
                save_png((out + ".progress.png").c_str(), W, H);
                shot = true;
            }
            if (!patches::workshop_busy() && (it.job == patches::WSJ_DONE || it.job == patches::WSJ_ERROR)) break;
        }
        for (int f = 0; f < 40; ++f) frame();
    }
    const bool ok = save_png(out.c_str(), W, H);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(wnd, dc);
    DestroyWindow(wnd);
    Gdiplus::GdiplusShutdown(gtok);
    return ok ? 0 : 1;
}
