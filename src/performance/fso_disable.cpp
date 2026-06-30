// disable win10/11 FSO via AppCompatFlags layer. effective next launch.

#include "performance/fso_disable.h"
#include "core/logger.h"

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

    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        logger::logf("fso_disable: cannot get exe path");
        return;
    }

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
