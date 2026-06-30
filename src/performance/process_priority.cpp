#include "performance/process_priority.h"
#include "core/logger.h"

namespace patches {

ProcessPriorityConfig g_process_priority_config = {
    /* process_priority         */ ProcessPriorityLevel::High,
    /* main_thread_priority     */ ThreadPriorityLevel::Highest,
    /* disable_power_throttling */ true,
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
    state.ControlMask  = PROCESS_POWER_THROTTLING_EXECUTION_SPEED_LOCAL;
    state.StateMask    = 0; // disabled

    if (fn(GetCurrentProcess(), 4, &state, sizeof(state))) {  // 4 = ProcessPowerThrottling
        logger::logf("process_priority: power throttling disabled");
    } else {
        logger::logf("process_priority: SetProcessInformation failed (err=%lu)",
                     GetLastError());
    }
}

}  // namespace

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
