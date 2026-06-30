#ifndef COD1RELOADED_TOAST_H
#define COD1RELOADED_TOAST_H

#include <windows.h>

namespace patches {

enum class ToastType { Info, Warning, Error };

// thread-safe, callable from any thread
void toast_show(const char* title, const char* body, ToastType type = ToastType::Info);

void toast_shutdown();

}  // namespace patches

#endif
