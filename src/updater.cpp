// Updater simple pour cod1reloaded.
//
// Flow :
//   1. Au demarrage (DllMain), `updater_apply_pending()` regarde si un
//      fichier `<dll>.new` existe. Si oui -> rename `.new` -> `.dll`
//      avant que quoi que ce soit d'autre soit fait.
//   2. `updater_start()` spawn un thread qui fetch le manifest JSON, le
//      parse, compare la version, et si plus recent telecharge le DLL
//      dans `<dll>.new` pour le prochain demarrage.
//   3. Notif via log + optionnellement MessageBox.
//
// Format manifest JSON attendu (statique sur ton serveur web) :
//   {
//     "version":       "0.2.0",
//     "download_url":  "https://.../mss32.dll",
//     "min_version":   "0.1.0",
//     "notes":         "Texte affiche dans la popup"
//   }
//
// Dependances : wininet.dll (lie via CMake). Pas de bibliotheque JSON,
// parsing manuel ultra simple (suffit pour 4 cles).

#include "updater.h"
#include "logger.h"

#include <wininet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace patches {

UpdaterConfig g_updater_config = {
    /* enable        */ true,
    /* manifest_url  */ "",
    /* auto_download */ true,
    /* show_dialog   */ true,
};

namespace {

// Chemin runtime du DLL courant (mss32.dll ou cod1reloaded.dll).
char g_dll_path[MAX_PATH] = {0};

// Compare deux strings de version "x.y.z" lexicographiquement avec un
// split en composants. Retourne -1 si a<b, 0 si egal, 1 si a>b.
int version_compare(const char* a, const char* b) {
    while (*a && *b) {
        // Lit un composant numerique
        int va = 0, vb = 0;
        while (*a >= '0' && *a <= '9') { va = va * 10 + (*a - '0'); ++a; }
        while (*b >= '0' && *b <= '9') { vb = vb * 10 + (*b - '0'); ++b; }
        if (va < vb) return -1;
        if (va > vb) return  1;
        // Skip separateur
        if (*a == '.') ++a;
        if (*b == '.') ++b;
    }
    if (*a) return  1;
    if (*b) return -1;
    return 0;
}

// Extrait la valeur string d'une cle JSON simple : "key": "value".
// Tres minimaliste, ne gere pas les escapes ni les structures imbriquees.
// Retourne true si trouve.
bool json_extract_string(const char* json, const char* key, char* out, size_t out_size) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    // Skip whitespace puis ':'
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != ':') return false;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != '"') return false;
    ++p;
    // Copie jusqu'au prochain "
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

// Telecharge l'URL en memoire ou dans un fichier.
// Si out_file != NULL : ecrit dans le fichier
// Sinon : remplit out_buf jusqu'a out_buf_size
bool http_download(const char* url, char* out_buf, size_t out_buf_size,
                   const char* out_file_path) {
    HINTERNET h_inet = InternetOpenA(
        "cod1reloaded updater", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!h_inet) return false;

    HINTERNET h_url = InternetOpenUrlA(
        h_inet, url, NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!h_url) {
        InternetCloseHandle(h_inet);
        return false;
    }

    // Verifie le status HTTP
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (HttpQueryInfoA(h_url, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &status_size, NULL)) {
        if (status != 200) {
            logger::logf("updater: HTTP %lu sur %s", status, url);
            InternetCloseHandle(h_url);
            InternetCloseHandle(h_inet);
            return false;
        }
    }

    FILE* out_fp = NULL;
    if (out_file_path) {
        out_fp = fopen(out_file_path, "wb");
        if (!out_fp) {
            InternetCloseHandle(h_url);
            InternetCloseHandle(h_inet);
            return false;
        }
    }

    char chunk[8192];
    DWORD bytes_read = 0;
    size_t total_read = 0;
    bool ok = true;
    while (InternetReadFile(h_url, chunk, sizeof(chunk), &bytes_read) && bytes_read > 0) {
        if (out_fp) {
            if (fwrite(chunk, 1, bytes_read, out_fp) != bytes_read) {
                ok = false; break;
            }
        } else {
            if (total_read + bytes_read >= out_buf_size) {
                ok = false; break;
            }
            memcpy(out_buf + total_read, chunk, bytes_read);
            total_read += bytes_read;
        }
    }

    if (out_fp) {
        fclose(out_fp);
    } else if (ok) {
        out_buf[total_read] = '\0';
    }

    InternetCloseHandle(h_url);
    InternetCloseHandle(h_inet);
    return ok;
}

