// news.cpp - see news.h
#include "features/news.h"
#include "core/logger.h"

#include <windows.h>
#include <wininet.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace patches {

NewsConfig g_news_config = {
    /* enable */ true,
    /* url    */ "https://raw.githubusercontent.com/cod1plus/rulesets/main/news.txt",
};

namespace {

CRITICAL_SECTION g_cs;
bool g_cs_init = false;
char g_title[64] = "", g_text[128] = "", g_url[256] = "";
constexpr DWORD REFRESH_MS = 10 * 60 * 1000;

void cs_init() {
    if (g_cs_init) return;
    InitializeCriticalSection(&g_cs);
    g_cs_init = true;
}

bool http_get(const char* url, std::string& out) {
    HINTERNET hi = InternetOpenA("cod1reloaded news", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hi) return false;
    DWORD tmo = 8000;
    InternetSetOptionA(hi, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo));
    HINTERNET hu = InternetOpenUrlA(hi, url, NULL, 0,
                                    INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!hu) { InternetCloseHandle(hi); return false; }
    DWORD rtmo = 15000;
    InternetSetOptionA(hu, INTERNET_OPTION_RECEIVE_TIMEOUT, &rtmo, sizeof(rtmo));
    DWORD status = 0, ssz = sizeof(status);
    bool ok = !(HttpQueryInfoA(hu, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &ssz, NULL) && status != 200);
    out.clear();
    char buf[4096];
    DWORD n = 0;
    while (ok && InternetReadFile(hu, buf, sizeof(buf), &n) && n > 0) {
        out.append(buf, n);
        if (out.size() > 16384) break;           // three lines, not a novel
    }
    InternetCloseHandle(hu);
    InternetCloseHandle(hi);
    if (!ok) logger::logf("news: GET %s -> HTTP %lu", url, status);
    return ok;
}

// printable ASCII/UTF-8 only, control chars dropped (the drawer feeds GDI with it)
void clean_copy(char* dst, size_t n, const std::string& src) {
    size_t k = 0;
    for (size_t i = 0; i < src.size() && k + 1 < n; ++i) {
        const unsigned char c = (unsigned char)src[i];
        if (c >= 0x20 || c == '\t') dst[k++] = (c == '\t') ? ' ' : (char)c;
    }
    dst[k] = 0;
    while (k && dst[k - 1] == ' ') dst[--k] = 0;
}

void parse(const std::string& text) {
    std::string lines[3];
    int got = 0;
    size_t pos = 0;
    while (pos < text.size() && got < 3) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        if (line[s] == '#' || line[s] == ';') continue;
        lines[got++] = line.substr(s);
    }
    char title[64] = "", txt[128] = "", url[256] = "";
    clean_copy(title, sizeof(title), lines[0]);
    clean_copy(txt, sizeof(txt), lines[1]);
    clean_copy(url, sizeof(url), lines[2]);
    // only a web link is ever handed to the shell
    if (url[0] && _strnicmp(url, "https://", 8) != 0 && _strnicmp(url, "http://", 7) != 0) url[0] = 0;
    for (char* p = url; *p; ++p) if (*p == ' ' || *p == '"' || *p == '\'') { *p = 0; break; }
    EnterCriticalSection(&g_cs);
    const bool changed = strcmp(g_title, title) || strcmp(g_text, txt) || strcmp(g_url, url);
    snprintf(g_title, sizeof(g_title), "%s", title);
    snprintf(g_text, sizeof(g_text), "%s", txt);
    snprintf(g_url, sizeof(g_url), "%s", url);
    LeaveCriticalSection(&g_cs);
    if (changed) {
        if (title[0]) logger::logf("news: \"%s\" / \"%s\" / %s", title, txt, url[0] ? url : "(no link)");
        else          logger::logf("news: nothing to show");
    }
}

DWORD WINAPI worker(LPVOID) {
    for (;;) {
        std::string text;
        if (http_get(g_news_config.url, text)) parse(text);
        Sleep(REFRESH_MS);
    }
    return 0;
}

}  // namespace

void news_start() {
    cs_init();
    if (!g_news_config.enable || !g_news_config.url[0]) {
        logger::logf("news: disabled");
        return;
    }
    HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (h) CloseHandle(h);
    logger::logf("news: card from %s (refresh every %lu min)", g_news_config.url, REFRESH_MS / 60000);
}

bool news_get(char* title, size_t tn, char* text, size_t xn, char* url, size_t un) {
    cs_init();
    EnterCriticalSection(&g_cs);
    snprintf(title, tn, "%s", g_title);
    snprintf(text, xn, "%s", g_text);
    snprintf(url, un, "%s", g_url);
    LeaveCriticalSection(&g_cs);
    return title[0] != 0;
}

void news_open_link() {
    char url[256];
    cs_init();
    EnterCriticalSection(&g_cs);
    snprintf(url, sizeof(url), "%s", g_url);
    LeaveCriticalSection(&g_cs);
    if (!url[0]) return;
    logger::logf("news: opening %s", url);
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

}  // namespace patches
