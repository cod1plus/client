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

// From the GAME thread at every frame wait, outside DllMain (avrt.dll is loaded here, never
// under the loader lock): join or leave the Multimedia Class Scheduler's "Games" task as the
// cvar cod1x_mmcss asks (0 = default since 1.6.15). MMCSS schedules the thread in the real-time
// band while it stays within its quota, with the task's I/O and GPU priorities. 1.6.15: OFF by
// default - with the frame limiter spinning the tail of every frame, a real-time game thread can
// starve the GPU driver's own thread on a CPU with few cores, and 1.6.13 players reported fps
// drops as soon as other players came into view. COD1RELOADED_MMCSS=0 forces it off.
void process_priority_mmcss_join();
// watcher thread: registers cod1x_mmcss (archived) and publishes its value
void process_priority_mmcss_tick();

}  // namespace patches

#endif
