#ifndef COD1RELOADED_INI_LOCK_H
#define COD1RELOADED_INI_LOCK_H
// The keys of cod1reloaded.ini the mod imposes (enzo + the admins, 2026-09-25: everyone
// plays with the same frame pacing, the same input path and the same presentation).
//
// At every launch the player's .ini is brought in line: a locked key that is missing is
// appended (with a one-line explanation), one that carries another value is rewritten
// in place - every other line, comment and the player's own choices are kept byte for
// byte. The values used at runtime are the locked ones whatever the file says, so
// editing the file changes nothing, and the next launch puts the line back.
//
// Two of them also live in engine cvars the 1.6X menu or the console could still push
// around: r_fullscreen is set to 1 and made read-only once the engine has registered
// it, and r_displayRefresh is re-asserted at the display's maximum (ini_lock_tick).
//
// The teeth of the lock are on the server: cod1plus.so refuses clients below the build
// that carries it (cod1x_build), so an older or edited DLL does not get in.
namespace patches {

void ini_lock_apply(const char* ini_path);   // load_config: rewrite the file, force the values
void ini_lock_tick();                        // watcher thread: the engine-side locks

}  // namespace patches

#endif
