#ifndef COD1RELOADED_HUD_EDITOR_H
#define COD1RELOADED_HUD_EDITOR_H
// hud_editor - Ctrl+M > Display > HUD: where the 1.6X HUD sits on the screen.
//
// The Display tab shows a miniature of the screen with the minimap at its place (live) and
// an "Edit HUD layout" button. The editor then takes the whole screen instead of the menu:
// the game stays visible behind a light veil, the minimap is drawn live at its real place
// and size, dragged with the mouse, resized by a corner (snapped to the screen's edges and
// centre lines), and a side panel sets the rest (shape, orientation, zoom, opacity,
// teammates). Everything is saved as it changes (archived cvars, see ui/minimap.h);
// Reset puts the defaults back. ESC or Done goes back to the Display tab.

namespace patches {

bool  hud_editor_active();
void  hud_editor_open(bool on);
// The Display tab's block (inside the menu's 2D pass). Returns its height.
float hud_editor_block(float x, float y, float w);
// The editor, full screen, drawn by the menu instead of its tabs.
void  hud_editor_draw(float sw, float sh);

}  // namespace patches

#endif
