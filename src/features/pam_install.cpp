// pam_install.cpp - PAM mod download from the 1.6X menu. See pam_install.h.

#include "features/pam_install.h"
#include "core/logger.h"

#include <windows.h>
#include <wininet.h>
#include <wincrypt.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace patches {

PamInstallConfig g_pam_install_config = {
    /* enable       */ true,
    /* manifest_url */ "https://raw.githubusercontent.com/cod1plus/cod1pluspam/main/pam.manifest",
};

namespace {

CRITICAL_SECTION g_cs;
bool      g_cs_init = false;
bool      g_running = false;
PamStatus g_status = { PAM_IDLE, "", 0.f, 0, 0, "" };

constexpr size_t MAX_MANIFEST_BYTES = 256 * 1024;

struct ManifestFile { std::string name; unsigned long long size; std::string sha; std::string url; };
struct Manifest {
    std::string mod;
    int version = 0;
    std::vector<ManifestFile> files;
    std::vector<std::string> removes;
};

void cs_init() {
    if (g_cs_init) return;
    InitializeCriticalSection(&g_cs);
    g_cs_init = true;
}

void set_status(int state, const char* text, float progress = -1.f) {
    EnterCriticalSection(&g_cs);
    g_status.state = state;
    if (text) snprintf(g_status.text, sizeof(g_status.text), "%s", text);
    if (progress >= 0.f) g_status.progress = progress;
    LeaveCriticalSection(&g_cs);
}

void set_counts(int done, int total) {
    EnterCriticalSection(&g_cs);
    g_status.files_done = done;
    g_status.files_total = total;
    LeaveCriticalSection(&g_cs);
}

// "<dir of this DLL>\" - mss32.dll sits next to CoDMP.exe
bool game_dir(char* out, size_t n) {
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&game_dir, &self);
    char p[MAX_PATH] = "";
    if (!self || !GetModuleFileNameA(self, p, MAX_PATH)) return false;
    char* s = strrchr(p, '\\');
    if (!s) return false;
    s[1] = 0;
    snprintf(out, n, "%s", p);
    return true;
}

// pk3 names only: no separators, no traversal, sane length
bool valid_name(const std::string& s) {
    if (s.empty() || s.size() > 96) return false;
    if (s.size() < 5 || _stricmp(s.c_str() + s.size() - 4, ".pk3")) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
    }
    return s != "." && s != "..";
}

bool valid_mod(const std::string& s) {
    if (s.empty() || s.size() > 48) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
    }
    return true;
}

bool parse_manifest(const std::string& text, Manifest& m, std::string& err) {
    size_t pos = 0;
    int line_no = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++line_no;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        // tokens
        std::vector<std::string> t;
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            size_t s = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
            if (i > s) t.push_back(line.substr(s, i - s));
        }
        if (t.empty() || t[0][0] == '#' || t[0][0] == ';') continue;
        if (t[0] == "mod" && t.size() >= 2) {
            if (!valid_mod(t[1])) { err = "bad mod folder name in manifest"; return false; }
            m.mod = t[1];
        } else if (t[0] == "version" && t.size() >= 2) {
            m.version = atoi(t[1].c_str());
        } else if (t[0] == "file" && t.size() >= 5) {
            ManifestFile f;
            f.name = t[1];
            f.size = _strtoui64(t[2].c_str(), nullptr, 10);
            f.sha  = t[3];
            f.url  = t[4];
            if (!valid_name(f.name) || f.sha.size() != 64 || f.url.compare(0, 4, "http") != 0) {
                char b[96]; snprintf(b, sizeof(b), "manifest line %d is malformed", line_no);
                err = b; return false;
            }
            for (size_t k = 0; k < 64; ++k) f.sha[k] = (char)tolower((unsigned char)f.sha[k]);
            m.files.push_back(f);
        } else if (t[0] == "remove" && t.size() >= 2) {
            if (valid_name(t[1])) m.removes.push_back(t[1]);
        }
    }
    if (m.mod.empty()) { err = "manifest names no mod folder"; return false; }
    if (m.files.empty()) { err = "manifest lists no file"; return false; }
    return true;
}

