#ifndef COD1RELOADED_DISCORD_RPC_H
#define COD1RELOADED_DISCORD_RPC_H

#include <windows.h>

namespace patches {

// The defaults are the published COD1.6X application: a player's existing ini has no
// discord_rpc_* lines beyond `enable`, and with an empty client id the presence was
// silently off in every release up to 1.6.9 (Discord only showed its own detection).
struct DiscordRpcConfig {
    bool enable = true;
    char client_id[32]      = "1511347633518678077";   // COD1.6X app; empty = disabled
    char large_image[64]    = "";   // empty = the application's icon
    char large_text[128]    = "COD1.6X";
    char details_menu[128]  = "In the menus";
    char details_match[128] = "In a match";
    char state_text[128]    = "";
    bool show_elapsed = true;

    // Map keys uploaded under Rich Presence > Art Assets, space or comma separated,
    // e.g. "mp_carentan mp_brecourt mp_coastal". When the current map is listed it
    // becomes large_image, so the thumbnail is the map you are on. "*" = try every
    // map, empty = never. A key with no asset behind it shows NO image at all, not
    // even the application icon, which is why this is a declared list and not a guess.
    char map_images[512]    = "mp_bocage mp_brecourt mp_burgundy mp_carentan mp_chateau mp_dawnville "
                              "mp_dawnville_gg mp_dawnville_x mp_depot mp_germantown mp_hanoi mp_harbor "
                              "mp_hurtgen mp_kandanos mp_neuville mp_pavlov mp_powcamp mp_railyard "
                              "mp_railyard_x mp_rocket mp_ship mp_stalingrad mp_tigertown";

    // Let Discord START the game when someone clicks Join and it is not running.
    // Requires a URI handler under HKCU\Software\Classes\discord-<client_id> - user
    // scope, no admin, exactly what Discord's own SDK registers. Turning this off does
    // not merely stop writing it: it deletes the key, so the switch actually undoes
    // what it did.
    bool register_launch = true;

    // Put the server password in the join secret, so Join also works on a private
    // server. It travels to everyone who can see the presence - friends and every
    // mutual server - so a match password stops being a match password. Kept behind
    // its own switch so it can be dropped in one line. Off by default: match servers
    // are passworded, and the presence would hand the password to anyone watching.
    bool share_password = false;
};

extern DiscordRpcConfig g_discord_rpc_config;

void discord_rpc_start();
void discord_rpc_shutdown();  // call from DLL_PROCESS_DETACH

}  // namespace patches

#endif
