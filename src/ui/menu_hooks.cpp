// menu_hooks.cpp - see menu_hooks.h
#include "ui/menu_hooks.h"
#include "core/logger.h"

#include <cstdint>
#include <cstring>

namespace patches {

// ---- shared with the naked hooks (C linkage, leading underscore in the asm) ----
extern "C" {
    void* g_uih_find            = nullptr;   // Menus_FindByName (abs)
    void* g_uih_menucount       = nullptr;   // &menuCount (abs)
    void* g_uih_activate_resume = nullptr;   // ActivateByName + 6
    void* g_uih_close_resume    = nullptr;   // CloseByName + 6
    void* g_uih_closeall_resume = nullptr;   // CloseAll + 5
    void  uih_on_activate(const char* name);
    void  uih_on_close(const char* name);
    void  uih_on_closeall(void);
}

namespace {

constexpr uintptr_t RVA_ACTIVATE  = 0x14010;   // push eax ; call Menus_FindByName ; ...
constexpr uintptr_t RVA_CLOSE     = 0x10e60;   // push eax ; call Menus_FindByName ; ...
constexpr uintptr_t RVA_CLOSEALL  = 0x10e80;   // mov eax, [menuCount] ; push edi ; ...
constexpr uintptr_t RVA_FIND      = 0x10d80;
constexpr uintptr_t RVA_MENUCOUNT = 0x1c04b8;
// the menu table itself (Menus_FindByName walks it from +0x20 = windowDef_t.name):
// menuDef_t stride 0x70c, window.name at +0x20, window.flags at +0x48 with
// WINDOW_HASFOCUS = 2 and WINDOW_VISIBLE = 4 (Menu_Close clears 6)
constexpr uintptr_t RVA_MENUS     = 0x203720;
constexpr size_t    MENU_STRIDE   = 0x70c;
constexpr int       MENU_MAX      = 128;

char      g_top[64] = "";
HMODULE   g_ui_base = NULL;
int       g_logged_fail = 0;

void set_top(const char* name) {
    if (!name) name = "";
    if (strcmp(g_top, name) != 0) {
        char safe[64]; size_t k = 0;
        for (; name[k] && k + 1 < sizeof(safe); ++k) safe[k] = name[k];
        safe[k] = 0;
        logger::logf("menu_hooks: top menu \"%s\" -> \"%s\"", g_top, safe);
        strcpy(g_top, safe);
    }
}

// Entry detour: E9 rel32 over the first `len` bytes (the rest become dead code; the
// trampoline re-emits the stolen instructions itself).
bool detour(uintptr_t func, const uint8_t* expect, int len, void* hook, const char* tag) {
    if (memcmp((const void*)func, expect, len) != 0) {
        const uint8_t* b = (const uint8_t*)func;
        if (b[0] == 0xE9) return true;             // already ours
        logger::logf("menu_hooks: %s: unexpected bytes @%p (%02x %02x %02x %02x %02x %02x) - not hooked",
                     tag, (void*)func, b[0], b[1], b[2], b[3], b[4], b[5]);
        return false;
    }
    uint8_t patch[5] = { 0xE9 };
    const int32_t rel = (int32_t)((uintptr_t)hook - (func + 5));
    memcpy(patch + 1, &rel, 4);
    DWORD old = 0;
    if (!VirtualProtect((void*)func, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)func, patch, 5);
    VirtualProtect((void*)func, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)func, len);
    return true;
}

// The engine's "main" is activated during UI init - usually before the watcher has
// even seen the DLL, so the hooks miss that first call. Read the table instead:
// the focused menu is the top one. Also a once-a-second correction afterwards.
void scan_focused(uintptr_t base) {
    const int count = *(const int*)(base + RVA_MENUCOUNT);
    if (count <= 0 || count > MENU_MAX) return;
    const char* focused = nullptr;
    for (int i = 0; i < count; ++i) {
        const uintptr_t m = base + RVA_MENUS + (uintptr_t)i * MENU_STRIDE;
        const int flags = *(const int*)(m + 0x48);
        const char* name = *(const char* const*)(m + 0x20);
        if ((flags & 2) && name) { focused = name; break; }
    }
    if (focused) set_top(focused);
    else if (g_top[0]) {
        // nothing focused: still visible? keep "main" only while it is visible
        for (int i = 0; i < count; ++i) {
            const uintptr_t m = base + RVA_MENUS + (uintptr_t)i * MENU_STRIDE;
            const char* name = *(const char* const*)(m + 0x20);
            if (name && strcmp(name, g_top) == 0 && (*(const int*)(m + 0x48) & 4)) return;
        }
        set_top("");
    }
}

uintptr_t menu_slot(const char* name) {
    const uintptr_t base = (uintptr_t)GetModuleHandleA("ui_mp_x86.dll");
    if (!base || !name) return 0;
    const int count = *(const int*)(base + RVA_MENUCOUNT);
    if (count <= 0 || count > MENU_MAX) return 0;
    for (int i = 0; i < count; ++i) {
        const uintptr_t m = base + RVA_MENUS + (uintptr_t)i * MENU_STRIDE;
        const char* n = *(const char* const*)(m + 0x20);
        if (n && strcmp(n, name) == 0) return m;
    }
    return 0;
}

}  // namespace

