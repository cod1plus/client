// Blocking updater. On launch, BEFORE the game creates its window (IAT hook on
// CoDMP.exe's CreateWindowExA), fetch the manifest and compare versions. If a newer
// version exists, show ONE dialog and stop the game from launching with the old
// version (click Yes -> auto-download + install, then the player relaunches). If the
// build is up to date or the machine is offline, the game launches normally.
//
// manifest keys: version, download_url, notes

#include "features/updater.h"
#include "core/logger.h"
#include "core/iat.h"

#include <windows.h>
#include <wininet.h>
#include <shellapi.h>
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

typedef ATOM (WINAPI *RegisterClassA_t)(const WNDCLASSA*);
typedef LONG (WINAPI *ChangeDisplaySettingsA_t)(DEVMODEA*, DWORD);
RegisterClassA_t         g_real_RegisterClassA         = nullptr;
ChangeDisplaySettingsA_t g_real_ChangeDisplaySettingsA = nullptr;

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

// out_file_path != NULL -> file, else into out_buf. Short timeouts so an offline
// machine fails fast instead of freezing the game at the window-creation gate.
bool http_download(const char* url, char* out_buf, size_t out_buf_size,
                   const char* out_file_path) {
    HINTERNET h_inet = InternetOpenA(
        "cod1reloaded updater", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!h_inet) return false;

    DWORD tmo = 6000;
    InternetSetOptionA(h_inet, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo));

    HINTERNET h_url = InternetOpenUrlA(
        h_inet, url, NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!h_url) {
        InternetCloseHandle(h_inet);
        return false;
    }
    DWORD rtmo = 15000;
    InternetSetOptionA(h_url, INTERNET_OPTION_RECEIVE_TIMEOUT, &rtmo, sizeof(rtmo));

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

    // How many bytes we are supposed to end up with. Without this a connection that
    // dies mid-transfer looks exactly like a clean end-of-file, and we would report
    // success for a truncated file - which, for the DLL, means installing a broken
    // image over the working one.
    DWORD expected = 0;
    DWORD expected_size = sizeof(expected);
    if (!HttpQueryInfoA(h_url, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
                        &expected, &expected_size, NULL)) {
        expected = 0;   // chunked transfer: no length to check against
    }

    char chunk[8192];
    DWORD bytes_read = 0;
    size_t total_read = 0;
    bool ok = true;
    for (;;) {
        if (!InternetReadFile(h_url, chunk, sizeof(chunk), &bytes_read)) {
            logger::logf("updater: read error after %zu bytes on %s (err=%lu)",
                         total_read, url, GetLastError());
            ok = false;
            break;
        }
        if (bytes_read == 0) break;          // genuine end of file
        if (out_fp) {
            if (fwrite(chunk, 1, bytes_read, out_fp) != bytes_read) {
                ok = false; break;
            }
        } else {
            if (total_read + bytes_read >= out_buf_size) {
                ok = false; break;
            }
            memcpy(out_buf + total_read, chunk, bytes_read);
        }
        total_read += bytes_read;
    }

    if (ok && expected > 0 && total_read != (size_t)expected) {
        logger::logf("updater: TRUNCATED download of %s (%zu of %lu bytes) - discarded",
                     url, total_read, (unsigned long)expected);
        ok = false;
    }

    if (out_fp) {
        fclose(out_fp);
        if (!ok && out_file_path) DeleteFileA(out_file_path);
    } else if (ok) {
        out_buf[total_read] = '\0';
    }

    InternetCloseHandle(h_url);
    InternetCloseHandle(h_inet);
    return ok;
}

