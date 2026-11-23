// workshop.cpp - the Workshop catalog and its downloads. See workshop.h.

#include "features/workshop.h"
#include "features/pam_install.h"
#include "core/logger.h"

#include <windows.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace patches {

WorkshopConfig g_workshop_config = {
    /* enable       */ true,
    // The public catalog: workshop.json in the cod1plus/workshop repository, the pk3s as
    // its release assets (one release per item version). The ini key overrides it (a
    // test catalog, a mirror); an empty key keeps this.
    /* manifest_url */ "https://raw.githubusercontent.com/cod1plus/workshop/main/workshop.json",
};

namespace {

constexpr size_t MAX_CATALOG_BYTES = 512 * 1024;
constexpr size_t MAX_THUMB_BYTES   = 4 * 1024 * 1024;

CRITICAL_SECTION g_cs;
bool g_cs_init = false;

WsCatalogInfo            g_info;
std::vector<WsItem>      g_items;
unsigned                 g_gen = 0;           // bumped by every refresh: an older worker stops publishing
bool                     g_refreshing = false;
std::vector<std::string> g_queue;             // ids waiting for the install worker
bool                     g_installing = false;

struct HashMemo { unsigned long long size; FILETIME mtime; std::string sha; };
std::map<std::string, HashMemo> g_hashes;     // path -> last hash (a 200 MB pk3 is hashed once)

void cs_init() {
    if (g_cs_init) return;
    InitializeCriticalSection(&g_cs);
    g_cs_init = true;
}

struct Lock {
    Lock()  { cs_init(); EnterCriticalSection(&g_cs); }
    ~Lock() { LeaveCriticalSection(&g_cs); }
};

WsItem* find_item(const std::string& id) {
    for (size_t i = 0; i < g_items.size(); ++i) if (g_items[i].id == id) return &g_items[i];
    return nullptr;
}

// ---------------------------------------------------------------- JSON (just enough)
struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* get(const char* k) const {
        if (t != OBJ) return nullptr;
        for (size_t i = 0; i < o.size(); ++i) if (o[i].first == k) return &o[i].second;
        return nullptr;
    }
};

struct JParser {
    const char* p;
    const char* e;
    std::string err;

    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
               out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& out) {
        if (p >= e || *p != '"') return fail("string expected");
        ++p;
        while (p < e && *p != '"') {
            const char c = *p++;
            if ((unsigned char)c < 0x20) return fail("control character inside a string");
            if (c != '\\') { out += c; continue; }
            if (p >= e) break;
            const char x = *p++;
            switch (x) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return fail("bad \\u escape");
                if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo;
                    if (!hex4(lo)) return fail("bad \\u escape");
                    if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: return fail("bad escape in a string");
            }
        }
        if (p >= e) return fail("unterminated string");
        ++p;
        return true;
    }
    bool word(const char* w) {
        const size_t n = strlen(w);
        if ((size_t)(e - p) < n || memcmp(p, w, n) != 0) return false;
        p += n;
        return true;
    }
    bool value(JVal& v, int depth) {
        if (depth > 32) return fail("nesting too deep");
        ws();
        if (p >= e) return fail("unexpected end");
        const char c = *p;
        if (c == '{') {
            ++p; v.t = JVal::OBJ; ws();
            if (p < e && *p == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p >= e || *p != ':') return fail("':' expected");
                ++p;
                v.o.emplace_back(std::move(k), JVal());
                if (!value(v.o.back().second, depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return true; }
                return fail("',' or '}' expected");
            }
        }
        if (c == '[') {
            ++p; v.t = JVal::ARR; ws();
            if (p < e && *p == ']') { ++p; return true; }
            for (;;) {
                v.a.emplace_back();
                if (!value(v.a.back(), depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return true; }
                return fail("',' or ']' expected");
            }
        }
        if (c == '"') { v.t = JVal::STR; return str(v.s); }
        if (word("true"))  { v.t = JVal::BOOL; v.b = true;  return true; }
        if (word("false")) { v.t = JVal::BOOL; v.b = false; return true; }
        if (word("null"))  { v.t = JVal::NUL; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            char* end = nullptr;
            v.n = strtod(p, &end);            // the text is a std::string: NUL-terminated past e
            if (!end || end == p || end > e) return fail("bad number");
            v.t = JVal::NUM;
            p = end;
            return true;
        }
        return fail("unexpected character");
    }
};

