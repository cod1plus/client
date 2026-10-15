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
    bool mmcss_games;               // the game thread joins the MMCSS "Games" task (see below)
};

extern ProcessPriorityConfig g_process_priority_config;

// call from DllMain attach, main thread
void process_priority_apply();

// Once, from the GAME thread and outside DllMain (avrt.dll is loaded here, never under the
// loader lock): register it with the Multimedia Class Scheduler's "Games" task. MMCSS then
// schedules it in the real-time band while it stays within its quota (SystemResponsiveness
// keeps 20 % of the CPU for everything else), with the task's I/O and GPU priorities - other
// programs' threads (browser, Discord, updaters) stop preempting it on its core.
// COD1RELOADED_MMCSS=0 leaves it out.
void process_priority_mmcss_join();

}  // namespace patches

#endif
