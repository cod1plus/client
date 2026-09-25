#ifndef COD1RELOADED_GL_OVERLAY_H
#define COD1RELOADED_GL_OVERLAY_H

// Native OpenGL overlay - a modern UI drawn at REAL screen resolution inside the
// game's own GL context, hooked at SwapBuffers. This exists because the engine's
// 2D pipeline cannot look modern: .menu files live in 640x480 virtual coordinates
// (everything is upscaled and soft), fonts are engine bitmaps, and both textured
// and Bahnschrift attempts were rejected on sight (2026-07-17). Drawing after the
// engine finished its frame, in native pixels, with GDI-rasterised TrueType text,
// sidesteps all of it - and works in exclusive fullscreen, since it is the game's
// own context.

#include <windows.h>

namespace patches {

struct UiInput {
    float mx = 0, my = 0;      // virtual cursor, client pixels
    bool  down = false;        // left button held
    bool  clicked = false;     // left button pressed this frame (edge)
    float wheel = 0;           // scroll steps this frame
    int   vk = 0;              // during ui_key_capture: virtual key / button pressed
                               // this frame (VK_*, 0xF001 wheel up, 0xF002 wheel
                               // down), 0 = nothing yet
};

// --- lifecycle -------------------------------------------------------------
void overlay_start();          // DllMain: IAT-hook SwapBuffers + register the wndhub listener
void overlay_tick();           // watcher thread, ~1/s: detects a bypassed SwapBuffers hook (log only)
// The game window (= wndhub_window()): learned from WindowFromDC at every
// SwapBuffers, valid in EVERY display mode.
HWND overlay_game_window();
bool overlay_visible();
void overlay_toggle(bool on);

// --- draw API (valid only inside the frame callback) -----------------------
void ui_rect(float x, float y, float w, float h, DWORD rgba);
void ui_rect_rounded(float x, float y, float w, float h, float r, DWORD rgba);
void ui_rect_border(float x, float y, float w, float h, float r, float th, DWORD rgba);
void ui_ellipse(float cx, float cy, float rx, float ry, float th, DWORD rgba);
// px = pixel size, weight 400/600. Returns drawn width.
float ui_text(float x, float y, int px, int weight, DWORD rgba, const char* utf8);
float ui_text_width(int px, int weight, const char* utf8);
const UiInput& ui_input();

// Bind-capture mode (Keys tab): while on, EVERY key / mouse button / wheel event
// is routed into UiInput::vk instead of driving the UI or the overlay hotkeys -
// so any key can be bound, including ESC (the menu treats it as cancel).
void ui_key_capture(bool on);

// Draws a JPG/PNG/BMP file (loaded via GDI+, cached by path, invalidated on
// vid_restart the same way text textures are) covering [x,y,w,h] - scaled up and
// centre-cropped, never letterboxed, like CSS `background-size: cover`. Returns
// false without drawing anything if the file does not exist or fails to decode, so
// callers can fall back to a flat colour.
bool ui_image_cover(float x, float y, float w, float h, const char* path);

// Images decoded from memory (e.g. a file read out of a pk3 through the engine's
// filesystem), cached under `key`. ui_image_known: already decoded (or failed) -
// load only once. ui_image_draw stretches it over [x,y,w,h] (no crop), tinted by rgba.
bool ui_image_known(const char* key);
bool ui_image_load_mem(const char* key, const void* data, int len);
bool ui_image_draw(float x, float y, float w, float h, const char* key, DWORD rgba);

// Filled triangle and a straight line (window pixels).
void ui_triangle(float x1, float y1, float x2, float y2, float x3, float y3, DWORD rgba);
void ui_line(float x1, float y1, float x2, float y2, float th, DWORD rgba);

// Clip everything drawn until ui_clip_pop() to a rectangle (window pixels, y down).
// Not nested.
void ui_clip_push(float x, float y, float w, float h);
void ui_clip_pop();
// A horizontal scale set on the modelview (glScalef) for the drawing that follows:
// ui_clip_push rectangles are then given in those scaled coordinates. 1 = none.
void ui_clip_xscale(float s);

// --- extended 2D (ui/ui_draw.cpp) --------------------------------------------
// Faces: Segoe UI (default) and Consolas for tracked small caps / tabular figures.
enum UiFont { UI_FONT_SANS = 0, UI_FONT_MONO = 1 };
// Text snapped to whole pixels. Weight = GDI weight (400 regular .. 900 black).
float ui_text_font(float x, float y, int px, int weight, DWORD rgba, const char* utf8, int font);
float ui_text_font_width(int px, int weight, const char* utf8, int font);
float ui_text_font_height(int px, int weight, int font);   // line box of that size
// Letter-spaced: every glyph placed apart by `tracking` extra pixels. Returns the width.
float ui_text_tracked(float x, float y, int px, int weight, DWORD rgba, const char* s, float tracking, int font);
float ui_text_tracked_width(int px, int weight, const char* s, float tracking, int font);

// Anti-aliased shapes (feathered one-pixel rim). Polygons must be convex, x/y pairs.
void ui_aa_rect(float x, float y, float w, float h, float r, DWORD rgba);           // r = corner radius
void ui_aa_rect_vgrad(float x, float y, float w, float h, float r, DWORD top, DWORD bottom);
void ui_aa_stroke_rect(float x, float y, float w, float h, float r, float th, DWORD rgba);
void ui_aa_poly(const float* xy, int n, DWORD rgba);
void ui_aa_poly_vgrad(const float* xy, int n, DWORD top, DWORD bottom);
void ui_aa_poly_stroke(const float* xy, int n, float th, DWORD rgba);
void ui_aa_disc(float cx, float cy, float r, DWORD rgba);
void ui_aa_ring(float cx, float cy, float r, float th, DWORD rgba);
void ui_aa_line(float x1, float y1, float x2, float y2, float th, DWORD rgba);
// Soft drop shadow of a rounded rectangle, `blur` pixels of falloff.
void ui_shadow(float x, float y, float w, float h, float r, float blur, DWORD rgba);
// Axis-aligned gradients (no rim: meant for full-pixel boxes).
void ui_rect_vgrad(float x, float y, float w, float h, DWORD top, DWORD bottom);
void ui_rect_hgrad(float x, float y, float w, float h, DWORD left, DWORD right);

// Textures from raw pixels (bytes R,G,B,A, rows top-down), cached under `key` until the
// GL context changes. Uploading under an existing key replaces it. A failed upload (null
// pixels) is remembered, so ui_tex_known also means "do not try again".
bool ui_tex_known(const char* key);
bool ui_tex_upload(const char* key, int w, int h, const void* rgba, bool repeat, bool mipmap);
bool ui_tex_size(const char* key, int* w, int* h);
bool ui_tex_draw(const char* key, float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1, DWORD rgba);

// animation helpers (exponential smoothing, per-frame dt computed at swap)
float ui_smooth(long key, float target, float speed);  // returns the eased value
void  ui_anim_set(long key, float v);                  // seed (e.g. 0 on open)
void  ui_alpha(float mul);                             // global alpha multiplier
float ui_alpha_get();                                  // current multiplier (custom GL draws)

// colors as 0xAARRGGBB
// Monochrome, high contrast: pure black ground, white ink, white pill = selection.
// The only color left is the red warning - danger must not be beautiful.
constexpr DWORD UI_BG      = 0xFF060606;
constexpr DWORD UI_PANEL   = 0xFF0B0B0B;
constexpr DWORD UI_ROW     = 0xFF161616;
constexpr DWORD UI_ROW_HOT = 0xFF242424;
constexpr DWORD UI_ACCENT  = 0xFFF2F2F2;
constexpr DWORD UI_TEXT    = 0xFFF5F5F5;
constexpr DWORD UI_MUTED   = 0xFF7E7E7E;
constexpr DWORD UI_DANGER  = 0xFFE05252;
constexpr DWORD UI_OK      = 0xFFF5F5F5;

}  // namespace patches

#endif
