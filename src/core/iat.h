#ifndef COD1RELOADED_IAT_H
#define COD1RELOADED_IAT_H

#include <windows.h>

namespace patches {

// Redirect one import slot of the main EXE to new_fn. Returns the original function
// (call it to chain), or null when the EXE does not import it by name.
// Ordinal-only imports are skipped: there is no name to match.
void* iat_hook(const char* dll, const char* func, void* new_fn);

}  // namespace patches

#endif