// ---------------------------------------------------------------- SHA-256 (CryptoAPI)
struct Sha256 {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    Sha256() {
        if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
            CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) ok = true;
    }
    ~Sha256() {
        if (hash) CryptDestroyHash(hash);
        if (prov) CryptReleaseContext(prov, 0);
    }
    void update(const void* p, DWORD n) { if (ok && n) CryptHashData(hash, (const BYTE*)p, n, 0); }
    std::string hex() {
        BYTE d[32]; DWORD dn = sizeof(d);
        if (!ok || !CryptGetHashParam(hash, HP_HASHVAL, d, &dn, 0)) return "";
        char out[65];
        for (int i = 0; i < 32; ++i) snprintf(out + i * 2, 3, "%02x", d[i]);
        return std::string(out, 64);
    }
};

bool file_sha256(const char* path, unsigned long long* size, std::string& hex) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    Sha256 h;
    if (!h.ok) { fclose(f); return false; }
    static char buf[1 << 16];
    size_t n;
    unsigned long long total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) { h.update(buf, (DWORD)n); total += n; }
    fclose(f);
    *size = total;
    hex = h.hex();
    return !hex.empty();
}

// ---------------------------------------------------------------- HTTP
HINTERNET open_inet() {
    HINTERNET hi = InternetOpenA("cod1reloaded pam", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (hi) { DWORD tmo = 10000; InternetSetOptionA(hi, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo)); }
    return hi;
}

HINTERNET open_url(HINTERNET hi, const char* url, std::string& err) {
    HINTERNET hu = InternetOpenUrlA(hi, url, NULL, 0,
                                    INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!hu) {
        char b[96]; snprintf(b, sizeof(b), "connection failed (err %lu)", GetLastError());
        err = b; return NULL;
    }
    DWORD rtmo = 30000;
    InternetSetOptionA(hu, INTERNET_OPTION_RECEIVE_TIMEOUT, &rtmo, sizeof(rtmo));
    DWORD status = 0, ssz = sizeof(status);
    if (HttpQueryInfoA(hu, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &ssz, NULL) && status != 200) {
        char b[64]; snprintf(b, sizeof(b), "HTTP %lu", status);
        err = b;
        InternetCloseHandle(hu);
        return NULL;
    }
    return hu;
}

bool http_get_text(const char* url, std::string& out, std::string& err) {
    HINTERNET hi = open_inet();
    if (!hi) { err = "wininet unavailable"; return false; }
    HINTERNET hu = open_url(hi, url, err);
    if (!hu) { InternetCloseHandle(hi); return false; }
    out.clear();
    char buf[8192]; DWORD n = 0; bool ok = true;
    while (InternetReadFile(hu, buf, sizeof(buf), &n) && n > 0) {
        out.append(buf, n);
        if (out.size() > MAX_MANIFEST_BYTES) { ok = false; err = "manifest too large"; break; }
    }
    InternetCloseHandle(hu);
    InternetCloseHandle(hi);
    if (ok && out.empty()) { ok = false; err = "empty manifest"; }
    return ok;
}

// stream `url` into `path`, hashing on the fly; progress via `done_base + bytes` over `total`
bool http_get_file(const char* url, const char* path, unsigned long long expect_size,
                   unsigned long long done_base, unsigned long long total,
                   const char* label, std::string& sha_out, std::string& err) {
    HINTERNET hi = open_inet();
    if (!hi) { err = "wininet unavailable"; return false; }
    HINTERNET hu = open_url(hi, url, err);
    if (!hu) { InternetCloseHandle(hi); return false; }
    FILE* f = fopen(path, "wb");
    if (!f) { err = "cannot write to the mod folder"; InternetCloseHandle(hu); InternetCloseHandle(hi); return false; }
    Sha256 h;
    static char buf[1 << 16];
    DWORD n = 0;
    unsigned long long got = 0;
    bool ok = true;
    DWORD last_ui = 0, t0 = GetTickCount();
    while (InternetReadFile(hu, buf, sizeof(buf), &n) && n > 0) {
        if (fwrite(buf, 1, n, f) != n) { ok = false; err = "disk write failed"; break; }
        h.update(buf, n);
        got += n;
        if (got > expect_size) { ok = false; err = "file larger than the manifest says"; break; }
        const DWORD now = GetTickCount();
        if (now - last_ui > 100) {
            last_ui = now;
            const double secs = (now - t0) / 1000.0;
            char txt[160];
            snprintf(txt, sizeof(txt), "%s  %.1f / %.1f MB  %.1f MB/s", label,
                     got / 1048576.0, expect_size / 1048576.0, secs > 0.2 ? got / 1048576.0 / secs : 0.0);
            set_status(PAM_DOWNLOADING, txt, total ? (float)((double)(done_base + got) / (double)total) : 0.f);
        }
    }
    fclose(f);
    InternetCloseHandle(hu);
    InternetCloseHandle(hi);
    if (ok && got != expect_size) { ok = false; err = "transfer ended early"; }
    if (!ok) { DeleteFileA(path); return false; }
    sha_out = h.hex();
    return true;
}

