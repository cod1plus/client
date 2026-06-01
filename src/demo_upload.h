#ifndef COD1RELOADED_DEMO_UPLOAD_H
#define COD1RELOADED_DEMO_UPLOAD_H

#include <windows.h>

namespace patches {

struct DemoUploadConfig {
    bool enable;
    // URL HTTP(S) qui accepte POST avec le fichier en body.
    // Vide = desactive.
    char upload_url[512];
    // Si true, supprime le fichier .dm_1 apres upload reussi.
    // Sinon le renomme en .dm_1.uploaded pour ne pas re-uploader.
    bool delete_after_upload;
    // Affiche un toast Windows pour les events d'upload (start/success/fail).
    bool show_toasts;
    // Intervalle de check pour nouveaux demos (en secondes). 0 = check seulement
    // au demarrage. >0 = polling regulier pour attraper les demos crees pendant
    // la session.
    int  poll_interval_sec;
};

extern DemoUploadConfig g_demo_upload_config;

// Spawn un thread background qui scan & upload les demos.
// Non bloquant.
void demo_upload_start();

// Reveille le thread d'upload immediatement (au lieu d'attendre le prochain
// poll). A appeler quand on detecte un evenement comme un disconnect, un
// quit, ou la fin d'un match. Thread-safe.
void demo_upload_trigger_now();

}  // namespace patches

#endif
