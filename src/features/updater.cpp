// manifest keys: version, download_url, min_version, notes

#include "features/updater.h"
#include "core/logger.h"

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

char g_dll_path[MAX_PATH] = {0};

int version_compare(const char* a, const char* b) {
    while (*a && *b) {
        int va = 0, vb = 0;
        while (*a >= '0' && *a <= '9') { va = va * 10 + (*a - '0'); ++a; }
        while (*b >= '0' && *b <= '9') { vb = vb * 10 + (*b - '0'); ++b; }
        if (va < vb) return -1;
        if (va > vb) return  1;
        if (*a == '.') ++a;
        if (*b == '.') ++b;
    }
    if (*a) return  1;
    if (*b) return -1;
    return 0;
}

// flat "key": "value" only, no escapes
bool json_extract_string(const char* json, const char* key, char* out, size_t out_size) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != ':') return false;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

// out_file_path != NULL -> file, else into out_buf
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

    // can't overwrite our own mapped image, but Windows allows renaming it:
    // .dll -> .old, .new -> .dll. takes effect next launch, no reboot.
    char old_path[MAX_PATH];
    snprintf(old_path, sizeof(old_path), "%s.old", g_dll_path);
    DeleteFileA(old_path);

    if (!MoveFileA(g_dll_path, old_path)) {
        return;
    }
    if (!MoveFileA(new_path, g_dll_path)) {
        MoveFileA(old_path, g_dll_path);
        return;
    }
    MoveFileExA(old_path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
}

void updater_start() {
    if (g_dll_path[0] == '\0') {
        HMODULE self = GetModuleHandleA("cod1reloaded.dll");
        if (!self) self = GetModuleHandleA("mss32.dll");
        if (self) GetModuleFileNameA(self, g_dll_path, MAX_PATH);
    }

    HANDLE h = CreateThread(NULL, 0, updater_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

}  // namespace patches