unsigned rd16(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
unsigned rd32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

// Second line of defence only. A DLL we are about to swap in must be a 32-bit PE whose
// file is at least as long as its own last section - that rejects an HTML error page,
// an empty file, and a transfer cut anywhere in the first ~75% of the image.
//
// It CANNOT prove the file is complete: mingw leaves ~280 KB of symbol data past the
// last section, and a cut inside that tail still loads. Completeness is guaranteed
// upstream instead - the download is written to a .part file and only promoted to .new
// once its length matches Content-Length, so a .new on disk is complete by
// construction. This check exists for the file that a killed process leaves behind.
bool looks_like_a_dll(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    bool ok = false;
    unsigned char dos[0x40];
    do {
        if (fread(dos, 1, sizeof(dos), f) != sizeof(dos)) break;
        if (dos[0] != 'M' || dos[1] != 'Z') break;

        const unsigned e_lfanew = rd32(dos + 0x3c);
        if (e_lfanew < 0x40 || e_lfanew > 0x10000000) break;

        unsigned char nt[0x18];
        if (fseek(f, (long)e_lfanew, SEEK_SET) != 0) break;
        if (fread(nt, 1, sizeof(nt), f) != sizeof(nt)) break;
        if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) break;
        if (rd16(nt + 4) != 0x014c) break;            // IMAGE_FILE_MACHINE_I386

        const unsigned nsec    = rd16(nt + 6);
        const unsigned opt_sz  = rd16(nt + 0x14);
        if (nsec == 0 || nsec > 96) break;

        if (fseek(f, 0, SEEK_END) != 0) break;
        const long file_size = ftell(f);
        if (file_size <= 0) break;

        const long sect_tbl = (long)e_lfanew + 0x18 + (long)opt_sz;
        if (fseek(f, sect_tbl, SEEK_SET) != 0) break;

        unsigned long needed = 0;
        bool short_read = false;
        for (unsigned i = 0; i < nsec; ++i) {
            unsigned char s[40];
            if (fread(s, 1, sizeof(s), f) != sizeof(s)) { short_read = true; break; }
            const unsigned raw_size = rd32(s + 16);
            const unsigned raw_ptr  = rd32(s + 20);
            if (raw_size == 0) continue;              // .bss and friends occupy no file
            const unsigned long end = (unsigned long)raw_ptr + raw_size;
            if (end > needed) needed = end;
        }
        if (short_read) break;                        // truncated inside the section table

        ok = ((unsigned long)file_size >= needed);
    } while (0);

    fclose(f);
    return ok;
}

