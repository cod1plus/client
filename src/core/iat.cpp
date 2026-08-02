// One IAT patcher for the whole mod. updater (user32), gamma_fix (gdi32) and rinput
// (user32) all need the same walk, and it used to be copy-pasted per module.

#include "core/iat.h"

#include <cstring>

namespace patches {

void* iat_hook(const char* dll_name, const char* func, void* new_fn) {
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
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func) != 0) continue;
            void** slot = (void**)&iat->u1.Function;
            void* real = *slot;
            DWORD op = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &op)) return nullptr;
            *slot = new_fn;
            VirtualProtect(slot, sizeof(void*), op, &op);
            return real;
        }
    }
    return nullptr;
}

}  // namespace patches
