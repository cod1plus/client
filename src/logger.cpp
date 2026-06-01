#include "logger.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>

namespace logger {

namespace {

char g_log_path[MAX_PATH] = {0};
CRITICAL_SECTION g_log_lock;
bool g_lock_initialized = false;

void resolve_log_path(HMODULE self_module) {
    char dll_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(self_module, dll_path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return;

    char* slash = strrchr(dll_path, '\\');
    if (!slash) return;
    *(slash + 1) = '\0';

    snprintf(g_log_path, sizeof(g_log_path), "%scod1reloaded.log", dll_path);
}

}  // namespace

void init(HMODULE self_module) {
    if (!g_lock_initialized) {
        InitializeCriticalSection(&g_log_lock);
        g_lock_initialized = true;
    }
    resolve_log_path(self_module);

    // Truncate at startup so each game session starts fresh.
    FILE* f = fopen(g_log_path, "w");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "=== cod1reloaded log opened %04d-%02d-%02d %02d:%02d:%02d ===\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fclose(f);
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
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\n",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);

    if (g_lock_initialized) EnterCriticalSection(&g_log_lock);
    FILE* f = fopen(g_log_path, "a");
    if (f) {
        fputs(line, f);
        fclose(f);
    }
    if (g_lock_initialized) LeaveCriticalSection(&g_log_lock);

    // Aussi vers DebugView pour debug en live.
    OutputDebugStringA(line);
}

}  // namespace logger
