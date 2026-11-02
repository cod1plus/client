// script_block.cpp - see script_block.h.
//
// CoDMP.exe 1.5:
//   0x40e2b0  Key_SetBinding(eax = keynum, edi = text) - every bind goes through it: the `bind`
//             command 0x40e420 (typed, or a config being executed), the menus' trap_Key_SetBinding
//             0x4037af, unbind / unbindall. It copies the text to keys[keynum].binding (0x14889a8 +
//             12 * keynum) and sets cvar_modifiedFlags 0x1912ae8 |= CVAR_ARCHIVE: the config is
//             written again at the next frame. Entry: cmp eax,-1; je 0x40e310 (its ret); push ebx.
//   0x40dea7  the console's Enter: Cbuf_AddText 0x42a090 (eax = text) with the typed line - eax =
//             0x148887d, the line after its \ or / (one is put in front when not in game), or
//             0x148887c, the line as chat right after "cmd say " (in game without a slash): chat is
//             left alone.
//   0x438b87  Com_EventLoop's SE_CONSOLE (the Windows console window, viewlog 1): Cbuf_AddText(text).
//   0x437c00  Com_Printf (cdecl).
// THE FIX = Key_SetBinding's entry jumps to a stub that swaps a refused text for "" (the key ends
// up unbound and the line leaves config_mp.cfg), and the two Cbuf_AddText calls go through a stub
// that drops a refused line. A command is refused when its first word is exec or vstr, in any of
// the ';' / newline separated commands of the text (quotes kept together as Cbuf does, leading
// slashes and a quoted command word seen through).

#include "input/script_block.h"
#include "core/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace patches {

namespace {

constexpr uintptr_t VA_SETBINDING   = 0x0040e2b0;
constexpr uintptr_t VA_SETBINDING_GO = 0x0040e2b5;   // push ebx, after the keynum -1 test
constexpr uintptr_t VA_CON_ENTER    = 0x0040dea7;   // call Cbuf_AddText (the typed line)
constexpr uintptr_t VA_CON_WINDOW   = 0x00438b87;   // call Cbuf_AddText (SE_CONSOLE)
constexpr uintptr_t VA_CBUF_ADDTEXT = 0x0042a090;
constexpr uintptr_t VA_COM_PRINTF   = 0x00437c00;
constexpr uintptr_t VA_CHAT_LINE    = 0x0148887c;   // the console field as chat (after "cmd say ")
const uint8_t SETBINDING_ENTRY[6] = { 0x83, 0xf8, 0xff, 0x74, 0x5b, 0x53 };

typedef void (__cdecl* Com_Printf_t)(const char*, ...);

bool is_word(const char* w, size_t n, const char* k) { return n == 4 && _strnicmp(w, k, 4) == 0; }

// true when one of the commands of `s` is exec or vstr
bool runs_script(const char* s) {
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == ';' || *s == '\n' || *s == '\r') ++s;
        while (*s == '/' || *s == '\\') ++s;
        const char* w = s;
        size_t n = 0;
        if (*s == '"') {                          // "exec" x is the command exec too
            w = ++s;
            while (*s && *s != '"') ++s;
            n = (size_t)(s - w);
            if (*s) ++s;
        } else {
            while (*s && *s != ' ' && *s != '\t' && *s != ';' && *s != '\n' && *s != '\r' && *s != '"') ++s;
            n = (size_t)(s - w);
        }
        if (is_word(w, n, "exec") || is_word(w, n, "vstr")) return true;
        bool quoted = false;                      // the rest of this command
        while (*s) {
            if (*s == '"') quoted = !quoted;
            else if (!quoted && (*s == ';' || *s == '\n' || *s == '\r')) break;
            ++s;
        }
    }
    return false;
}

