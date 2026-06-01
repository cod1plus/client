#ifndef COD1RELOADED_LOGGER_H
#define COD1RELOADED_LOGGER_H

#include <windows.h>

namespace logger {

// Initialise le fichier "cod1reloaded.log" a cote du DLL.
// Doit etre appele depuis DllMain avant tout autre log.
void init(HMODULE self_module);

// Ajoute une ligne au log avec horodatage.
// Format printf classique. Ecrit aussi via OutputDebugStringA.
void logf(const char* fmt, ...);

}  // namespace logger

#endif
