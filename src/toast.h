#ifndef COD1RELOADED_TOAST_H
#define COD1RELOADED_TOAST_H

#include <windows.h>

namespace patches {

// Affiche un toast Windows (balloon tip dans system tray).
// Marche sur Win7 → Win11. Auto-disparait apres ~5 sec.
//
// type :
//   "info"    => icone info (i)
//   "warning" => icone warning (!)
//   "error"   => icone error (X)
//
// Thread-safe : peut etre appele depuis n'importe quel thread.
enum class ToastType { Info, Warning, Error };

void toast_show(const char* title, const char* body, ToastType type = ToastType::Info);

// Cleanup au shutdown.
void toast_shutdown();

}  // namespace patches

#endif
