// See single_instance.h for the why (AMD alt-tab -> two CoDMP.exe).

#include "core/single_instance.h"
#include "core/logger.h"

#include <cstdio>
#include <cstring>

namespace patches {

namespace {

HANDLE g_mutex = NULL;   // held for the process lifetime, released by Windows on exit

// One mutex per INSTALL, not per machine: enzo runs a dev folder and the stock
// 1.5 install at the same time on purpose, and a global name would break that.
// Case-insensitive FNV-1a of the exe path, since Windows paths are.
void mutex_name(char* out, size_t n) {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    unsigned int h = 2166136261u;
    for (const char* p = path; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        h = (h ^ c) * 16777619u;
    }
    snprintf(out, n, "Local\\cod1reloaded_instance_%08x", h);
}

bool cmdline_is_monkey_relaunch() {
    // CoDMP.exe relaunches itself as: <exe> monkey <parentpid> <handle> "<args>"
    const char* cl = GetCommandLineA();
    if (!cl) return false;
    const char* p = strstr(cl, "monkey");
    if (!p) return false;
    // must be its own token, not part of a path or a player name
    if (p != cl && p[-1] != ' ' && p[-1] != '"') return false;
    char after = p[6];
    return after == ' ' || after == '\0';
}

// The relaunch passes the parent PID; the child is supposed to wait for it.
// If the parent is wedged (the AMD case) nothing ever exits, so give it a
// bounded grace period and then take it down - that is the whole point of the
// relaunch, and leaving both alive is strictly worse than a hard kill.
void reap_parent_from_cmdline() {
    const char* p = strstr(GetCommandLineA(), "monkey");
    if (!p) return;
    unsigned int pid = 0;
    if (sscanf(p + 6, " %u", &pid) != 1 || pid == 0) return;
    if (pid == GetCurrentProcessId()) return;

    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h) return;                       // already gone: nothing to do
    DWORD w = WaitForSingleObject(h, 5000);
    if (w == WAIT_TIMEOUT) {
        logger::logf("single_instance: parent pid %u still alive after 5s "
                     "(wedged relaunch) -> terminating it", pid);
        TerminateProcess(h, 0);
    }
    CloseHandle(h);
}

// The already-running instance is found by WINDOW CLASS, not by pid: we have no
// handle on the other process, and the class is stable ("CoDMP" is what the
// engine registers - verified in the exe's string table alongside CoDSP/CoDHost).
BOOL CALLBACK focus_existing(HWND hwnd, LPARAM) {
    char cls[32] = {0};
    if (!GetClassNameA(hwnd, cls, sizeof(cls))) return TRUE;
    if (strcmp(cls, "CoDMP") != 0) return TRUE;
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner == GetCurrentProcessId()) return TRUE;   // never our own
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    return FALSE;                          // stop at the first match
}

}  // namespace

void single_instance_guard() {
    char name[64];
    mutex_name(name, sizeof(name));

    const bool monkey = cmdline_is_monkey_relaunch();

    // Create first, THEN look at the error: this is the only race-free order.
    g_mutex = CreateMutexA(NULL, FALSE, name);
    const bool already = (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS);

    if (monkey) {
        // Legit engine relaunch. Never block it - just make sure the old process
        // actually dies, so we do not end up with the two-CoDMP state.
        logger::logf("single_instance: engine relaunch (monkey) - allowed");
        reap_parent_from_cmdline();
        return;
    }

    if (!already) return;                  // first instance: carry on

    logger::logf("single_instance: another CoDMP.exe is already running for this "
                 "install - refusing to start a second one");

    // Bring the one that IS running back to the front: on AMD the player usually
    // relaunches precisely because the first window looks dead after an alt-tab.
    EnumWindows(focus_existing, 0);

    MessageBoxA(NULL,
        "Call of Duty is already running.\n\n"
        "A second copy would share the same config and log files and break both.\n"
        "Use the window that is already open (Alt+Tab).",
        "COD1.6X", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);

    ExitProcess(0);
}

}  // namespace patches
