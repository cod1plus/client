#ifndef COD1RELOADED_PROCESS_PRIORITY_H
#define COD1RELOADED_PROCESS_PRIORITY_H

#include <windows.h>

namespace patches {

enum class ProcessPriorityLevel {
    Normal,
    AboveNormal,
    High,
    Realtime, // can lock the system
    Default,  // no-op
};

enum class ThreadPriorityLevel {
    Normal,
    AboveNormal,
    Highest,
    TimeCritical,
    Default,
};

struct ProcessPriorityConfig {
    ProcessPriorityLevel process_priority;
    ThreadPriorityLevel  main_thread_priority;
    bool disable_power_throttling;  // Win8+ ProcessPowerThrottling (EcoQoS downclock)
};

extern ProcessPriorityConfig g_process_priority_config;

// call from DllMain attach, main thread
void process_priority_apply();

}  // namespace patches

#endif