bool json_parse(const std::string& text, JVal& root, std::string& err) {
    JParser ps;
    ps.p = text.c_str();
    ps.e = ps.p + text.size();
    if (ps.e - ps.p >= 3 && !memcmp(ps.p, "\xEF\xBB\xBF", 3)) ps.p += 3;   // BOM
    if (!ps.value(root, 0)) { err = ps.err; return false; }
    ps.ws();
    if (ps.p != ps.e) { err = "text after the end of the JSON"; return false; }
    return true;
}

std::string jstr(const JVal& o, const char* k, size_t maxlen) {
    const JVal* v = o.get(k);
    std::string s;
    if (v && v->t == JVal::STR) s = v->s;
    else if (v && v->t == JVal::NUM) {
        char b[32];
        if (v->n == (double)(long long)v->n) snprintf(b, sizeof(b), "%lld", (long long)v->n);
        else snprintf(b, sizeof(b), "%g", v->n);
        s = b;
    }
    if (s.size() > maxlen) {
        size_t cut = maxlen;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;   // not inside a UTF-8 sequence
        s.resize(cut);
    }
    return s;
}

// ---------------------------------------------------------------- URLs and paths
bool starts_with_ci(const std::string& s, const char* pre) {
    const size_t n = strlen(pre);
    return s.size() >= n && _strnicmp(s.c_str(), pre, n) == 0;
}
bool is_file_url(const std::string& u) { return starts_with_ci(u, "file://"); }
bool is_http_url(const std::string& u) { return starts_with_ci(u, "http://") || starts_with_ci(u, "https://"); }

// file:///C:/a%20b/c.json -> C:\a b\c.json
std::string file_url_path(const std::string& u) {
    std::string s = u.substr(7);
    if (!s.empty() && s[0] == '/' && s.size() > 2 && s[2] == ':') s.erase(0, 1);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            char h[3] = { s[i + 1], s[i + 2], 0 };
            c = (char)strtol(h, nullptr, 16);
            i += 2;
        }
        out += c == '/' ? '\\' : c;
    }
    return out;
}

// a link in the catalog: absolute, or relative to the catalog's own address
std::string resolve_url(const std::string& base, const std::string& rel) {
    if (rel.empty() || rel.find("://") != std::string::npos) return rel;
    const size_t cut = base.find_last_of("/\\");
    return cut == std::string::npos ? rel : base.substr(0, cut + 1) + rel;
}

std::string ansi_to_utf8(const std::string& s) {
    wchar_t w[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w, MAX_PATH * 2)) return s;
    char u[MAX_PATH * 4];
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), NULL, NULL)) return s;
    return u;
}

bool exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string data_dir() {            // <game>\cod1reloaded\workshop\ (created)
    char base[MAX_PATH];
    if (!pkg_game_dir(base, sizeof(base))) return "";
    std::string d = std::string(base) + "cod1reloaded\\";
    CreateDirectoryA(d.c_str(), NULL);
    d += "workshop\\";
    CreateDirectoryA(d.c_str(), NULL);
    return d;
}

bool read_file(const std::string& path, size_t max, std::string& out, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    out.clear();
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
        if (out.size() > max) { fclose(f); err = "file too large"; return false; }
    }
    fclose(f);
    if (out.empty()) { err = "empty file"; return false; }
    return true;
}

bool write_file(const std::string& path, const std::string& data) {
    const std::string part = path + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok || !MoveFileExA(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(part.c_str()); return false; }
    return true;
}

bool fetch_text(const std::string& url, size_t max, std::string& out, std::string& err) {
    if (is_file_url(url)) return read_file(file_url_path(url), max, out, err);
    if (is_http_url(url)) return pkg_fetch_text(url.c_str(), max, out, err);
    err = "unsupported link (http, https or file only)";
    return false;
}

unsigned fnv1a(const std::string& s) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < s.size(); ++i) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

// The memo is kept on disk too (hashes.txt, "size mtime_hi mtime_lo sha path" per line,
// appended; a later line wins): the HD pack is 5.6 GB, hashed once, not at every launch.
bool g_memo_loaded = false;

