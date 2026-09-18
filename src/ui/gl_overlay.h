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
void overlay_start();          // DllMain: IAT-hook SwapBuffers + subclass the window
// The game window, learned from WindowFromDC at every SwapBuffers - valid in
// EVERY display mode, unlike window_patch::get_game_window() which stays NULL
// unless the borderless watcher runs (the trap that killed gamma restore).
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