const char* __cdecl bind_filter(int keynum, const char* text) {
    if (!text || !runs_script(text)) return text;
    ((Com_Printf_t)VA_COM_PRINTF)("^1Bind refused^7 (COD1.6X): exec and vstr are not allowed in a bind - \"%s\" removed\n", text);
    logger::logf("script_block: bind \"%s\" refused (key %d left unbound)", text, keynum);
    return "";
}

int __cdecl console_filter(const char* text) {
    if (!text || text == (const char*)VA_CHAT_LINE || !runs_script(text)) return 1;
    ((Com_Printf_t)VA_COM_PRINTF)("^1Refused^7 (COD1.6X): exec and vstr are not allowed\n");
    logger::logf("script_block: console line \"%s\" refused", text);
    return 0;
}

void* g_bind_fn = (void*)&bind_filter;
void* g_console_fn = (void*)&console_filter;
uintptr_t g_bind_back = VA_SETBINDING_GO;
uintptr_t g_cbuf_addtext = VA_CBUF_ADDTEXT;

// at Key_SetBinding's entry: eax = keynum, edi = text
__attribute__((naked)) void bind_stub() {
    __asm__ volatile(
        "cmpl $-1, %%eax\n\t"
        "je 1f\n\t"
        "pushal\n\t"
        "pushl %%edi\n\t"
        "pushl %%eax\n\t"
        "call *%0\n\t"                     /* bind_filter(keynum, text) */
        "addl $8, %%esp\n\t"
        "movl %%eax, (%%esp)\n\t"          /* the pushal frame's edi: the text to bind */
        "popal\n\t"
        "jmp *%1\n\t"
        "1:\n\t"
        "ret\n\t"                          /* keynum -1: the game's own early return */
        : : "m"(g_bind_fn), "m"(g_bind_back));
}

// in place of call Cbuf_AddText (eax = text); popal leaves the flags of the test alone
__attribute__((naked)) void console_stub() {
    __asm__ volatile(
        "pushal\n\t"
        "pushl %%eax\n\t"
        "call *%0\n\t"                     /* console_filter(text) */
        "addl $4, %%esp\n\t"
        "testl %%eax, %%eax\n\t"
        "popal\n\t"
        "jz 1f\n\t"
        "jmp *%1\n\t"                      /* Cbuf_AddText, returning to the game's caller */
        "1:\n\t"
        "ret\n\t"
        : : "m"(g_console_fn), "m"(g_cbuf_addtext));
}

bool write_rel32(uintptr_t at, uint8_t op, const void* target) {
    uint8_t* p = (uint8_t*)at;
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = op;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)target - (at + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

bool is_call_to(uintptr_t at, uintptr_t target) {
    const uint8_t* p = (const uint8_t*)at;
    return p[0] == 0xE8 && at + 5 + *(const int32_t*)(p + 1) == target;
}

}  // namespace

bool script_block_install() {
    if (memcmp((const void*)VA_SETBINDING, SETBINDING_ENTRY, sizeof(SETBINDING_ENTRY)) != 0 ||
        !is_call_to(VA_CON_ENTER, VA_CBUF_ADDTEXT) || !is_call_to(VA_CON_WINDOW, VA_CBUF_ADDTEXT)) {
        logger::logf("script_block: Key_SetBinding / the console's Cbuf_AddText are not the 1.5 ones - not installed");
        return false;
    }
    if (!write_rel32(VA_CON_ENTER, 0xE8, (const void*)&console_stub) ||
        !write_rel32(VA_CON_WINDOW, 0xE8, (const void*)&console_stub) ||
        !write_rel32(VA_SETBINDING, 0xE9, (const void*)&bind_stub)) {
        logger::logf("script_block: VirtualProtect failed - not installed");
        return false;
    }
    logger::logf("script_block: exec / vstr refused in binds (Key_SetBinding 0x%x) and typed in the console (0x%x, 0x%x)",
                 (unsigned)VA_SETBINDING, (unsigned)VA_CON_ENTER, (unsigned)VA_CON_WINDOW);
    return true;
}

}  // namespace patches