void load_memo() {
    if (g_memo_loaded) return;
    g_memo_loaded = true;
    std::string text, err;
    if (!read_file(data_dir() + "hashes.txt", 4 * 1024 * 1024, text, err)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        unsigned long long size = 0;
        unsigned long hi = 0, lo = 0;
        char sha[72] = "";
        int off = 0;
        if (sscanf(line.c_str(), "%llu %lu %lu %71s %n", &size, &hi, &lo, sha, &off) >= 4 && off > 0 && strlen(sha) == 64) {
            FILETIME ft; ft.dwHighDateTime = hi; ft.dwLowDateTime = lo;
            g_hashes[line.substr(off)] = HashMemo{ size, ft, sha };
        }
    }
}

// SHA-256 of a file, memoised on (size, mtime)
bool hash_file(const std::string& path, std::string& sha) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return false;
    const unsigned long long size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    {
        Lock l;
        load_memo();
        auto it = g_hashes.find(path);
        if (it != g_hashes.end() && it->second.size == size &&
            !CompareFileTime(&it->second.mtime, &d.ftLastWriteTime)) { sha = it->second.sha; return true; }
    }
    unsigned long long got = 0;
    char hex[65];
    if (!pkg_file_sha256(path.c_str(), &got, hex)) return false;
    sha = hex;
    Lock l;
    g_hashes[path] = HashMemo{ got, d.ftLastWriteTime, sha };
    if (FILE* f = fopen((data_dir() + "hashes.txt").c_str(), "ab")) {
        fprintf(f, "%llu %lu %lu %s %s\n", got, d.ftLastWriteTime.dwHighDateTime, d.ftLastWriteTime.dwLowDateTime,
                sha.c_str(), path.c_str());
        fclose(f);
    }
    return true;
}

// what installed.txt records for an item: its sha256, or for a pack one key over all files
std::string set_key(const std::vector<std::string>& shas) {
    if (shas.size() == 1) return shas[0];
    std::string all;
    for (size_t i = 0; i < shas.size(); ++i) all += shas[i];
    char k[24];
    snprintf(k, sizeof(k), "set-%08x%02x", fnv1a(all), (unsigned)shas.size());
    return k;
}

// installed.txt: "<id> <version> <sha256>" per line, what our installs put on disk
std::map<std::string, std::pair<std::string, std::string>> read_installed() {
    std::map<std::string, std::pair<std::string, std::string>> m;
    std::string text, err;
    if (!read_file(data_dir() + "installed.txt", 256 * 1024, text, err)) return m;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        char id[64] = "", ver[32] = "", sha[72] = "";
        if (sscanf(text.substr(pos, eol - pos).c_str(), "%63s %31s %71s", id, ver, sha) == 3)
            m[id] = std::make_pair(std::string(ver), std::string(sha));
        pos = eol + 1;
    }
    return m;
}

void note_installed(const std::string& id, const std::string& version, const std::string& sha) {
    auto m = read_installed();
    std::string ver = version.empty() ? "-" : version;
    for (size_t i = 0; i < ver.size(); ++i) if (isspace((unsigned char)ver[i])) ver[i] = '_';
    m[id] = std::make_pair(ver, sha);
    std::string out = "# cod1reloaded workshop: what was installed from the catalog (id version sha256)\n";
    for (auto& kv : m) out += kv.first + " " + kv.second.first + " " + kv.second.second + "\n";
    write_file(data_dir() + "installed.txt", out);
}

