// minimap_preview - renders the minimap (src/ui/minimap.cpp) offline to PNGs with the very
// drawing code the DLL runs: a hidden OpenGL window, an offscreen framebuffer, a game
// screenshot as background, the map's BSP (or a minimaps\<map>.png pack) from folders.
//
//     minimap_preview <scene.txt>
//
// scene.txt, one directive per line ('#' comments), run in order:
//     size 1920 1080                  window pixels
//     background C:/.../shot.jpg      drawn "cover" behind the HUD
//     files C:/.../extracted          root of the game files (maps/mp/<map>.bsp)
//     root C:/.../game                folder holding minimaps\<map>.png + .json ("" = none)
//     map mp_carentan                 the map on screen
//     cvar <name> <value>             a cvar (cod1x_minimap_shape 1, ...)
//     view <x> <y> <yaw> [fov]        the player's view (world units, degrees)
//     mate <x> <y> <yaw> [team]       a teammate in the snapshot (clear: `mates 0`)
//     allowed 0|1|2                   the server's rule
//     stretch 1.333                   the display widens the picture (4:3 stretched on 16:9)
//     shot out.png                    draws and writes a picture
//     radar out.png                   writes the radar texture itself (the BSP drawing)
#include <windows.h>
#include <GL/gl.h>
#include <gdiplus.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ui/gl_overlay.h"
#include "ui/ui_draw.h"
#include "ui/minimap.h"
#include "ui/radar_map.h"

// ---- what the minimap needs from the rest of the DLL ----
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
HWND overlay_game_window() { return nullptr; }
bool ruleset_active() { return false; }
void hud_editor_open(bool) {}
void overlay_toggle(bool) {}
}  // namespace patches

namespace {

std::map<std::string, std::string> g_cvars;
std::string g_files, g_root, g_map;
float g_view[4] = { 0, 0, 90, 80 };
std::vector<patches::MinimapMate> g_mates;
int g_allowed = 2;
float g_stretch = 1.0f;

const char* p_cvar(const char* name) {
    auto it = g_cvars.find(name);
    return it == g_cvars.end() ? "" : it->second.c_str();
}
void p_reg(const char* name, const char* def) { if (!g_cvars.count(name)) g_cvars[name] = def; }
void p_command(const char* t) {
    char a[64], b[64];
    if (sscanf(t, "seta %63s %63s", a, b) == 2) g_cvars[a] = b;
}
int p_read(const char* path, void** buf) {
    *buf = nullptr;
    const std::string full = g_files + "/" + path;
    FILE* f = fopen(full.c_str(), "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* p = malloc(n > 0 ? n : 1);
    const size_t got = fread(p, 1, n, f);
    fclose(f);
    *buf = p;
    return (int)got;
}
void p_free(void* b) { free(b); }
const char* p_map() { return g_map.c_str(); }
bool p_view(float org[3], float* yaw, float* fov) {
    org[0] = g_view[0]; org[1] = g_view[1]; org[2] = 0;
    *yaw = g_view[2]; *fov = g_view[3];
    return true;
}
int p_mates(patches::MinimapMate* out, int max) {
    int n = 0;
    for (const auto& m : g_mates) if (n < max) out[n++] = m;
    return n;
}
int p_allowed() { return g_allowed; }
bool p_hidden() { return false; }
float p_stretch(float, float) { return g_stretch; }
const char* p_root() { return g_root.c_str(); }

typedef void (APIENTRY* GenFB)(GLsizei, GLuint*);
typedef void (APIENTRY* BindFB)(GLenum, GLuint);
typedef void (APIENTRY* StorageRB)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY* AttachRB)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY* StatusFB)(GLenum);

