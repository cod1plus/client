// hud_preview - renders the native streamer overlay (src/ui/streamer_hud.cpp) offline to a
// PNG, with the very drawing code the DLL runs in the game: a hidden OpenGL window, an
// offscreen framebuffer, a game screenshot as background, fake cvars for the PAM feed and
// the game files (BSP, weapon files, HUD icons) from a folder.
//
//     hud_preview <scene.txt> <out.png>
//
// scene.txt, one directive per line ('#' comments):
//     size 1920 1080                  window pixels
//     background C:/.../shot.jpg      drawn "cover" behind the overlay
//     files C:/.../extracted          root of the game files (maps/mp/x.bsp, weapons/mp/...)
//     cvar <name> <value...>          a cvar (the value is the rest of the line)
//     camera <fovx> <fovy> <x> <y> <z> <yaw> <pitch>   refdef for the XRAY
//     frames 40                       frames drawn before the capture (smoothing settles)
//     stretch 1.333                   the display widens the picture (4:3 stretched on 16:9)
//     view 1920 1080                  also write <out>.view.png: the capture resized as seen
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
#include "ui/streamer_hud.h"

// ---- what the overlay needs from the rest of the DLL ----
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
bool ui_menu_visible(const char*) { return true; }
HWND overlay_game_window() { return nullptr; }
void ui_menu_hide(const char*) {}
}  // namespace patches

namespace {

std::map<std::string, std::string> g_cvars;
std::string g_root;
float g_cam[7] = { 80, 64, 0, 0, 0, 0, 0 };
float g_stretch = 1.0f;
bool  g_has_cam = false;

const char* p_cvar(const char* name) {
    auto it = g_cvars.find(name);
    return it == g_cvars.end() ? "" : it->second.c_str();
}
int p_read(const char* path, void** buf) {
    *buf = nullptr;
    const std::string full = g_root + "/" + path;
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
bool p_camera(float* fx, float* fy, float org[3], float axis[9]) {
    if (!g_has_cam) return false;
    *fx = g_cam[0]; *fy = g_cam[1];
    org[0] = g_cam[2]; org[1] = g_cam[3]; org[2] = g_cam[4];
    const float y = g_cam[5] * 3.14159265f / 180, p = g_cam[6] * 3.14159265f / 180;
    const float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p);
    const float f[3] = { cp * cy, cp * sy, -sp }, l[3] = { -sy, cy, 0 }, u[3] = { sp * cy, sp * sy, cp };
    memcpy(axis, f, 12); memcpy(axis + 3, l, 12); memcpy(axis + 6, u, 12);
    return true;
}
void p_command(const char* t) { printf("command: %s", t); }
float p_stretch(float, float) { return g_stretch; }

// ---- offscreen framebuffer (EXT_framebuffer_object: any driver of the last 15 years) ----
typedef void (APIENTRY* GenFB)(GLsizei, GLuint*);
typedef void (APIENTRY* BindFB)(GLenum, GLuint);
typedef void (APIENTRY* StorageRB)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY* AttachRB)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY* StatusFB)(GLenum);

bool save_png(const char* path, int w, int h, const std::vector<unsigned char>& rgba_bottom_up) {
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    Gdiplus::BitmapData bd;
    Gdiplus::Rect rc(0, 0, w, h);
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    for (int y = 0; y < h; ++y) {
        const unsigned char* s = &rgba_bottom_up[(size_t)(h - 1 - y) * w * 4];
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
            return bmp.Save(wpath, &codecs[i].Clsid, nullptr) == Gdiplus::Ok;
        }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: hud_preview <scene.txt> <out.png>\n"); return 2; }
    int W = 1920, H = 1080, frames = 40, VW = 0, VH = 0;
    std::string background;
    FILE* sf = fopen(argv[1], "rb");
    if (!sf) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    char line[8192];
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
        else if (!strcmp(key, "files")) g_root = rest;
        else if (!strcmp(key, "frames")) frames = atoi(rest);
        else if (!strcmp(key, "stretch")) g_stretch = (float)atof(rest);
        else if (!strcmp(key, "view")) sscanf(rest, "%d %d", &VW, &VH);
        else if (!strcmp(key, "camera")) {
            g_has_cam = sscanf(rest, "%f %f %f %f %f %f %f", &g_cam[0], &g_cam[1], &g_cam[2], &g_cam[3], &g_cam[4], &g_cam[5], &g_cam[6]) == 7;
        } else if (!strcmp(key, "cvar")) {
            char name[128] = "";
            int o2 = 0;
            if (sscanf(rest, "%127s %n", name, &o2) >= 1) g_cvars[name] = rest + o2;
        }
    }
    fclose(sf);

    Gdiplus::GdiplusStartupInput gin;
    ULONG_PTR gtok = 0;
    Gdiplus::GdiplusStartup(&gtok, &gin, nullptr);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "hud_preview";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("hud_preview", "hud_preview", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
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

    patches::StreamerHudIO io = { p_cvar, p_read, p_free, p_camera, p_command, p_stretch, true };
    patches::streamer_hud_set_io(&io);

    for (int f = 0; f < frames; ++f) {
        glViewport(0, 0, W, H);
        glClearColor(0.2f, 0.22f, 0.25f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        patches::ui_frame_dt(1.0f / 60);
        patches::ui_begin_2d(W, H);
        if (!background.empty()) patches::ui_image_cover(0, 0, (float)W, (float)H, background.c_str());
        if (patches::streamer_hud_update()) patches::streamer_hud_draw((float)W, (float)H);
        else if (f == 0) printf("feed not live: the overlay would not be drawn\n");
        patches::ui_alpha(1.0f);
        patches::ui_end_2d();
        glFinish();
        Sleep(16);
    }
    std::vector<unsigned char> px((size_t)W * H * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const bool ok = save_png(argv[2], W, H, px);
    printf("%s %s (%dx%d)\n", ok ? "wrote" : "FAILED", argv[2], W, H);
    if (ok && VW > 0 && VH > 0) {
        // what the viewer sees when the display stretches the picture
        wchar_t wsrc[MAX_PATH], wdst[MAX_PATH];
        const std::string dst = std::string(argv[2]) + ".view.png";
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, wsrc, MAX_PATH);
        MultiByteToWideChar(CP_UTF8, 0, dst.c_str(), -1, wdst, MAX_PATH);
        Gdiplus::Bitmap* src = new Gdiplus::Bitmap(wsrc);
        Gdiplus::Bitmap out(VW, VH, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics g(&out);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(src, 0, 0, VW, VH);
        }
        delete src;
        UINT n = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&n, &size);
        std::vector<unsigned char> buf(size);
        Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
        Gdiplus::GetImageEncoders(n, size, codecs);
        for (UINT i = 0; i < n; ++i)
            if (!wcscmp(codecs[i].MimeType, L"image/png")) out.Save(wdst, &codecs[i].Clsid, nullptr);
        printf("wrote %s (%dx%d)\n", dst.c_str(), VW, VH);
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(wnd, dc);
    DestroyWindow(wnd);
    Gdiplus::GdiplusShutdown(gtok);
    return ok ? 0 : 1;
}
