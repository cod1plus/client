#ifndef COD1RELOADED_MINIMAP_H
#define COD1RELOADED_MINIMAP_H
// minimap - a CoD4-style radar in the CoD1 HUD (cod1x_minimap_on 1, off by default).
//
// Zoomed on the player: only `cod1x_minimap_range` units around him are shown, the map
// turning with the view (or north up), in a round or rounded-square frame. Everything is
// placed from the view the game has just rendered (refdef), at every frame, so it turns
// and slides as smoothly as the picture whatever the server's tick.
//
// The map drawing, by order of preference:
//   1. <game>\minimaps\<map>.png + <map>.json (the Workshop's "Minimaps" pack: images
//      drawn offline from the collision of the map, world rectangle + bombsites in the
//      json, north = +Y up);
//   2. else the client draws it from the map's own BSP (ui/radar_map.cpp) on a worker
//      thread: any map, custom ones included, a few seconds after the map loads.
//
// Who is shown: you, and your teammates present in your snapshot - never an enemy (the
// client knows only what the server sends; in a competitive match showing enemies would
// be a wallhack). Your team is read from the snapshot's client states. The server has the
// last word with its serverinfo / systeminfo key cod1x_minimap: 0 = no minimap on this
// server, 1 = yourself only, 2 = you + teammates. Without the key: teammates, except
// while a competitive ruleset is enforced (ruleset_active) where only you are shown.
//
// The layout (position, size, shape, zoom...) is a set of archived cvars, edited live by
// the HUD editor (ui/hud_editor.cpp, Ctrl+M > Display).
#include <windows.h>

namespace patches {

struct MinimapMate { float x, y, yaw; int team; };   // team: 1 axis, 2 allies (the viewer's)

// ---- how the minimap reaches the game. The DLL uses the engine; tools/minimap_preview
// ---- hands over files, a view and teammates to render it offline.
struct MinimapIO {
    const char* (*cvar)(const char* name);                    // "" when unknown
    void (*reg)(const char* name, const char* def);           // an archived cvar
    void (*command)(const char* text);                        // appended to the command buffer
    int  (*read_file)(const char* path, void** buf);          // engine FS (maps/mp/x.bsp), <= 0 missing
    void (*free_file)(void* buf);
    const char* (*map)();                                     // map of the game on screen, "" = none
    bool (*view)(float org[3], float* yaw_deg, float* fov_x); // the view just rendered
    int  (*mates)(MinimapMate* out, int max);                 // teammates in the snapshot
    int  (*allowed)();                                        // 0 off, 1 yourself, 2 + teammates
    bool (*hidden)();                                         // console / menu / no 2D: nothing drawn
    float (*stretch)(float vw, float vh);                     // 4:3 stretched on a wide panel: 1.33
    const char* (*root)();                                    // folder holding minimaps\ (the game's)
    bool sync;                                                // build the radar on this thread
};
void minimap_set_io(const MinimapIO* io);

// The layout, from / to the cvars (seta: archived).
struct MinimapLayout {
    int   on = 0;          // cod1x_minimap_on: off until the player turns it on (1.6.14; 1.6.13's
                           // cod1x_minimap 1, archived in every config that ran it, is ignored)
    float x = 0.99f;       // cod1x_minimap_x / _y: place in the room the box leaves, 0 = left /
    float y = 0.07f;       // top edge, 1 = right / bottom edge. Top right under a fps line by
                           // default: CoD1's chat is top left.
    int   size = 260;      // cod1x_minimap_size: side in pixels at 1080p (scaled with the height)
    int   shape = 0;       // cod1x_minimap_shape: 0 rounded square, 1 round
    int   rotate = 1;      // cod1x_minimap_rotate: 1 turns with the view, 0 north up
    int   range = 1600;    // cod1x_minimap_range: world units from the centre to the edge
    int   alpha = 90;      // cod1x_minimap_alpha: opacity of the map, percent
    int   mates = 1;       // cod1x_minimap_mates: teammates (when the server allows them)
};
MinimapLayout minimap_layout();
void minimap_layout_set(const MinimapLayout& l);    // only the fields that changed
MinimapLayout minimap_layout_default();

void minimap_frame();                     // every SwapBuffers: cvars, the radar job (cheap)
bool minimap_active();                    // in a game, on, allowed, not hidden: draw this frame
void minimap_draw(float vw, float vh);    // inside a 2D overlay pass, at the layout's place
// The HUD editor: the minimap in a box of the screen (window pixels), with the live view
// when a game runs, else the last radar seen (or an empty frame) at a demo view.
void minimap_draw_box(float x, float y, float w, float h, float vw, float vh, const MinimapLayout& l);
// The box of the layout on a vw x vh screen (window pixels; narrower on a stretched panel).
void minimap_box(const MinimapLayout& l, float vw, float vh, float* x, float* y, float* w, float* h);
// What the map drawing comes from, for the menu: "Workshop pack", "map BSP", "" (none yet).
const char* minimap_source();
const char* minimap_map();                // map of the radar loaded ("" none)
int minimap_server_rule();                // 0 off, 1 yourself only, 2 + teammates (see above)

// In-game check (dev), cod1x_minimap_test 1 on a devmap: <game>\cod1x_minimap_views.txt,
// one step per line, run once the radar is ready:
//     x y z yaw [cvar=value ...]   the cvars set, `setviewpos x y z yaw`, settled, captured
//     editor | menu | close        the HUD editor / the Ctrl+M menu opened (captured) / closed
//     quit                         `quit` once done
// Each capture is the whole frame WITH the overlay (the engine's screenshot command reads the
// picture before the overlay is drawn): Main\screenshots\minimap_<hhmmss>_vNN.png. The
// engine's menus are not drawn meanwhile (the team menu of a fresh devmap).
void minimap_test_frame(int vw, int vh);  // end of the SwapBuffers work, after every overlay draw

}  // namespace patches

#endif
