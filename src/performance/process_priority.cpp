#include "performance/process_priority.h"
#include "core/logger.h"
#include "features/settings_menu.h"      // CODMP_CVAR_FINDVAR_VA
#include "netcode/protocol_patch.h"      // CODMP_CVAR_GET_VA

#include <cstdlib>

namespace patches {

ProcessPriorityConfig g_process_priority_config = {
    /* process_priority         */ ProcessPriorityLevel::High,
    /* main_thread_priority     */ ThreadPriorityLevel::Highest,
    /* disable_power_throttling */ true,
    /* mmcss_games              */ true,
};

namespace {

DWORD priority_class_from(ProcessPriorityLevel lvl) {
    switch (lvl) {
        case ProcessPriorityLevel::Normal:      return NORMAL_PRIORITY_CLASS;
        case ProcessPriorityLevel::AboveNormal: return ABOVE_NORMAL_PRIORITY_CLASS;
        case ProcessPriorityLevel::High:        return HIGH_PRIORITY_CLASS;
        case ProcessPriorityLevel::Realtime:    return REALTIME_PRIORITY_CLASS;
        case ProcessPriorityLevel::Default:
        default:                                return 0;
    }
}

int thread_priority_from(ThreadPriorityLevel lvl) {
    switch (lvl) {
        case ThreadPriorityLevel::Normal:        return THREAD_PRIORITY_NORMAL;
        case ThreadPriorityLevel::AboveNormal:   return THREAD_PRIORITY_ABOVE_NORMAL;
        case ThreadPriorityLevel::Highest:       return THREAD_PRIORITY_HIGHEST;
        case ThreadPriorityLevel::TimeCritical:  return THREAD_PRIORITY_TIME_CRITICAL;
        case ThreadPriorityLevel::Default:
        default:                                 return -999;  // sentinel
    }
}

const char* level_name(ProcessPriorityLevel lvl) {
    switch (lvl) {
        case ProcessPriorityLevel::Normal:      return "normal";
        case ProcessPriorityLevel::AboveNormal: return "above_normal";
        case ProcessPriorityLevel::High:        return "high";
        case ProcessPriorityLevel::Realtime:    return "realtime";
        default:                                return "default";
    }
}

const char* level_name(ThreadPriorityLevel lvl) {
    switch (lvl) {
        case ThreadPriorityLevel::Normal:       return "normal";
        case ThreadPriorityLevel::AboveNormal:  return "above_normal";
        case ThreadPriorityLevel::Highest:      return "highest";
        case ThreadPriorityLevel::TimeCritical: return "time_critical";
        default:                                return "default";
    }
}

// MinGW may not define PROCESS_POWER_THROTTLING_STATE (Win8+); local alias.
typedef struct _COD1R_PWR_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} PROCESS_POWER_THROTTLING_STATE_LOCAL;
#define PROCESS_POWER_THROTTLING_STATE_LOCAL_VERSION   1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED_LOCAL 0x1
// Windows 11: a process whose window is occluded / minimized / invisible has its
// timeBeginPeriod(1) IGNORED unless it opts out with this bit. The frame limiter sleeps
// on that 1 ms resolution; without it a Sleep(1) lasts 15.6 ms and the game runs at
// ~62 fps while a full-screen overlay covers it. Opt out (bit in ControlMask, clear in
// StateMask = "do not ignore").
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION_LOCAL 0x4

void disable_power_throttling_impl() {
    // SetProcessInformation is Win8+; resolve dynamically (Win7 just skips)
    typedef BOOL (WINAPI *SetProcessInformation_t)(HANDLE, int, LPVOID, DWORD);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) return;
    SetProcessInformation_t fn = (SetProcessInformation_t)
        GetProcAddress(k32, "SetProcessInformation");
    if (!fn) {
        logger::logf("process_priority: SetProcessInformation unavailable (pre-Win8)");
        return;
    }

    PROCESS_POWER_THROTTLING_STATE_LOCAL state = {};
    state.Version      = PROCESS_POWER_THROTTLING_STATE_LOCAL_VERSION;
    state.ControlMask  = PROCESS_POWER_THROTTLING_EXECUTION_SPEED_LOCAL |
                         PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION_LOCAL;
    state.StateMask    = 0; // both disabled: full speed, timer resolution always honoured

