#ifndef COD1RELOADED_MODERN_MENU_H
#define COD1RELOADED_MODERN_MENU_H

namespace patches {

// The 1.6X settings panel drawn by gl_overlay. One call per frame while visible.
void modern_menu_draw(float screen_w, float screen_h);

// Polled every frame even while hidden: returns true when the engine main-menu
// button requested the panel (cvar cod1x_open_modern, set by ui_mp/main.menu).
bool modern_menu_poll_open();

}  // namespace patches

#endif