// Swap the freshly-downloaded dll into place: g_dll_path -> .old, new_path -> g_dll_path.
// Windows allows renaming a mapped image; it takes effect at the next launch.
bool apply_new(const char* new_path) {
    if (g_dll_path[0] == '\0') return false;

    // NEVER touch the working DLL before the replacement is known to be loadable.
    // There is a moment in the swap below where mss32.dll does not exist, and the
    // player's only copy of it is the one we are about to move aside: installing a
    // corrupt file here leaves the game unable to start at all ("mss32.dll was not
    // found"), with no way back short of a manual reinstall.
    if (!looks_like_a_dll(new_path)) {
        logger::logf("updater: %s is not a valid 32-bit DLL - refusing to install it, "
                     "keeping the current version", new_path);
        DeleteFileA(new_path);
        return false;
    }

    char old_path[MAX_PATH];
    snprintf(old_path, sizeof(old_path), "%s.old", g_dll_path);
    DeleteFileA(old_path);
    if (!MoveFileA(g_dll_path, old_path)) return false;
    if (!MoveFileA(new_path, g_dll_path)) {
        MoveFileA(old_path, g_dll_path);  // roll back
        return false;
    }
    MoveFileExA(old_path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    return true;
}

// Runs ONCE, on the main thread, right before the game creates its window. May
// ExitProcess (update pending) or return (up to date / offline -> game continues).
void updater_gate() {
    if (!g_updater_config.enable || g_updater_config.manifest_url[0] == '\0') return;
    if (g_dll_path[0] == '\0') return;

    char manifest[4096];
    if (!http_download(g_updater_config.manifest_url, manifest, sizeof(manifest), NULL)) {
        logger::logf("updater: manifest fetch failed -> launching without check (offline?)");
        return;  // never lock a player out because the network is down
    }

    char remote[32] = {0}, url[512] = {0}, notes[512] = "";
    if (!json_extract_string(manifest, "version", remote, sizeof(remote)) ||
        !json_extract_string(manifest, "download_url", url, sizeof(url))) {
        logger::logf("updater: malformed manifest -> launching without check");
        return;
    }
    json_extract_string(manifest, "notes", notes, sizeof(notes));

    if (version_compare(remote, COD1RELOADED_VERSION) <= 0) {
        logger::logf("updater: up to date (local=%s remote=%s)", COD1RELOADED_VERSION, remote);
        return;  // launch normally
    }

    logger::logf("updater: update %s -> %s available, blocking launch",
                 COD1RELOADED_VERSION, remote);

    char msg[1400];
    snprintf(msg, sizeof(msg),
        "A COD1.6X update is available.\n\n"
        "Current version: %s\n"
        "New version:     %s\n\n"
        "%s\n\n"
        "Download and install it now?\n"
        "(The game cannot launch with the old version.)",
        COD1RELOADED_VERSION, remote, notes[0] ? notes : "");

    const int r = MessageBoxA(NULL, msg, "COD1.6X - Update",
                              MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);

    if (r == IDYES) {
        char new_path[MAX_PATH], part_path[MAX_PATH];
        snprintf(new_path,  sizeof(new_path),  "%s.new",  g_dll_path);
        snprintf(part_path, sizeof(part_path), "%s.part", g_dll_path);
        logger::logf("updater: downloading %s -> %s", url, part_path);

        // Download under .part and only promote it once the length checked out, so a
        // .new on disk is always a complete file - including for the run that gets
        // killed halfway through.
        DeleteFileA(part_path);
        bool got = false;
        if (http_download(url, NULL, 0, part_path)) {
            DeleteFileA(new_path);
            got = MoveFileA(part_path, new_path);
            if (!got) logger::logf("updater: cannot rename %s -> %s (err=%lu)",
                                   part_path, new_path, GetLastError());
        }
        if (got && apply_new(new_path)) {
            char done[320];
            snprintf(done, sizeof(done),
                "Update installed!\n\nRestart the game to play on %s.", remote);
            MessageBoxA(NULL, done, "COD1.6X",
                        MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        } else {
            logger::logf("updater: auto-download/install FAILED, offering manual link");
            char fail[720];
            snprintf(fail, sizeof(fail),
                "Automatic download failed.\n\n"
                "Download the new version manually here:\n%s", url);
            MessageBoxA(NULL, fail, "COD1.6X",
                        MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
            ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
        }
    }

    // Yes (installed, needs relaunch) or No: the old version must never run.
    ExitProcess(0);
}

// Gate the FIRST video-init call, before the game registers its window class or
// changes the display mode -> the dialog shows on the desktop, never over the game.
void gate_once() {
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        updater_gate();  // may ExitProcess
    }
}
ATOM WINAPI hk_RegisterClassA(const WNDCLASSA* wc) {
    gate_once();
    return g_real_RegisterClassA(wc);
}
LONG WINAPI hk_ChangeDisplaySettingsA(DEVMODEA* dm, DWORD flags) {
    gate_once();
    return g_real_ChangeDisplaySettingsA(dm, flags);
}

}  // namespace

// Apply a leftover .new (interrupted update) + clean the previous .old. Call early.
void updater_apply_pending() {
    HMODULE self = GetModuleHandleA("cod1reloaded.dll");
    if (!self) self = GetModuleHandleA("mss32.dll");
    if (!self) return;
    if (GetModuleFileNameA(self, g_dll_path, MAX_PATH) == 0) return;

    char new_path[MAX_PATH];
    snprintf(new_path, sizeof(new_path), "%s.new", g_dll_path);
    if (GetFileAttributesA(new_path) != INVALID_FILE_ATTRIBUTES) {
        apply_new(new_path);
    }
    char old_path[MAX_PATH];
    snprintf(old_path, sizeof(old_path), "%s.old", g_dll_path);
    DeleteFileA(old_path);
    // A .part is by definition an unfinished download: never install it, just clean up.
    char part_path[MAX_PATH];
    snprintf(part_path, sizeof(part_path), "%s.part", g_dll_path);
    DeleteFileA(part_path);
}

void updater_start() {
    if (g_dll_path[0] == '\0') {
        HMODULE self = GetModuleHandleA("cod1reloaded.dll");
        if (!self) self = GetModuleHandleA("mss32.dll");
        if (self) GetModuleFileNameA(self, g_dll_path, MAX_PATH);
    }
    // Gate as early as possible: ChangeDisplaySettingsA (fullscreen res change) and
    // RegisterClassA (window class) are both called before the window exists.
    void* rc = iat_hook("user32.dll", "RegisterClassA", (void*)hk_RegisterClassA);
    void* cd = iat_hook("user32.dll", "ChangeDisplaySettingsA", (void*)hk_ChangeDisplaySettingsA);
    g_real_RegisterClassA         = (RegisterClassA_t)rc;
    g_real_ChangeDisplaySettingsA = (ChangeDisplaySettingsA_t)cd;
    if (rc || cd) {
        logger::logf("updater: video-init gate installed (RegisterClassA=%p ChangeDisplaySettingsA=%p)",
                     rc, cd);
    } else {
        logger::logf("updater: IAT hook FAILED -> no update gate");
    }
}

}  // namespace patches
