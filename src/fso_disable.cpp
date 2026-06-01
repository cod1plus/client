// Disable Windows 10/11 "Fullscreen Optimization" for CoDMP.exe.
//
// FSO is a Win10+ feature that wraps "true" fullscreen apps in a borderless
// windowed compositor managed by DWM. For modern games it's transparent.
// For older games like CoD1 it adds latency and sometimes causes the
// "windowed when fullscreen requested" bug.
//
// The fix is a registry entry that tells Windows "this exe wants the
// classic exclusive fullscreen path":
//
//   HKCU\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers
//   Value name : <full path to CoDMP.exe>
//   Value data : "~ DISABLEDXMAXIMIZEDWINDOWEDMODE"
//
// We write this on every launch so it stays applied. Effect kicks in at
// the NEXT launch of CoDMP.exe.

#include "fso_disable.h"
#include "logger.h"

#include <cstring>

namespace patches {

FsoDisableConfig g_fso_disable_config = {
    /* enable */ true,
};

void fso_disable_apply() {
    if (!g_fso_disable_config.enable) {
        logger::logf("fso_disable: disabled");
        return;
    }

    // Get full path to CoDMP.exe (we're loaded inside it)
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        logger::logf("fso_disable: cannot get exe path");
        return;
    }

    // Open / create the AppCompatFlags\Layers key
    HKEY hkey = NULL;
    LSTATUS rc = RegCreateKeyExA(
        HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
        0, NULL, REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WRITE, NULL,
        &hkey, NULL);
    if (rc != ERROR_SUCCESS) {
        logger::logf("fso_disable: RegCreateKeyEx failed (err=%ld)", rc);
        return;
    }

    // Check current value first to avoid unnecessary writes
    char existing[256] = {0};
    DWORD existing_size = sizeof(existing);
    DWORD type = 0;
    rc = RegQueryValueExA(hkey, exe_path, NULL, &type,
                         (LPBYTE)existing, &existing_size);
    const char* desired = "~ DISABLEDXMAXIMIZEDWINDOWEDMODE";

    if (rc == ERROR_SUCCESS && type == REG_SZ &&
        strstr(existing, "DISABLEDXMAXIMIZEDWINDOWEDMODE") != NULL) {
        logger::logf("fso_disable: already disabled for %s", exe_path);
        RegCloseKey(hkey);
        return;
    }

    rc = RegSetValueExA(hkey, exe_path, 0, REG_SZ,
                       (const BYTE*)desired,
                       (DWORD)strlen(desired) + 1);
    RegCloseKey(hkey);

    if (rc == ERROR_SUCCESS) {
        logger::logf(
            "fso_disable: registry tweak written for %s (effective next launch)",
            exe_path);
    } else {
        logger::logf("fso_disable: RegSetValueEx failed (err=%ld)", rc);
    }
}

}  // namespace patches
