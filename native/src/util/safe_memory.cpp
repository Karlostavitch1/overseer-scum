#include "util/safe_memory.h"

#include <windows.h>

#include <cstring>

namespace overseer::memory {
namespace {

// Kept free of C++ objects so __try/__except is allowed.
bool CopyGuarded(const void* source, void* destination, size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool SafeRead(const void* source, void* destination, size_t size) {
    if (!source || reinterpret_cast<uintptr_t>(source) < 0x10000) return false;
    return CopyGuarded(source, destination, size);
}

bool IsReadable(uintptr_t address, size_t size) {
    if (address < 0x10000) return false;
    uintptr_t end = address + size;
    while (address < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) return false;
        if (info.State != MEM_COMMIT) return false;
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(info.Protect & readable) || (info.Protect & PAGE_GUARD)) return false;
        address = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

bool ReadCString(uintptr_t address, std::string& out, size_t maxLength) {
    out.clear();
    for (size_t i = 0; i < maxLength; ++i) {
        char c;
        if (!Read(address + i, c)) return false;
        if (c == '\0') return true;
        out.push_back(c);
    }
    return true;
}

}  // namespace overseer::memory
