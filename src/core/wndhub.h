#ifndef COD1RELOADED_WNDHUB_H
#define COD1RELOADED_WNDHUB_H
// The ONE WindowProc subclass of the whole mod.
//
// 1.6.6 crashed on the way back from the desktop because two modules (gl_overlay and
// window_patch) each subclassed the game window on their own, each keeping a single
// global "original" pointer. window_patch re-subclassed after its own restore had
// FORGOTTEN a hook it could not remove (the overlay's, sitting above it), stored the
// overlay's proc as its original - and the overlay's original was window_patch:
// proc A -> proc B -> proc A -> ... stack overflow on the next message (a player's
// log, 2026-09-20: "WindowProc subclassed (orig=0x713d37c0)", an address INSIDE our
// own DLL, was the last line before the game vanished).
//
// So: one proc, one record per window, listeners instead of subclassers, and
//   * installed from the window's own thread only (the SwapBuffers hook - every
//     display mode reaches it, and it is the thread that owns the window), never
//     from a watcher thread;
//   * never "restored" by writing a saved pointer back - a third-party overlay
//     (Discord, Medal, the GPU vendors' - a Unicode proc 0xffffXXXX above ours is
//     the norm on players' machines, not the exception) may sit above us, and
//     writing over it would orphan it exactly the way window_patch orphaned us;
//   * self-healing: once a second the window is pinged with a private message; if
//     the reply does not come back our proc is no longer in the chain (a third
//     party unhooked by restoring what IT had saved) and it is re-installed on top
//     of whatever is there now - safe, because we are proven absent from the chain;
//   * a depth guard: a message that passes through the hub more than a sane number
//     of times in a row is a loop somebody else built through us; it is cut with
//     DefWindowProc and logged instead of taking the process down.
#include <windows.h>

namespace patches {

// A listener sees every message of every attached window, in registration order.
// Return true to CONSUME the message: *result is returned to Windows and nothing
// further (not even the engine's proc) sees it. Return false to pass it on.
typedef bool (*WndHubListener)(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

// DllMain time. Order matters: an input-consuming listener (the overlay) goes first.
void wndhub_add_listener(WndHubListener fn, const char* name);

// Attach to (or verify the attachment of) a window. MUST run on the window's thread:
// call it from the SwapBuffers hook with WindowFromDC(). Cheap when nothing changed;
// once a second it also runs the in-chain ping. Foreign windows (not the game class)
// are refused - the mod never subclasses a window it did not ask for.
void wndhub_attach(HWND hwnd);

// The engine's window: the last one attached that is still alive, or NULL before the
// first frame. Valid in every display mode.
HWND wndhub_window();

// The class name the engine registers for its game window: WNDCLASSA.lpszClassName
// at CoDMP.exe 0x4eeb72 and both name arguments of CreateWindowExA at 0x4eeca8 are
// the same string. (NOT "CoDMP" - that string does not exist in the exe; it is only
// the file name.)
constexpr const char* WNDHUB_GAME_CLASS = "Call of Duty Multiplayer";

}  // namespace patches

#endif
