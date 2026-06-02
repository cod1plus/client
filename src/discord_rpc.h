#ifndef COD1RELOADED_DISCORD_RPC_H
#define COD1RELOADED_DISCORD_RPC_H

#include <windows.h>

namespace patches {

// Discord Rich Presence : affiche l'etat du joueur dans Discord (logo
// cod1plus, "En partie" / "Dans les menus", temps ecoule).
//
// Pour l'activer il faut un client_id d'application Discord :
//   1. https://discord.com/developers/applications -> New Application
//   2. copier l'APPLICATION ID (= client_id) dans discord_rpc_client_id
//   3. Rich Presence > Art Assets : uploader un logo, son nom = large_image
struct DiscordRpcConfig {
    bool enable = true;
    // Application ID Discord (snowflake, ~18-19 chiffres). Vide = desactive.
    char client_id[32]      = "";
    // Cle de l'asset image uploade dans l'app Discord (grand logo).
    char large_image[64]    = "logo";
    // Texte au survol du grand logo.
    char large_text[128]    = "cod1reloaded";
    // Ligne principale selon l'etat.
    char details_menu[128]  = "Dans les menus";
    char details_match[128] = "En partie";
    // Ligne secondaire optionnelle (vide = masquee).
    char state_text[128]    = "";
    // Afficher le chrono "temps ecoule" (depuis le debut de l'etat courant).
    bool show_elapsed = true;
};

extern DiscordRpcConfig g_discord_rpc_config;

// Demarre le thread RPC (connexion pipe + handshake + boucle d'update).
// Non bloquant. No-op si desactive ou si client_id vide.
void discord_rpc_start();

// Stoppe le thread et ferme le pipe (la presence disparait cote Discord).
// A appeler depuis DllMain DLL_PROCESS_DETACH.
void discord_rpc_shutdown();

}  // namespace patches

#endif
