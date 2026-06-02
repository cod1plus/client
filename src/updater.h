#ifndef COD1RELOADED_UPDATER_H
#define COD1RELOADED_UPDATER_H

#include <windows.h>

namespace patches {

// Version du DLL embarquee dans le binaire (compare a la version distante).
// Format semver simple : "MAJ.MIN.PATCH". Compare lexicographiquement avec
// padding zero -> "0.1.0" < "0.10.0" marche correctement.
constexpr const char* COD1RELOADED_VERSION = "1.0.1";

struct UpdaterConfig {
    bool        enable;
    // URL d'un manifest JSON. Format attendu :
    //   { "version": "0.2.0",
    //     "download_url": "https://.../mss32.dll",
    //     "min_version": "0.1.0",
    //     "notes": "..." }
    // Mets une chaine vide pour desactiver completement le check reseau.
    char        manifest_url[512];
    // Si true, telecharge automatiquement le nouveau DLL en arriere-plan.
    // Le remplacement s'applique au prochain lancement.
    bool        auto_download;
    // Si true, affiche une MessageBox quand une nouvelle version est dispo.
    bool        show_dialog;
};

extern UpdaterConfig g_updater_config;

// Demarre le check d'update en background. Non bloquant.
// Doit etre appele apres logger::init.
void updater_start();

// Appele tres tot au demarrage pour appliquer une mise a jour pending
// (renomme cod1reloaded.dll.new -> cod1reloaded.dll si present).
// A appeler avant que notre DLL fasse quoi que ce soit d'important.
void updater_apply_pending();

}  // namespace patches

#endif