// ---------------------------------------------------------------- the catalog
bool valid_id(const std::string& s) {
    if (s.empty() || s.size() > 48) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

// the game's own paks are never a catalog's to replace
bool stock_pak(const std::string& f) {
    if (_strnicmp(f.c_str(), "pak", 3) == 0 && f.size() > 3 && isdigit((unsigned char)f[3])) return true;
    return _strnicmp(f.c_str(), "localized_", 10) == 0;
}

// a file of a "hud" item (the minimap's radar images): <name>.png / <name>.json, nothing else
bool hud_file(const std::string& f) {
    if (f.size() < 6 || f.size() > 64) return false;
    for (char c : f) if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
    const size_t dot = f.find('.');
    if (dot == 0 || dot != f.rfind('.')) return false;
    return !_stricmp(f.c_str() + dot, ".png") || !_stricmp(f.c_str() + dot, ".json");
}

bool parse_catalog(const std::string& text, const std::string& base, bool base_is_file,
                   std::string& title, std::vector<WsItem>& items, std::string& err) {
    JVal root;
    if (!json_parse(text, root, err)) return false;
    if (root.t != JVal::OBJ) { err = "the catalog is not a JSON object"; return false; }
    title = jstr(root, "title", 64);
    const JVal* arr = root.get("items");
    if (!arr || arr->t != JVal::ARR) { err = "the catalog has no \"items\" list"; return false; }
    for (size_t i = 0; i < arr->a.size() && items.size() < 200; ++i) {
        const JVal& o = arr->a[i];
        if (o.t != JVal::OBJ) continue;
        WsItem it;
        it.id          = jstr(o, "id", 48);
        it.type        = jstr(o, "type", 8);
        it.name        = jstr(o, "name", 64);
        it.author      = jstr(o, "author", 64);
        it.version     = jstr(o, "version", 24);
        it.description = jstr(o, "description", 1500);
        it.folder      = jstr(o, "folder", 48);
        it.thumb_url   = resolve_url(base, jstr(o, "thumbnail", 512));
        if (it.type != "mod" && it.type != "hud") it.type = "map";
        if (it.folder.empty()) it.folder = "main";
        if (it.name.empty()) it.name = it.id;

        // the pk3(s): a "files" list, or the item's own file/size/sha256/url
        auto read_file_entry = [&](const JVal& f) {
            WsFile w;
            w.file   = jstr(f, "file", 96);
            w.sha256 = jstr(f, "sha256", 64);
            w.url    = resolve_url(base, jstr(f, "url", 512));
            const JVal* sz = f.get("size");
            if (sz && sz->t == JVal::NUM && sz->n > 0) w.size = (unsigned long long)sz->n;
            else if (sz && sz->t == JVal::STR) w.size = _strtoui64(sz->s.c_str(), nullptr, 10);
            for (size_t k = 0; k < w.sha256.size(); ++k) w.sha256[k] = (char)tolower((unsigned char)w.sha256[k]);
            it.files.push_back(w);
        };
        const JVal* fl = o.get("files");
        if (fl && fl->t == JVal::ARR) { for (size_t k = 0; k < fl->a.size() && k < 32; ++k) if (fl->a[k].t == JVal::OBJ) read_file_entry(fl->a[k]); }
        else read_file_entry(o);
        // cvars: plain tokens only (as pam_install's manifest): nothing a console line could hide in
        const JVal* cv = o.get("cvars");
        if (cv && cv->t == JVal::OBJ) {
            for (size_t k = 0; k < cv->o.size() && it.cvars.size() < 4; ++k) {
                const std::string& n = cv->o[k].first;
                const JVal& jv = cv->o[k].second;
                std::string v;
                if (jv.t == JVal::STR) v = jv.s;
                else if (jv.t == JVal::NUM) { char b[32]; snprintf(b, sizeof(b), "%lld", (long long)jv.n); v = b; }
                bool ok = pkg_valid_folder(n.c_str()) && !v.empty() && v.size() < 32;
                for (size_t c = 0; ok && c < v.size(); ++c)
                    ok = isalnum((unsigned char)v[c]) || v[c] == '.' || v[c] == '-' || v[c] == '_';
                if (ok) it.cvars.push_back(std::make_pair(n, v));
                else logger::logf("workshop: %s: cvar %s refused", it.id.c_str(), n.c_str());
            }
        }
        // play: a map name and a gametype, plain tokens - the tab writes the devmap line itself
        const JVal* pl = o.get("play");
        if (pl && pl->t == JVal::OBJ && it.type == "map") {
            auto token = [](const std::string& s, size_t maxlen) {
                if (s.empty() || s.size() > maxlen) return false;
                for (char c : s) if (!(isalnum((unsigned char)c) || c == '_')) return false;
                return true;
            };
            const std::string m = jstr(*pl, "map", 64), g = jstr(*pl, "gametype", 16);
            if (token(m, 63) && (g.empty() || token(g, 15))) { it.play_map = m; it.play_gametype = g; }
            else logger::logf("workshop: %s: play refused", it.id.c_str());
        }

        const char* bad = nullptr;
        if (!valid_id(it.id)) bad = "id";
        else if (!pkg_valid_folder(it.folder.c_str())) bad = "folder";
        else if (it.type == "hud" && _stricmp(it.folder.c_str(), "minimaps") != 0) bad = "folder (a hud item goes to minimaps)";
        else if (it.files.empty()) bad = "files";
        for (size_t k = 0; !bad && k < it.files.size(); ++k) {
            const WsFile& w = it.files[k];
            if (it.type == "hud" ? !hud_file(w.file) : (!pkg_valid_pk3_name(w.file.c_str()) || stock_pak(w.file))) bad = "file";
            else if (w.size == 0) bad = "size";
            else if (w.sha256.size() != 64 || w.sha256.find_first_not_of("0123456789abcdef") != std::string::npos) bad = "sha256";
            else if (!(is_http_url(w.url) || (is_file_url(w.url) && base_is_file))) bad = "url";
            for (size_t j = 0; !bad && j < k; ++j) if (!_stricmp(it.files[j].file.c_str(), w.file.c_str())) bad = "file (listed twice)";
            it.size += w.size;
        }
        if (!bad) for (size_t k = 0; k < items.size(); ++k) if (items[k].id == it.id) bad = "id (duplicate)";
        if (bad) { logger::logf("workshop: catalog item %d (%s) skipped - bad %s", (int)i, it.id.c_str(), bad); continue; }
        if (!it.thumb_url.empty() && !(is_http_url(it.thumb_url) || (is_file_url(it.thumb_url) && base_is_file)))
            it.thumb_url.clear();
        items.push_back(it);
    }
    if (title.empty()) title = "1.6X Workshop";
    return true;
}

// what is on disk for this item, by hash: every file right = installed, the missing
// ones only staged (.new) = restart, nothing there = absent, anything else = update
void check_local(WsItem& it, const std::map<std::string, std::pair<std::string, std::string>>& noted) {
    char base[MAX_PATH];
    if (!pkg_game_dir(base, sizeof(base))) { it.local = WS_ABSENT; return; }
    int ok = 0, staged = 0, present = 0;
    std::vector<std::string> local_shas;
    for (size_t i = 0; i < it.files.size(); ++i) {
        const std::string path = std::string(base) + it.folder + "\\" + it.files[i].file;
        std::string sha;
        if (exists(path + ".new") && hash_file(path + ".new", sha) && sha == it.files[i].sha256) { ++staged; ++present; continue; }
        if (!exists(path)) continue;
        ++present;
        if (hash_file(path, sha)) {
            local_shas.push_back(sha);
            if (sha == it.files[i].sha256) ++ok;
        }
    }
    const int n = (int)it.files.size();
    it.local = ok == n ? WS_INSTALLED : (ok + staged == n ? WS_STAGED : (present == 0 ? WS_ABSENT : WS_OUTDATED));
    auto rec = noted.find(it.id);
    if (rec != noted.end() && (int)local_shas.size() == n && rec->second.second == set_key(local_shas) && rec->second.first != "-")
        it.local_version = rec->second.first;
}

void publish_local(unsigned gen, const WsItem& it) {
    Lock l;
    if (gen != g_gen) return;
    WsItem* t = find_item(it.id);
    if (!t) return;
    t->local = it.local;
    t->local_version = it.local_version;
}

// <game>\cod1reloaded\workshop\thumbs\<id>-<hash of the link>.<ext>: a new picture = a new name
std::string fetch_thumb(const WsItem& it, const std::string& dir) {
    std::string ext = ".jpg";
    const size_t dot = it.thumb_url.find_last_of('.');
    if (dot != std::string::npos) {
        std::string e = it.thumb_url.substr(dot);
        for (size_t k = 0; k < e.size(); ++k) e[k] = (char)tolower((unsigned char)e[k]);
        if (e == ".png" || e == ".jpeg" || e == ".bmp") ext = e;
    }
    char name[96];
    snprintf(name, sizeof(name), "%s-%08x%s", it.id.c_str(), fnv1a(it.thumb_url), ext.c_str());
    const std::string path = dir + name;
    if (exists(path)) return path;
    std::string data, err;
    if (!fetch_text(it.thumb_url, MAX_THUMB_BYTES, data, err)) {
        logger::logf("workshop: thumbnail of %s: %s (%s)", it.id.c_str(), err.c_str(), it.thumb_url.c_str());
        return "";
    }
    if (!write_file(path, data)) return "";
    return path;
}

DWORD WINAPI catalog_worker(LPVOID p) {
    const unsigned gen = (unsigned)(uintptr_t)p;
    const std::string url = g_workshop_config.manifest_url;
    const bool local = is_file_url(url);
    const std::string dir = data_dir();
    std::string text, err, title;
    std::vector<WsItem> items;
    int state = WSC_READY;
    std::string line;

    if (fetch_text(url, MAX_CATALOG_BYTES, text, err) && parse_catalog(text, url, local, title, items, err)) {
        if (!dir.empty()) write_file(dir + "catalog.json", text);
        char b[200];
        snprintf(b, sizeof(b), "%d item%s  -  %s", (int)items.size(), items.size() == 1 ? "" : "s",
                 local ? "local catalog file" : url.c_str());
        line = b;
        logger::logf("workshop: catalog %s: %d item(s) (%s)", title.c_str(), (int)items.size(), url.c_str());
    } else {
        logger::logf("workshop: catalog %s: %s", url.c_str(), err.c_str());
        // offline: the last catalog that loaded, so installed items stay visible
        std::string cached, err2;
        items.clear();
        if (!dir.empty() && read_file(dir + "catalog.json", MAX_CATALOG_BYTES, cached, err2) &&
            parse_catalog(cached, url, local, title, items, err2)) {
            state = WSC_CACHED;
            line = "Offline (" + err + ") - showing the last catalog that loaded";
        } else {
            state = WSC_ERROR;
            line = "Catalog: " + err;
        }
    }

    {
        Lock l;
        if (gen != g_gen) { g_refreshing = false; return 0; }
        // a download in flight keeps its progress across the refresh
        for (size_t i = 0; i < items.size(); ++i) {
            const WsItem* old = find_item(items[i].id);
            if (!old) continue;
            items[i].job = old->job;
            items[i].progress = old->progress;
            items[i].job_text = old->job_text;
            items[i].installed_now = old->installed_now;
            items[i].thumb_path = old->thumb_path;
        }
        g_items = items;
        g_info.state = state;
        g_info.title = title;
        g_info.text = line;
        g_info.local = local;
    }

    // pictures, then states (hashing: the HD pack is 5.6 GB the first time) - item by item
    if (!dir.empty()) {
        const std::string tdir = dir + "thumbs\\";
        CreateDirectoryA(tdir.c_str(), NULL);
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].thumb_url.empty()) continue;
            const std::string path = fetch_thumb(items[i], tdir);
            if (path.empty()) continue;
            Lock l;
            if (gen != g_gen) break;
            if (WsItem* t = find_item(items[i].id)) t->thumb_path = ansi_to_utf8(path);
        }
    }
    const auto noted = read_installed();
    for (size_t i = 0; i < items.size(); ++i) {
        check_local(items[i], noted);
        publish_local(gen, items[i]);
    }
    Lock l;
    if (gen == g_gen) g_refreshing = false;
    return 0;
}

