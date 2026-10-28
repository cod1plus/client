#ifndef COD1RELOADED_HUD_LAYOUT_H
#define COD1RELOADED_HUD_LAYOUT_H
// hud_layout - the game's own HUD moved or hidden, and the 1.6X counters.
//
// The cgame draws most of its HUD from the menus of ui_mp/hud.menu (Compass, stance,
// Health, weaponinfo, Cursorhints): every item has a rect relative to its menu (rectClient)
// and an absolute one the ownerdraws paint into. Moving an element = rewriting the absolute
// rects of its items with an offset (in the HUD's 640x480 frame), hiding it = clearing their
// WINDOW_VISIBLE flag. It is applied at every frame, so a HUD reloaded by a map change or a
// vid_restart is caught again at once. Ammo and the weapon name are two elements of the
// same menu (weaponinfo), split by item names.
//
// The weapon list (the slots shown at the top when you switch) is drawn by code
// (CG_DrawWeaponSelect): its call is routed through us, and while it runs its pictures and
// texts are shifted (or dropped) on their way to the engine (the cgame's syscall pointer).
//
// The fps counter and the lagometer of the game are drawn at fixed places by code: either
// the game's ones (style 0, cg_drawFPS / cg_lagometer as usual) or the 1.6X ones, drawn by
// the overlay where you put them (the game's are then skipped by a one-byte patch of their
// test, the lagometer reading the game's own samples).
//
// One archived cvar per element: cod1x_hud_<key> "on x y style" - game elements: x y =
// offset in 640x480 units; 1.6X ones: x y = place in the room the box leaves (0 = left /
// top edge, 1 = right / bottom edge), style = how it looks (0 = the game's).

namespace patches {

enum HudElem {
    HUD_COMPASS, HUD_STANCE, HUD_HEALTH, HUD_AMMO, HUD_WEAPON, HUD_HINTS,   // the game's menus
    HUD_WEAPLIST,                                                           // the game's, by code
    HUD_FPS, HUD_LAG,                                                       // fps, lagometer
    HUD_COUNT
};
struct HudState { int on = 1; float x = 0, y = 0; int style = 0; };

const char* hud_elem_label(int e);
bool        hud_elem_game(int e);         // a game element (moved by an offset) - else a counter
int         hud_elem_styles(int e, const char* const** names);   // counters: style names
HudState    hud_elem_get(int e);
HudState    hud_elem_default(int e);
void        hud_elem_set(int e, const HudState& s);              // saved (seta) as it changes
// Where it is on a sw x sh window right now; false when it cannot be placed (no game, not
// in this HUD, the game's own counter).
bool        hud_elem_box(int e, float sw, float sh, float* x, float* y, float* w, float* h);
// The state that puts its box at (x, y) - the editor's drag.
HudState    hud_elem_moved(int e, float sw, float sh, float x, float y);

float hud_compass_size();                 // cg_hudCompassSize
void  hud_compass_size_set(float v);

void hud_layout_frame();                  // every frame (SwapBuffers): cvars, HUD menus, patches, fps clock
void hud_layout_preview();                // the editor is open (each frame): the weapon list kept on screen
bool hud_layout_active();                 // something of ours to draw in game
void hud_layout_draw(float sw, float sh); // in game 2D pass: the 1.6X counters
void hud_layout_draw_elem(int e, float sw, float sh);   // one counter (the editor)
void hud_layout_log();                    // dev: the HUD's menus and items, to the log

}  // namespace patches

#endif