// ---------------------------------------------------------------- the job
DWORD WINAPI worker(LPVOID) {
    char base[MAX_PATH];
    if (!game_dir(base, sizeof(base))) { set_status(PAM_ERROR, "cannot locate the game folder"); goto out; }
    {
        set_status(PAM_CHECKING, "Fetching the PAM manifest...", 0.f);
        std::string text, err;
        if (!http_get_text(g_pam_install_config.manifest_url, text, err)) {
            char b[200]; snprintf(b, sizeof(b), "Manifest: %s", err.c_str());
            logger::logf("pam_install: %s (%s)", b, g_pam_install_config.manifest_url);
            set_status(PAM_ERROR, b); goto out;
        }
        Manifest m;
        if (!parse_manifest(text, m, err)) {
            char b[200]; snprintf(b, sizeof(b), "Manifest: %s", err.c_str());
            logger::logf("pam_install: %s", b);
            set_status(PAM_ERROR, b); goto out;
        }
        EnterCriticalSection(&g_cs);
        snprintf(g_status.mod, sizeof(g_status.mod), "%s", m.mod.c_str());
        LeaveCriticalSection(&g_cs);
        logger::logf("pam_install: manifest %s v%d, %d file(s), %d removal(s)",
                     m.mod.c_str(), m.version, (int)m.files.size(), (int)m.removes.size());

        char moddir[MAX_PATH];
        snprintf(moddir, sizeof(moddir), "%s%s\\", base, m.mod.c_str());
        CreateDirectoryA(moddir, NULL);

        // pass 1: what is already right (hash the local copies)
        std::vector<int> todo;
        unsigned long long total = 0;
        for (size_t i = 0; i < m.files.size(); ++i) {
            const ManifestFile& f = m.files[i];
            char txt[160]; snprintf(txt, sizeof(txt), "Checking %s", f.name.c_str());
            set_status(PAM_CHECKING, txt, (float)i / (float)m.files.size());
            char path[MAX_PATH]; snprintf(path, sizeof(path), "%s%s", moddir, f.name.c_str());
            unsigned long long sz = 0; std::string hex;
            if (file_sha256(path, &sz, hex) && sz == f.size && hex == f.sha) continue;
            // a .new from a previous locked install that already matches: nothing to fetch
            char pnew[MAX_PATH]; snprintf(pnew, sizeof(pnew), "%s.new", path);
            if (file_sha256(pnew, &sz, hex) && sz == f.size && hex == f.sha) continue;
            todo.push_back((int)i);
            total += f.size;
        }
        set_counts(0, (int)todo.size());

        // pass 2: fetch
        unsigned long long done = 0;
        int restart_needed = 0, fetched = 0;
        for (size_t k = 0; k < todo.size(); ++k) {
            const ManifestFile& f = m.files[todo[k]];
            char path[MAX_PATH], part[MAX_PATH];
            snprintf(path, sizeof(path), "%s%s", moddir, f.name.c_str());
            snprintf(part, sizeof(part), "%s.part", path);
            char label[120]; snprintf(label, sizeof(label), "%d/%d  %s", (int)k + 1, (int)todo.size(), f.name.c_str());
            std::string hex;
            if (!http_get_file(f.url.c_str(), part, f.size, done, total, label, hex, err)) {
                char b[200]; snprintf(b, sizeof(b), "%s: %s", f.name.c_str(), err.c_str());
                logger::logf("pam_install: %s (%s)", b, f.url.c_str());
                set_status(PAM_ERROR, b); goto out;
            }
            if (hex != f.sha) {
                DeleteFileA(part);
                char b[200]; snprintf(b, sizeof(b), "%s: checksum mismatch, discarded", f.name.c_str());
                logger::logf("pam_install: %s", b);
                set_status(PAM_ERROR, b); goto out;
            }
            if (!MoveFileExA(part, path, MOVEFILE_REPLACE_EXISTING)) {
                // the engine holds the old pk3 open (this mod is the active fs_game):
                // stage it, swapped at the next launch
                char pnew[MAX_PATH]; snprintf(pnew, sizeof(pnew), "%s.new", path);
                if (!MoveFileExA(part, pnew, MOVEFILE_REPLACE_EXISTING)) {
                    char b[200]; snprintf(b, sizeof(b), "%s: cannot place the file", f.name.c_str());
                    logger::logf("pam_install: %s", b);
                    set_status(PAM_ERROR, b); goto out;
                }
                ++restart_needed;
                logger::logf("pam_install: %s is in use, staged as .new (applied at next launch)", f.name.c_str());
            }
            done += f.size;
            ++fetched;
            set_counts(fetched, (int)todo.size());
        }
        // removals: the manifest's explicit `remove` list (old revisions the admin dropped).
        // Best effort, never fatal. No blind folder sweep: a correct+complete manifest is
        // what makes a client pure; the engine's leftover auto-download copies (name.<crc>.pk3)
        // are harmless duplicates (a server the client connects to loads the matching plain
        // pk3), and deleting unrelated content the player keeps here would be wrong.
        int removed = 0;
        for (size_t i = 0; i < m.removes.size(); ++i) {
            char path[MAX_PATH]; snprintf(path, sizeof(path), "%s%s", moddir, m.removes[i].c_str());
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && DeleteFileA(path)) ++removed;
        }
        char b[200];
        if (restart_needed)
            snprintf(b, sizeof(b), "%d file(s) staged - restart the game to finish (mod in use)", restart_needed);
        else if (fetched == 0)
            snprintf(b, sizeof(b), "PAM is up to date (%d files, v%d)", (int)m.files.size(), m.version);
        else
            snprintf(b, sizeof(b), "PAM ready: %d file(s) installed, %.0f MB (v%d)", fetched, total / 1048576.0, m.version);
        logger::logf("pam_install: %s%s", b, removed ? " (+ stale files removed)" : "");
        set_status(restart_needed ? PAM_RESTART : PAM_DONE, b, 1.f);
    }
