#ifndef COD1RELOADED_UI_DRAW_H
#define COD1RELOADED_UI_DRAW_H
// The overlay's 2D renderer: GDI text cache, GDI+ images, raw textures, shapes (flat and
// anti-aliased). Kept apart from gl_overlay.cpp (the SwapBuffers hook, input, menus) so it
// can also run outside the game: tools/hud_preview renders the streamer overlay to a PNG
// with this very file, which is how its look is checked before shipping.
//
// The public draw API is declared in ui/gl_overlay.h. This header is the frame bracket.
#include <windows.h>

namespace patches {

// 2D pass over the current GL context, in window pixels (y down). The engine's state is
// saved and restored around it. A new GL context (vid_restart) drops every cached texture.
void ui_begin_2d(int vw, int vh);
void ui_end_2d();
// Seconds since the previous frame, for ui_smooth.
void ui_frame_dt(float dt);
// Current colour (0xAARRGGBB, times the global alpha) for raw GL drawing.
void ui_set_color(DWORD rgba);

}  // namespace patches

#endif
