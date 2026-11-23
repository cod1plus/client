// One IAT patcher for the whole mod. updater (user32), gamma_fix (gdi32) and rinput
// (user32) all need the same walk, and it used to be copy-pasted per module.
//
// CoDMP.exe imports WSOCK32.dll by ORDINAL only (no names in its thunk table), so the
// walk matches either a name or an ordinal (net_diag: sendto = 20, recvfrom = 17).

#include "core/iat.h"

#include <cstring>

namespace patches {

namespace {

// The import slot of <dll>!<func> (func set) or <dll>!#<ordinal> (func null) in the
// main EXE, or null.
void** iat_slot_impl(const char* dll_name, const char* func, WORD ordinal) {
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base) return nullptr;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    DWORD imp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva) return nullptr;

    auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + imp_rva);
    for (; imp->Name; ++imp) {
        const char* dll = (const char*)(base + imp->Name);
        if (_stricmp(dll, dll_name) != 0) continue;
        DWORD orig_rva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        auto orig = (IMAGE_THUNK_DATA*)(base + orig_rva);
        auto iat  = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; orig->u1.AddressOfData; ++orig, ++iat) {
            const bool by_ordinal = (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) != 0;
            if (func) {
                if (by_ordinal) continue;
                auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
                if (strcmp((const char*)ibn->Name, func) != 0) continue;
            } else {
                if (!by_ordinal || IMAGE_ORDINAL(orig->u1.Ordinal) != ordinal) continue;
            }
            return (void**)&iat->u1.Function;
        }
    }
    return nullptr;
}

// swap the slot's function for new_fn, return what it held
void* hook_slot(void** slot, void* new_fn) {
    if (!slot) return nullptr;
    void* real = *slot;
    DWORD op = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &op)) return nullptr;
    *slot = new_fn;
    VirtualProtect(slot, sizeof(void*), op, &op);
    return real;
}

}  // namespace

void* iat_peek(const char* dll_name, const char* func) {
    void** slot = func ? iat_slot_impl(dll_name, func, 0) : nullptr;
    return slot ? *slot : nullptr;
}

void* iat_hook(const char* dll_name, const char* func, void* new_fn) {
    return func ? hook_slot(iat_slot_impl(dll_name, func, 0), new_fn) : nullptr;
}

void* iat_hook_ordinal(const char* dll_name, WORD ordinal, void* new_fn) {
    return hook_slot(iat_slot_impl(dll_name, nullptr, ordinal), new_fn);
}

}  // namespace patches
