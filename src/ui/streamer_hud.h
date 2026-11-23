#ifndef COD1RELOADED_STREAMER_HUD_H
#define COD1RELOADED_STREAMER_HUD_H
// Native caster overlay for the PAM streamer mode (the "Streamer" team of the PAM).
//
// The PAM (kvcodPAM _streamer_native.gsc) keeps being the source of every piece of data:
// the top bar cvars (ui_streamersystem_team*_player*_*, ui_streamersystem_top_*), and,
// once this client has announced itself (menu response "streamersystem_native", sent as
// `cmd mr <sv_serverid> -1 streamersystem_native`), a 10 Hz feed of every player's position,
// weapon and round kills plus the round / bomb clock (ui_streamersystem_feed). From then
// on the PAM draws no radar / XRAY hudelems for this streamer, and this module hides the
// PAM's top bar menus and draws, at native resolution with real fonts, in the look of the
// 1.6X HUD kit (hud-kit-png/: charcoal panels, orange axis / blue allies, tracked caps):
//   - the 5v5 top bar: player cards (HP, weapon silhouette, grenades, round kills,
//     followed player), the clock block (round / strat / bomb, scores, players alive)
//   - the radar: the map drawn by the client from its own BSP (ui/radar_map.cpp) at the
//     size it is shown, bombsites, players with their view direction
//   - the XRAY, projected with the game's own camera (refdef: pitch, zoom, FOV, aspect),
//     which the server-side XRAY cannot know.
// Weapon silhouettes are the game's own kill icons (the weapon file's killIcon, DDS out of
// the paks). A streamer without this client keeps the PAM overlay: nothing changes for him.
#include <windows.h>

namespace patches {

// Called every frame from the SwapBuffers hook while no 1.6X menu is open. Cheap when
// the streamer overlay is not on screen. Returns true when it wants to draw.
bool streamer_hud_frame();
// Draws the overlay (inside the overlay's 2D pass, window pixels).
void streamer_hud_draw(float vw, float vh);

// ---- how the overlay reaches the game. The DLL uses the engine; tools/hud_preview hands
// ---- over files and fake cvars to render the overlay offline.
struct StreamerHudIO {
    const char* (*cvar)(const char* name);                     // "" when unknown
    int  (*read_file)(const char* path, void** buf);           // length, <= 0 when missing
    void (*free_file)(void* buf);
    bool (*camera)(float* fov_x, float* fov_y, float org[3], float axis[9]);   // refdef
    void (*command)(const char* text);                         // appended to the command buffer
    // how much wider than drawn the screen shows the game (1440x1080 stretched to a 16:9
    // panel: 1.33); the overlay is drawn that much narrower so it comes out right. 1 = none
    float (*stretch)(float vw, float vh);
    bool sync;                                                 // build the radar on this thread
};
void streamer_hud_set_io(const StreamerHudIO* io);
// Reads the cvars / feed. True when the feed is live (what streamer_hud_frame checks
// after its menu gate).
bool streamer_hud_update();

}  // namespace patches

#endif
