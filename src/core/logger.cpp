#include "core/logger.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace logger {

// ASYNCHRONOUS since 2026-10-16. Every logf() used to fopen("a") / fputs / fclose the log on
// the calling thread - most of them the game thread, in the middle of a frame (the dev
// pos_log / aim_log lines alone came several times a second). Opening a file is where an
// antivirus scans and where OneDrive takes its locks: a few milliseconds now and then, the
// "longues" frames of the bilan. Now logf() only formats into a string under a short lock
// and wakes a writer thread, which keeps the file open and appends with WriteFile.
//
// Plain Win32 file calls, no CRT stream: flush() runs at process exit, after ExitProcess has
// killed the writer thread wherever it was - possibly inside a CRT lock - so it must not
// need one. It also only TRIES the queue lock, for the same reason.

namespace {

char             g_log_path[MAX_PATH] = {0};
CRITICAL_SECTION g_q_lock;
bool             g_lock_initialized = false;
std::string      g_pending;               // lines not written yet
HANDLE           g_wake = NULL;           // auto-reset: something was queued
HANDLE           g_thread = NULL;
constexpr size_t PENDING_MAX = 4u << 20;  // a stuck disk never eats more than 4 MB
long             g_dropped = 0;

void resolve_log_path(HMODULE self_module) {
    char dll_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(self_module, dll_path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return;

    char* slash = strrchr(dll_path, '\\');
    if (!slash) return;
    *(slash + 1) = '\0';

    snprintf(g_log_path, sizeof(g_log_path), "%scod1reloaded.log", dll_path);
}

HANDLE open_append() {
    return CreateFileA(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void write_all(HANDLE h, const std::string& s) {
    const char* p = s.data();
    size_t left = s.size();
    while (left) {
        DWORD w = 0;
        if (!WriteFile(h, p, (DWORD)(left > (1u << 20) ? (1u << 20) : left), &w, NULL) || !w) return;
        p += w;
        left -= w;
    }
}

DWORD WINAPI writer(LPVOID) {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::string batch;
    for (;;) {
        WaitForSingleObject(g_wake, 1000);
        EnterCriticalSection(&g_q_lock);
        batch.swap(g_pending);
        LeaveCriticalSection(&g_q_lock);
        if (batch.empty()) continue;
        if (h == INVALID_HANDLE_VALUE) h = open_append();
        if (h != INVALID_HANDLE_VALUE) write_all(h, batch);
        OutputDebugStringA(batch.c_str());   // DebugView
        batch.clear();
    }
    return 0;
}

}  // namespace

void init(HMODULE self_module) {
    if (!g_lock_initialized) {
        InitializeCriticalSection(&g_q_lock);
        g_lock_initialized = true;
    }
    resolve_log_path(self_module);

    // Keep the dying session's evidence: the engine's self-relaunch (monkey) used
    // to truncate the log RIGHT AFTER the crash it should explain (AMD alt-tab,
    // 2026-08-25). Rename is atomic and cheap; if the old process is mid-write,
    // fall back to a copy so we never lose the trail.
    if (g_log_path[0]) {
        char prev_path[MAX_PATH];
        snprintf(prev_path, sizeof(prev_path), "%.*sprev.log",
                 (int)(strlen(g_log_path) - 3), g_log_path);
        if (!MoveFileExA(g_log_path, prev_path, MOVEFILE_REPLACE_EXISTING))
            CopyFileA(g_log_path, prev_path, FALSE);
    }

    FILE* f = fopen(g_log_path, "w");  // truncate
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "=== cod1reloaded log opened %04d-%02d-%02d %02d:%02d:%02d ===\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fclose(f);
    }

    // The thread only starts running once DllMain returns (loader lock); lines logged
    // before that just wait in the queue.
    if (!g_wake) g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (g_wake && !g_thread) {
        g_thread = CreateThread(NULL, 0, writer, NULL, 0, NULL);
        if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    }
}

void logf(const char* fmt, ...) {
    if (!g_log_path[0]) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[1200];
    int len = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\n",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    if (len <= 0) return;
    if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;

    if (!g_lock_initialized || !g_thread) {
        // no writer (init failed or not called): the old synchronous path
        FILE* f = fopen(g_log_path, "a");
        if (f) { fputs(line, f); fclose(f); }
        return;
    }
    EnterCriticalSection(&g_q_lock);
    if (g_pending.size() < PENDING_MAX) g_pending.append(line, (size_t)len);
    else ++g_dropped;
    LeaveCriticalSection(&g_q_lock);
    SetEvent(g_wake);
}

void flush() {
    if (!g_lock_initialized || !g_log_path[0]) return;
    std::string batch;
    bool got = false;
    for (int i = 0; i < 50 && !(got = TryEnterCriticalSection(&g_q_lock) != 0); ++i) Sleep(1);
    if (!got) return;                  // the writer died holding it: lose the tail, not the exit
    batch.swap(g_pending);
    LeaveCriticalSection(&g_q_lock);
    if (batch.empty()) return;
    HANDLE h = open_append();
    if (h == INVALID_HANDLE_VALUE) return;
    write_all(h, batch);
    CloseHandle(h);
}

bool dev_logs() {
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("COD1RELOADED_DEV_LOGS");
        s = (e && *e && *e != '0') ? 1 : 0;
    }
    return s == 1;
}

}  // namespace logger