DWORD WINAPI updater_thread(LPVOID) {
    if (!g_updater_config.enable || g_updater_config.manifest_url[0] == '\0') {
        logger::logf("updater: disabled (no manifest_url or enable=false)");
        return 0;
    }

    logger::logf("updater: fetching manifest %s", g_updater_config.manifest_url);

    char manifest[4096];
    if (!http_download(g_updater_config.manifest_url, manifest, sizeof(manifest), NULL)) {
        logger::logf("updater: manifest fetch failed");
        return 0;
    }

    char remote_version[32]  = {0};
    char download_url[512]   = {0};
    char notes[512]          = "";

    if (!json_extract_string(manifest, "version", remote_version, sizeof(remote_version))) {
        logger::logf("updater: malformed manifest (no 'version')");
        return 0;
    }
    if (!json_extract_string(manifest, "download_url", download_url, sizeof(download_url))) {
        logger::logf("updater: malformed manifest (no 'download_url')");
        return 0;
    }
    json_extract_string(manifest, "notes", notes, sizeof(notes));

    const int cmp = version_compare(remote_version, COD1RELOADED_VERSION);
    if (cmp <= 0) {
        logger::logf("updater: up to date (local=%s remote=%s)",
                     COD1RELOADED_VERSION, remote_version);
        return 0;
    }

    logger::logf("updater: NEW VERSION available %s -> %s",
                 COD1RELOADED_VERSION, remote_version);

    if (g_updater_config.auto_download) {
        char new_path[MAX_PATH];
        snprintf(new_path, sizeof(new_path), "%s.new", g_dll_path);
        logger::logf("updater: downloading %s -> %s", download_url, new_path);
        if (http_download(download_url, NULL, 0, new_path)) {
            logger::logf("updater: download OK, will apply on next launch");
        } else {
            logger::logf("updater: download FAILED");
        }
    }

    if (g_updater_config.show_dialog) {
        char msg[1024];
        snprintf(msg, sizeof(msg),
            "cod1reloaded update available\n\n"
            "Current: %s\n"
            "New:     %s\n\n"
            "%s\n\n"
            "%s",
            COD1RELOADED_VERSION, remote_version, notes,
            g_updater_config.auto_download
                ? "Downloaded. Will apply at next launch."
                : "Visit project page to download.");
        MessageBoxA(NULL, msg, "cod1reloaded", MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}

}  // namespace

void updater_apply_pending() {
    HMODULE self = GetModuleHandleA("cod1reloaded.dll");
    if (!self) self = GetModuleHandleA("mss32.dll");
    if (!self) return;
    if (GetModuleFileNameA(self, g_dll_path, MAX_PATH) == 0) return;

    char new_path[MAX_PATH];
    snprintf(new_path, sizeof(new_path), "%s.new", g_dll_path);
    if (GetFileAttributesA(new_path) == INVALID_FILE_ATTRIBUTES) return;

    // ATTENTION : on est CURRENTLY loaded. On peut PAS remplacer le fichier
    // dont on est l'image en memoire. La rename .new -> .dll va echouer.
    //
    // Solution : on schedule un MoveFileEx avec MOVEFILE_DELAY_UNTIL_REBOOT,
    // OU on bytewise copy le .new par dessus le .dll (Windows autorise une
    // re-ecriture du fichier exe meme s'il est mappe, dans certains cas).
    //
    // Strategie ici : on tente la copie. Si echec, on schedule au reboot.
    // L'utilisateur doit relancer le jeu pour appliquer (pas reboot Windows).

    // Le truc qui marche en pratique : on rename le .dll en .dll.old puis
    // on rename le .new en .dll. Windows accepte les renames de fichiers
    // mappes en memoire (mais pas les writes).
    char old_path[MAX_PATH];
    snprintf(old_path, sizeof(old_path), "%s.old", g_dll_path);
    DeleteFileA(old_path); // au cas ou un ancien .old traine

    if (!MoveFileA(g_dll_path, old_path)) {
        // Echec : peut-etre le .dll n'est plus la (cas etrange)
        return;
    }
    if (!MoveFileA(new_path, g_dll_path)) {
        // Roll back
        MoveFileA(old_path, g_dll_path);
        return;
    }
    // Note : le .old peut etre supprime au prochain run mais pour l'instant
    // on le laisse comme backup. Suppression delayed-reboot :
    MoveFileExA(old_path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
}

void updater_start() {
    // Resoud le chemin du DLL courant pour les operations de fichier
    if (g_dll_path[0] == '\0') {
        HMODULE self = GetModuleHandleA("cod1reloaded.dll");
        if (!self) self = GetModuleHandleA("mss32.dll");
        if (self) GetModuleFileNameA(self, g_dll_path, MAX_PATH);
    }

    HANDLE h = CreateThread(NULL, 0, updater_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

}  // namespace patches
