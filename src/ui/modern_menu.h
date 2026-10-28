#ifndef COD1RELOADED_MODERN_MENU_H
#define COD1RELOADED_MODERN_MENU_H

namespace patches {

// The 1.6X settings panel drawn by gl_overlay. One call per frame while visible.
void modern_menu_draw(float screen_w, float screen_h);

// Polled every frame even while hidden: returns true when the engine main-menu
// button requested the panel (cvar cod1x_open_modern, set by ui_mp/main.menu).
bool modern_menu_poll_open();

// The menu's widgets, for the panels drawn inside it (ui/hud_editor.cpp): same look,
// same behaviour. Window pixels; each returns the new value / true when clicked.
bool menu_button(float x, float y, float w, float h, const char* label, bool primary);
int  menu_segmented(float x, float y, float w, const char* const* opts, int n, int sel);   // 40 px high
int  menu_slider(float x, float y, float w, int lo, int hi, int v);                          // 40 px high
bool menu_toggle(float x, float y, const char* label, bool on);                              // 28 px high
void menu_section(float x, float y, float w, const char* s);
void menu_hint(float x, float y, const char* s);
bool menu_hit(float x, float y, float w, float h);

}  // namespace patches

#endif