// ---------------------------------------------------------------- installs
std::vector<std::pair<std::string, std::string>> g_cvars_out;   // installed items' cvars, for the main thread

// one transfer of a job: `base` bytes of the job done before it, `all` = the job's total
struct Progress { std::string id; unsigned long long base, all; int index, count; };

void on_progress(void* user, unsigned long long got, unsigned long long total, double mbps) {
    const Progress* pr = (const Progress*)user;
    const unsigned long long all = pr->all ? pr->all : total, done = pr->base + got;
    char lead[24] = "";
    if (pr->count > 1) snprintf(lead, sizeof(lead), "%d/%d  ", pr->index + 1, pr->count);
    char txt[128];
    if (mbps > 0) snprintf(txt, sizeof(txt), "%s%.1f / %.1f MB  -  %.1f MB/s", lead, done / 1048576.0, all / 1048576.0, mbps);
    else snprintf(txt, sizeof(txt), "%s%.1f / %.1f MB", lead, done / 1048576.0, all / 1048576.0);
    Lock l;
    if (WsItem* t = find_item(pr->id)) {
        t->job = WSJ_DOWNLOADING;
        t->progress = all ? (float)((double)done / (double)all) : 0.f;
        t->job_text = txt;
    }
}

struct CopyCtx { Progress* pr; DWORD t0, last; };

