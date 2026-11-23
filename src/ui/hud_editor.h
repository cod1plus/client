#ifndef COD1RELOADED_HUD_EDITOR_H
#define COD1RELOADED_HUD_EDITOR_H
// hud_editor - Ctrl+M > Display > HUD: where the HUD sits on the screen.
//
// The Display tab shows a miniature of the screen with the HUD at its place (the minimap
// live) and an "Edit HUD layout" button. The editor then takes the whole screen instead of
// the menu: the game stays visible behind a light veil with every element framed at its
// real place - the minimap, the game's own compass / stance / health bar / ammo / weapon
// name / use hints / weapon list (ui/hud_layout.h; the list is kept on screen while the
// editor is open), the fps counter and the lagometer. A click picks one,
// a drag moves it (snapped to the screen's edges and centre lines), the minimap also takes
// a corner to resize it. The side panel lists them all with a switch to hide each, and the
// options of the one picked (the minimap's shape, orientation, zoom..., the compass' size,
// the counters' style). Everything is saved as it changes (archived cvars); Reset puts the
// picked one back at its place, Reset all every one. ESC or Done goes back to the Display tab.

namespace patches {

bool  hud_editor_active();
void  hud_editor_open(bool on);
void  hud_editor_select(int el);          // -1 the minimap, else a HUD_* (ui/hud_layout.h)
// The Display tab's block (inside the menu's 2D pass). Returns its height.
float hud_editor_block(float x, float y, float w);
// The editor, full screen, drawn by the menu instead of its tabs.
void  hud_editor_draw(float sw, float sh);

}  // namespace patches

#endif
