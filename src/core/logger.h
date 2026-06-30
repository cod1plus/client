#ifndef COD1RELOADED_LOGGER_H
#define COD1RELOADED_LOGGER_H

#include <windows.h>

namespace logger {

// call from DllMain before any log
void init(HMODULE self_module);

void logf(const char* fmt, ...);

}  // namespace logger

#endif