DWORD CALLBACK copy_progress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER,
                             DWORD, DWORD, HANDLE, HANDLE, LPVOID user) {
    CopyCtx* c = (CopyCtx*)user;
    const DWORD now = GetTickCount();
    if (now - c->last > 100 || done.QuadPart == total.QuadPart) {
        c->last = now;
        const double secs = (now - c->t0) / 1000.0;
        on_progress(c->pr, (unsigned long long)done.QuadPart, (unsigned long long)total.QuadPart,
                    secs > 0.2 ? done.QuadPart / 1048576.0 / secs : 0.0);
    }
    return PROGRESS_CONTINUE;
}

// fetch one file into `part`; sha_hex = what arrived
bool fetch_pk3(const WsFile& f, Progress& pr, const std::string& part, char sha_hex[65], std::string& err) {
    if (is_http_url(f.url))
        return pkg_fetch_file(f.url.c_str(), part.c_str(), f.size, on_progress, &pr, sha_hex, err);
    // file:// (a local test catalog or a LAN share): a plain copy, then the same hash check
    const std::string src = file_url_path(f.url);
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(src.c_str(), GetFileExInfoStandard, &d)) { err = "source file not found"; return false; }
    const unsigned long long size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    if (size != f.size) { err = "the source file is not the size the catalog says"; return false; }
    CopyCtx ctx{ &pr, GetTickCount(), 0 };
    if (!CopyFileExA(src.c_str(), part.c_str(), copy_progress, &ctx, NULL, 0)) {
        char b[64]; snprintf(b, sizeof(b), "copy failed (err %lu)", GetLastError());
        err = b;
        return false;
    }
    unsigned long long got = 0;
    if (!pkg_file_sha256(part.c_str(), &got, sha_hex)) { err = "cannot read the copy back"; return false; }
    return true;
}

