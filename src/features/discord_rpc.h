#ifndef COD1RELOADED_DISCORD_RPC_H
#define COD1RELOADED_DISCORD_RPC_H

#include <windows.h>

namespace patches {

struct DiscordRpcConfig {
    bool enable = true;
    char client_id[32]      = "";   // discord app id, empty = disabled
    char large_image[64]    = "logo";
    char large_text[128]    = "cod1reloaded";
    char details_menu[128]  = "Dans les menus";
    char details_match[128] = "En partie";
    char state_text[128]    = "";
    bool show_elapsed = true;

    // Map keys uploaded under Rich Presence > Art Assets, space or comma separated,
    // e.g. "mp_carentan mp_brecourt mp_coastal". When the current map is listed it
    // becomes large_image, so the thumbnail is the map you are on. "*" = try every
    // map, empty = never. A key with no asset behind it shows NO image at all, not
    // even the application icon, which is why this is a declared list and not a guess.
    char map_images[512]    = "";

    // Let Discord START the game when someone clicks Join and it is not running.
    // Requires a URI handler under HKCU\Software\Classes\discord-<client_id> - user
    // scope, no admin, exactly what Discord's own SDK registers. Turning this off does
    // not merely stop writing it: it deletes the key, so the switch actually undoes
    // what it did.
    bool register_launch = true;
};

extern DiscordRpcConfig g_discord_rpc_config;

void discord_rpc_start();
void discord_rpc_shutdown();  // call from DLL_PROCESS_DETACH

}  // namespace patches

#endif