bool save_png(const char* path, int w, int h, const unsigned char* rgba, bool bottom_up, bool keep_alpha) {
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    Gdiplus::BitmapData bd;
    Gdiplus::Rect rc(0, 0, w, h);
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    for (int y = 0; y < h; ++y) {
        const unsigned char* s = rgba + (size_t)(bottom_up ? h - 1 - y : y) * w * 4;
        unsigned char* d = (unsigned char*)bd.Scan0 + (size_t)y * bd.Stride;
        for (int x = 0; x < w; ++x) {
            d[x * 4 + 0] = s[x * 4 + 2]; d[x * 4 + 1] = s[x * 4 + 1]; d[x * 4 + 2] = s[x * 4 + 0];
            d[x * 4 + 3] = keep_alpha ? s[x * 4 + 3] : 255;
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
            return bmp.Save(wpath, &codecs[i].Clsid, nullptr) == Gdiplus::Ok;
        }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: minimap_preview <scene.txt>\n"); return 2; }
    int W = 1920, H = 1080;
    std::string background;

    Gdiplus::GdiplusStartupInput gin;
    ULONG_PTR gtok = 0;
    Gdiplus::GdiplusStartup(&gtok, &gin, nullptr);
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "minimap_preview";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("minimap_preview", "minimap_preview", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
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
    int fbw = 0, fbh = 0;
    auto make_fb = [&]() {
        if (fbw == W && fbh == H) return true;
        if (!fb) { genFB(1, &fb); genRB(1, &cb); }
        bindFB(0x8D40, fb); bindRB(0x8D41, cb);
        storRB(0x8D41, 0x8058 /* GL_RGBA8 */, W, H);
        attRB(0x8D40, 0x8CE0, 0x8D41, cb);
        fbw = W; fbh = H;
        return status(0x8D40) == 0x8CD5;
    };

    patches::MinimapIO io = { p_cvar, p_reg, p_command, p_read, p_free, p_map, p_view, p_mates,
                              p_allowed, p_hidden, p_stretch, p_root, true };
    patches::minimap_set_io(&io);

    FILE* sf = fopen(argv[1], "rb");
    if (!sf) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    char line[8192];
    int rc_out = 0;
    while (fgets(line, sizeof(line), sf)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        char key[32] = "";
        int off = 0;
        if (sscanf(line, "%31s %n", key, &off) < 1) continue;
        const char* rest = line + off;
        if (!strcmp(key, "size")) sscanf(rest, "%d %d", &W, &H);
        else if (!strcmp(key, "background")) background = rest;
        else if (!strcmp(key, "files")) g_files = rest;
        else if (!strcmp(key, "root")) g_root = rest;
        else if (!strcmp(key, "map")) g_map = rest;
        else if (!strcmp(key, "stretch")) g_stretch = (float)atof(rest);
        else if (!strcmp(key, "allowed")) g_allowed = atoi(rest);
        else if (!strcmp(key, "view")) sscanf(rest, "%f %f %f %f", &g_view[0], &g_view[1], &g_view[2], &g_view[3]);
        else if (!strcmp(key, "mates")) g_mates.clear();
        else if (!strcmp(key, "mate")) {
            patches::MinimapMate m = { 0, 0, 0, 2 };
            sscanf(rest, "%f %f %f %d", &m.x, &m.y, &m.yaw, &m.team);
            g_mates.push_back(m);
        } else if (!strcmp(key, "cvar")) {
            char name[128] = "";
            int o2 = 0;
            if (sscanf(rest, "%127s %n", name, &o2) >= 1) g_cvars[name] = rest + o2;
        } else if (!strcmp(key, "radar")) {
            const std::string path = "maps/mp/" + g_map + ".bsp";
            void* buf = nullptr;
            const int len = p_read(path.c_str(), &buf);
            patches::RadarMap m;
            const DWORD t0 = GetTickCount();
            if (len > 0 && patches::radar_map_build_detail((const unsigned char*)buf, len, 3.0f, 2048, 13.0f, m)) {
                printf("radar %s: %dx%d, world %.0f x %.0f, %lu ms\n", g_map.c_str(), m.w, m.h, m.span_x, m.span_y, GetTickCount() - t0);
                save_png(rest, m.w, m.h, m.rgba.data(), false, true);
            } else {
                printf("radar %s FAILED: %s\n", g_map.c_str(), m.error.c_str());
                rc_out = 1;
            }
            free(buf);
        } else if (!strcmp(key, "shot")) {
            if (!make_fb()) { fprintf(stderr, "framebuffer incomplete\n"); return 1; }
            Sleep(220);                                   // the layout cvars are polled every 200 ms
            for (int f = 0; f < 3; ++f) {
                patches::minimap_frame();
                glViewport(0, 0, W, H);
                glClearColor(0.2f, 0.22f, 0.25f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                patches::ui_frame_dt(1.0f / 60);
                patches::ui_begin_2d(W, H);
                if (!background.empty()) patches::ui_image_cover(0, 0, (float)W, (float)H, background.c_str());
                if (patches::minimap_active()) patches::minimap_draw((float)W, (float)H);
                patches::ui_alpha(1.0f);
                patches::ui_end_2d();
                glFinish();
            }
            std::vector<unsigned char> px((size_t)W * H * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            const bool ok = save_png(rest, W, H, px.data(), true, false);
            printf("%s %s (%dx%d) source=%s\n", ok ? "wrote" : "FAILED", rest, W, H, patches::minimap_source());
            if (!ok) rc_out = 1;
        }
    }
    fclose(sf);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(wnd, dc);
    DestroyWindow(wnd);
    Gdiplus::GdiplusShutdown(gtok);
    return rc_out;
}