void set_job(const std::string& id, int job, const std::string& text, float progress = -1.f) {
    Lock l;
    if (WsItem* t = find_item(id)) {
        t->job = job;
        t->job_text = text;
        if (progress >= 0.f) t->progress = progress;
    }
}

void install_one(const WsItem& it) {
    char base[MAX_PATH];
    if (!pkg_game_dir(base, sizeof(base))) { set_job(it.id, WSJ_ERROR, "cannot locate the game folder"); return; }
    const std::string dir = std::string(base) + it.folder + "\\";
    CreateDirectoryA(dir.c_str(), NULL);
    logger::logf("workshop: installing %s v%s -> %s\\ (%d file(s), %.1f MB)", it.id.c_str(), it.version.c_str(),
                 it.folder.c_str(), (int)it.files.size(), it.size / 1048576.0);
    set_job(it.id, WSJ_DOWNLOADING, "Starting...", 0.f);

    // what is already right stays (a pack updated by one file fetches that file only)
    std::vector<int> todo;
    unsigned long long all = 0;
    for (size_t i = 0; i < it.files.size(); ++i) {
        const std::string path = dir + it.files[i].file;
        std::string sha;
        if (hash_file(path, sha) && sha == it.files[i].sha256) continue;
        if (exists(path + ".new") && hash_file(path + ".new", sha) && sha == it.files[i].sha256) continue;
        todo.push_back((int)i);
        all += it.files[i].size;
    }
    int staged_n = 0;
    unsigned long long done = 0;
    for (size_t k = 0; k < todo.size(); ++k) {
        const WsFile& f = it.files[todo[k]];
        const std::string path = dir + f.file, part = path + ".part", staged = path + ".new";
        Progress pr{ it.id, done, all, (int)k, (int)todo.size() };
        char sha[65];
        std::string err;
        if (!fetch_pk3(f, pr, part, sha, err)) {
            logger::logf("workshop: %s: %s: %s (%s)", it.id.c_str(), f.file.c_str(), err.c_str(), f.url.c_str());
            set_job(it.id, WSJ_ERROR, todo.size() > 1 ? f.file + ": " + err : err);
            return;
        }
        set_job(it.id, WSJ_VERIFYING, "Verifying SHA-256...", -1.f);
        if (f.sha256 != sha) {
            DeleteFileA(part.c_str());
            logger::logf("workshop: %s: %s checksum mismatch (got %.16s, catalog %.16s) - discarded",
                         it.id.c_str(), f.file.c_str(), sha, f.sha256.c_str());
            set_job(it.id, WSJ_ERROR, "Checksum mismatch - the download was discarded");
            return;
        }
        if (MoveFileExA(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            // an older staged copy would overwrite this one at the next launch
            if (exists(staged)) DeleteFileA(staged.c_str());
        } else if (MoveFileExA(part.c_str(), staged.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            ++staged_n;                    // the engine holds the old pk3 open
            logger::logf("workshop: %s is in use, staged as .new (applied at next launch)", f.file.c_str());
        } else {
            DeleteFileA(part.c_str());
            set_job(it.id, WSJ_ERROR, "Cannot place " + f.file + " (folder not writable?)");
            return;
        }
        std::string dummy;
        hash_file(exists(staged) ? staged : path, dummy);   // memo the new file: no rehash on refresh
        done += f.size;
    }
    std::vector<std::string> shas;
    for (size_t i = 0; i < it.files.size(); ++i) shas.push_back(it.files[i].sha256);
    note_installed(it.id, it.version, set_key(shas));
    const int local = staged_n ? WS_STAGED : WS_INSTALLED;
    char b[160];
    if (local == WS_INSTALLED)
        snprintf(b, sizeof(b), "Installed - %.1f MB, SHA-256 verified", it.size / 1048576.0);
    else
        snprintf(b, sizeof(b), "Downloaded and verified - restart the game to finish (the pk3 is in use)");
    logger::logf("workshop: %s: %s", it.id.c_str(), b);
    Lock l;
    for (size_t i = 0; i < it.cvars.size(); ++i) g_cvars_out.push_back(it.cvars[i]);
    if (WsItem* t = find_item(it.id)) {
        t->job = WSJ_DONE;
        t->job_text = b;
        t->progress = 1.f;
        t->local = local;
        t->local_version = it.version;
        t->installed_now = true;
    }
}

DWORD WINAPI install_worker(LPVOID) {
    for (;;) {
        WsItem it;
        {
            Lock l;
            if (g_queue.empty()) { g_installing = false; return 0; }
            const std::string id = g_queue.front();
            g_queue.erase(g_queue.begin());
            const WsItem* t = find_item(id);
            if (!t) continue;
            it = *t;
        }
        install_one(it);
    }
}

}  // namespace

void workshop_refresh() {
    Lock l;
    if (!g_workshop_config.enable || !g_workshop_config.manifest_url[0]) {
        ++g_gen;                       // a load still running publishes nothing
        g_items.clear();
        g_info = WsCatalogInfo();
        g_info.state = g_workshop_config.enable ? WSC_NONE : WSC_DISABLED;
        g_info.text = g_workshop_config.enable
            ? "No catalog configured yet (workshop_manifest_url in cod1reloaded.ini)."
            : "The Workshop is turned off in cod1reloaded.ini (workshop_enable).";
        return;
    }
    if (g_refreshing) return;
    g_refreshing = true;
    ++g_gen;
    if (g_info.state != WSC_READY && g_info.state != WSC_CACHED) g_info.state = WSC_LOADING;
    g_info.text = "Loading the catalog...";
    HANDLE h = CreateThread(NULL, 0, catalog_worker, (LPVOID)(uintptr_t)g_gen, 0, NULL);
    if (!h) { g_refreshing = false; g_info.state = WSC_ERROR; g_info.text = "cannot start the catalog thread"; return; }
    CloseHandle(h);
}

void workshop_snapshot(WsCatalogInfo& info, std::vector<WsItem>& items) {
    Lock l;
    info = g_info;
    info.busy = g_refreshing;
    items = g_items;
}

void workshop_install(const std::string& id) {
    Lock l;
    WsItem* t = find_item(id);
    if (!t || t->job == WSJ_QUEUED || t->job == WSJ_DOWNLOADING || t->job == WSJ_VERIFYING) return;
    t->job = WSJ_QUEUED;
    t->job_text = "Queued";
    t->progress = 0.f;
    g_queue.push_back(id);
    if (g_installing) return;
    g_installing = true;
    HANDLE h = CreateThread(NULL, 0, install_worker, NULL, 0, NULL);
    if (!h) {
        g_installing = false;
        g_queue.clear();
        t->job = WSJ_ERROR;
        t->job_text = "cannot start the download thread";
        return;
    }
    CloseHandle(h);
}

bool workshop_busy() {
    Lock l;
    return g_installing;
}

std::vector<std::pair<std::string, std::string>> workshop_take_cvars() {
    Lock l;
    std::vector<std::pair<std::string, std::string>> out;
    out.swap(g_cvars_out);
    return out;
}

}  // namespace patches
