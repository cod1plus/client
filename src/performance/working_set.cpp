// keep pages resident to avoid page-fault hitches after alt-tab. min>0 is a hint, not a guarantee.

#include "performance/working_set.h"
#include "core/logger.h"

namespace patches {

WorkingSetConfig g_working_set_config = {
    /* enable  */ true,
    /* min_mb  */ 128,
    /* max_mb  */ 512,
};

namespace {

// CoDMP.exe is a 32-bit process: SIZE_T is 32 bits and `5000 * 1024 * 1024` (the shipped
// max) silently wrapped to 904 MB. Compute in 64 bits and clamp to the address space
// (CoDMP.exe is not LARGE_ADDRESS_AWARE: 2 GB of user VA, a working set beyond it is
// meaningless). The game answered ERROR_INVALID_PARAMETER (87) to the dev ini's
// 2000 / 3000 MB (2026-09-25) although a bare 32-bit process accepts those very values,
// so whatever the reason a refused minimum falls back to the 128 MB hint below rather
// than costing the maximum too - and the log now shows the numbers tried.
constexpr unsigned long long MB = 1024ull * 1024ull;

unsigned long long address_space_mb() {
    const BYTE* base = (const BYTE*)GetModuleHandleA(NULL);
    if (base) {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) return 4095;
    }
    return 2047;
}

bool try_set(unsigned long long min_mb, unsigned long long max_mb, DWORD* err) {
    const unsigned long long cap = address_space_mb();
    if (max_mb > cap) max_mb = cap;
    if (min_mb > max_mb) min_mb = max_mb;
    if (SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)(min_mb * MB), (SIZE_T)(max_mb * MB))) {
        logger::logf("working_set: min=%llu MB, max=%llu MB", min_mb, max_mb);
        return true;
    }
    *err = GetLastError();
    return false;
}

}  // namespace

void working_set_apply() {
    if (!g_working_set_config.enable) {
        logger::logf("working_set: disabled");
        return;
    }
    const unsigned long long min_mb = g_working_set_config.min_mb;
    const unsigned long long max_mb = g_working_set_config.max_mb;
    DWORD err = 0;
    if (try_set(min_mb, max_mb, &err)) return;
    // A minimum the system will not grant (err 87 for a minimum above what an ordinary
    // process may pin) must not cost the maximum too: the minimum is only a hint.
    if (min_mb > 128 && try_set(128, max_mb, &err)) {
        logger::logf("working_set: the requested minimum of %llu MB was refused (err=%lu) - 128 MB used",
                     min_mb, err);
        return;
    }
    logger::logf("working_set: SetProcessWorkingSetSize(%llu MB, %llu MB) failed (err=%lu) - skipping",
                 min_mb, max_mb, err);
}

}  // namespace patches
