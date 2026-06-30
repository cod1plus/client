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
};

extern DiscordRpcConfig g_discord_rpc_config;

void discord_rpc_start();
void discord_rpc_shutdown();  // call from DLL_PROCESS_DETACH

}  // namespace patches

#endif