bool ui_menu_visible(const char* name) {
    const uintptr_t m = menu_slot(name);
    return m && (*(const int*)(m + 0x48) & 4);
}

void ui_menu_hide(const char* name) {
    const uintptr_t m = menu_slot(name);
    if (m && (*(const int*)(m + 0x48) & 6))
        *(int*)(m + 0x48) &= ~6;              // what Menu_Close clears (no onClose script)
}

// ---- the C side of the hooks (main thread, inside the UI DLL's own calls) ----
extern "C" void uih_on_activate(const char* name) { set_top(name); }
extern "C" void uih_on_close(const char* name) {
    if (name && strcmp(g_top, name) == 0) set_top("");
}
extern "C" void uih_on_closeall(void) { set_top(""); }

// ---- naked trampolines ----
extern "C" __attribute__((naked)) void uih_activate_hook() {
    asm(
        "pushal\n\t"
        "pushfl\n\t"
        "pushl %eax\n\t"
        "call _uih_on_activate\n\t"
        "addl $4, %esp\n\t"
        "popfl\n\t"
        "popal\n\t"
        "pushl %eax\n\t"                        // stolen: push eax
        "call *_g_uih_find\n\t"                 // stolen: call Menus_FindByName
        "jmp *_g_uih_activate_resume\n\t"
    );
}
extern "C" __attribute__((naked)) void uih_close_hook() {
    asm(
        "pushal\n\t"
        "pushfl\n\t"
        "pushl %eax\n\t"
        "call _uih_on_close\n\t"
        "addl $4, %esp\n\t"
        "popfl\n\t"
        "popal\n\t"
        "pushl %eax\n\t"
        "call *_g_uih_find\n\t"
        "jmp *_g_uih_close_resume\n\t"
    );
}
extern "C" __attribute__((naked)) void uih_closeall_hook() {
    asm(
        "pushal\n\t"
        "pushfl\n\t"
        "call _uih_on_closeall\n\t"
        "popfl\n\t"
        "popal\n\t"
        "movl _g_uih_menucount, %eax\n\t"      // stolen: mov eax, [menuCount]
        "movl (%eax), %eax\n\t"
        "jmp *_g_uih_closeall_resume\n\t"
    );
}

void menu_hooks_tick() {
    HMODULE h = GetModuleHandleA("ui_mp_x86.dll");
    if (!h) { if (g_ui_base) { g_ui_base = NULL; set_top(""); } return; }
    const uintptr_t base = (uintptr_t)h;
    // (re)install when the module is new OR its entry bytes are no longer ours (the
    // engine reloads the DLL at the same address on vid_restart)
    const bool fresh = (h != g_ui_base);
    if (!fresh && *(const uint8_t*)(base + RVA_ACTIVATE) == 0xE9) {
        static DWORD s_scan = 0;
        const DWORD now = GetTickCount();
        if (now - s_scan > 1000) { s_scan = now; scan_focused(base); }
        return;
    }

    const uint8_t* a = (const uint8_t*)(base + RVA_ACTIVATE);
    const uint8_t* c = (const uint8_t*)(base + RVA_CLOSE);
    // expected: 50 E8 <rel32 to FindByName>
    uint8_t exp_a[6] = { 0x50, 0xE8 }, exp_c[6] = { 0x50, 0xE8 };
    int32_t rel_a = (int32_t)((base + RVA_FIND) - (base + RVA_ACTIVATE + 6));
    int32_t rel_c = (int32_t)((base + RVA_FIND) - (base + RVA_CLOSE + 6));
    memcpy(exp_a + 2, &rel_a, 4);
    memcpy(exp_c + 2, &rel_c, 4);
    uint8_t exp_all[5] = { 0xA1 };
    uint32_t mc = (uint32_t)(base + RVA_MENUCOUNT);
    memcpy(exp_all + 1, &mc, 4);
    (void)a; (void)c;

    g_uih_find            = (void*)(base + RVA_FIND);
    g_uih_menucount       = (void*)(base + RVA_MENUCOUNT);
    g_uih_activate_resume = (void*)(base + RVA_ACTIVATE + 6);
    g_uih_close_resume    = (void*)(base + RVA_CLOSE + 6);
    g_uih_closeall_resume = (void*)(base + RVA_CLOSEALL + 5);

    const bool ok1 = detour(base + RVA_ACTIVATE, exp_a, 6, (void*)&uih_activate_hook, "Menus_ActivateByName");
    const bool ok2 = detour(base + RVA_CLOSE,    exp_c, 6, (void*)&uih_close_hook,    "Menus_CloseByName");
    const bool ok3 = detour(base + RVA_CLOSEALL, exp_all, 5, (void*)&uih_closeall_hook, "Menus_CloseAll");
    g_ui_base = h;
    scan_focused(base);
    if (ok1 && ok2 && ok3)
        logger::logf("menu_hooks: ui_mp_x86.dll @%p hooked (activate/close/closeall)%s", (void*)h,
                     fresh ? "" : " - re-installed after reload");
    else if (g_logged_fail++ < 3)
        logger::logf("menu_hooks: ui_mp_x86.dll @%p: partial (%d%d%d) - home screen stays off", (void*)h, ok1, ok2, ok3);
}

bool ui_main_menu_active() { return strcmp(g_top, "main") == 0; }
const char* ui_menu_top()  { return g_top; }

}  // namespace patches