    if (fn(GetCurrentProcess(), 4, &state, sizeof(state))) {  // 4 = ProcessPowerThrottling
        logger::logf("process_priority: power throttling disabled, 1 ms timer kept when occluded");
    } else {
        // Windows 10 rejects the timer bit: ask for the execution speed alone
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED_LOCAL;
        if (fn(GetCurrentProcess(), 4, &state, sizeof(state)))
            logger::logf("process_priority: power throttling disabled");
        else
            logger::logf("process_priority: SetProcessInformation failed (err=%lu)",
                         GetLastError());
    }
}

}  // namespace

namespace {
volatile LONG g_mmcss_want = 0;          // cod1x_mmcss (watcher writes, game thread reads)
HANDLE g_mmcss = nullptr;                // the game thread's MMCSS registration, when joined
bool   g_mmcss_dead = false;             // avrt.dll missing / env override: never try again
}

void process_priority_mmcss_tick() {
    typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
    typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
    static void* s_cv = nullptr;
    if (!s_cv) s_cv = ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_mmcss", "0", 1 /* archive */);
    const LONG v = (s_cv && *(int*)((char*)s_cv + 0x20) != 0) ? 1 : 0;
    if (v != g_mmcss_want) InterlockedExchange(&g_mmcss_want, v);
}

void process_priority_mmcss_join() {
    typedef HANDLE (WINAPI *AvSetMmThreadCharacteristicsW_t)(LPCWSTR, LPDWORD);
    typedef BOOL (WINAPI *AvRevertMmThreadCharacteristics_t)(HANDLE);
    static AvSetMmThreadCharacteristicsW_t s_join = nullptr;
    static AvRevertMmThreadCharacteristics_t s_revert = nullptr;
    const bool want = g_mmcss_want != 0 && g_process_priority_config.mmcss_games && !g_mmcss_dead;
    if (want == (g_mmcss != nullptr)) return;    // the common case: nothing to do
    if (!s_join) {
        const char* e = getenv("COD1RELOADED_MMCSS");
        if (e && e[0] == '0' && e[1] == 0) {
            logger::logf("process_priority: MMCSS off (COD1RELOADED_MMCSS=0)");
            g_mmcss_dead = true;
            return;
        }
        HMODULE avrt = LoadLibraryA("avrt.dll");                 // Vista+; absent = skip
        s_join = avrt ? (AvSetMmThreadCharacteristicsW_t)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : nullptr;
        s_revert = avrt ? (AvRevertMmThreadCharacteristics_t)GetProcAddress(avrt, "AvRevertMmThreadCharacteristics") : nullptr;
        if (!s_join || !s_revert) {
            logger::logf("process_priority: MMCSS unavailable (avrt.dll)");
            g_mmcss_dead = true;
            return;
        }
    }
    if (want) {
        DWORD task = 0;
        g_mmcss = s_join(L"Games", &task);
        if (g_mmcss)
            logger::logf("process_priority: game thread in the MMCSS Games task (cod1x_mmcss 1; index %lu, now priority %d)",
                         task, GetThreadPriority(GetCurrentThread()));
        else {
            logger::logf("process_priority: MMCSS Games refused (err=%lu) - thread priority unchanged", GetLastError());
            g_mmcss_dead = true;
        }
    } else {
        s_revert(g_mmcss);
        g_mmcss = nullptr;
        logger::logf("process_priority: game thread out of MMCSS (cod1x_mmcss 0; priority %d)",
                     GetThreadPriority(GetCurrentThread()));
    }
}

void process_priority_apply() {
    const DWORD pclass = priority_class_from(g_process_priority_config.process_priority);
    if (pclass != 0) {
        if (SetPriorityClass(GetCurrentProcess(), pclass)) {
            logger::logf("process_priority: process priority set to %s",
                         level_name(g_process_priority_config.process_priority));
        } else {
            logger::logf("process_priority: SetPriorityClass failed (err=%lu)",
                         GetLastError());
        }
    }

    const int tprio = thread_priority_from(g_process_priority_config.main_thread_priority);
    if (tprio != -999) {
        if (SetThreadPriority(GetCurrentThread(), tprio)) {
            logger::logf("process_priority: main thread priority set to %s",
                         level_name(g_process_priority_config.main_thread_priority));
        } else {
            logger::logf("process_priority: SetThreadPriority failed (err=%lu)",
                         GetLastError());
        }
    }

    if (g_process_priority_config.disable_power_throttling) {
        disable_power_throttling_impl();
    }
}

}  // namespace patches
