#ifndef COD1RELOADED_PROCESS_PRIORITY_H
#define COD1RELOADED_PROCESS_PRIORITY_H

#include <windows.h>

namespace patches {

enum class ProcessPriorityLevel {
    Normal,
    AboveNormal,
    High,
    Realtime, // dangerous, can lock the system
    Default,  // do nothing
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
    // Disable Windows "ProcessPowerThrottling" (introduced in Win8).
    // When throttled, background apps run slower to save power - bad for games.
    bool disable_power_throttling;
};

extern ProcessPriorityConfig g_process_priority_config;

// Apply priorities. Call from DllMain DLL_PROCESS_ATTACH on the main thread.
void process_priority_apply();

}  // namespace patches

#endif
