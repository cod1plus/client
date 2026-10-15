#ifndef COD1RELOADED_LOGGER_H
#define COD1RELOADED_LOGGER_H

#include <windows.h>

namespace logger {

// call from DllMain before any log
void init(HMODULE self_module);

// Formats and queues the line; a writer thread appends it to cod1reloaded.log. The caller
// never touches the disk (see logger.cpp: the game thread used to open, write and close
// the file for every line - a stall of the frame whenever an antivirus or OneDrive looked).
void logf(const char* fmt, ...);

// Writes whatever is still queued, on the calling thread. For the end of the process
// (DLL_PROCESS_DETACH, WM_CLOSE): ExitProcess kills the writer thread first.
void flush();

// Developer diagnostics that write every second or more (pos_log, aim_log): off unless
// COD1RELOADED_DEV_LOGS=1 in the environment.
bool dev_logs();

}  // namespace logger

#endif