out:
    EnterCriticalSection(&g_cs);
    g_running = false;
    LeaveCriticalSection(&g_cs);
    return 0;
}

}  // namespace

void pam_install_start() {
    cs_init();
    if (!g_pam_install_config.enable || !g_pam_install_config.manifest_url[0]) {
        set_status(PAM_ERROR, "PAM download disabled in cod1reloaded.ini");
        return;
    }
    EnterCriticalSection(&g_cs);
    if (g_running) { LeaveCriticalSection(&g_cs); return; }
    g_running = true;
    g_status.state = PAM_CHECKING;
    g_status.progress = 0.f;
    g_status.files_done = g_status.files_total = 0;
    snprintf(g_status.text, sizeof(g_status.text), "Starting...");
    LeaveCriticalSection(&g_cs);
    HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!h) {
        EnterCriticalSection(&g_cs);
        g_running = false;
        LeaveCriticalSection(&g_cs);
        set_status(PAM_ERROR, "cannot start the download thread");
        return;
    }
    CloseHandle(h);
}

void pam_install_status(PamStatus* out) {
    cs_init();
    EnterCriticalSection(&g_cs);
    *out = g_status;
    LeaveCriticalSection(&g_cs);
}

bool pam_install_running() {
    cs_init();
    EnterCriticalSection(&g_cs);
    const bool r = g_running;
    LeaveCriticalSection(&g_cs);
    return r;
}

// DllMain, before the engine opens any pak: every <game>\<dir>\*.pk3.new replaces its pk3.
void pam_install_swap_pending() {
    char base[MAX_PATH];
    if (!game_dir(base, sizeof(base))) return;
    char pat[MAX_PATH]; snprintf(pat, sizeof(pat), "%s*", base);
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    int swapped = 0;
    do {
        if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == '.') continue;
        char pat2[MAX_PATH]; snprintf(pat2, sizeof(pat2), "%s%s\\*.pk3.new", base, d.cFileName);
        WIN32_FIND_DATAA e;
        HANDLE h2 = FindFirstFileA(pat2, &e);
        if (h2 == INVALID_HANDLE_VALUE) continue;
        do {
            char from[MAX_PATH], to[MAX_PATH];
            snprintf(from, sizeof(from), "%s%s\\%s", base, d.cFileName, e.cFileName);
            snprintf(to, sizeof(to), "%s", from);
            to[strlen(to) - 4] = 0;                       // strip ".new"
            if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) ++swapped;
        } while (FindNextFileA(h2, &e));
        FindClose(h2);
    } while (FindNextFileA(h, &d));
    FindClose(h);
    if (swapped) logger::logf("pam_install: %d staged pk3(s) swapped in from the previous session", swapped);
}

}  // namespace patches
