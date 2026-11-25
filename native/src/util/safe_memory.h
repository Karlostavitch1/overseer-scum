// Fault-tolerant reads for probing game memory whose layout we are still
// discovering. Never use these on hot paths.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace overseer::memory {

// Copies `size` bytes, returning false instead of crashing on an access violation.
bool SafeRead(const void* source, void* destination, size_t size);

template <typename T>
bool Read(uintptr_t address, T& out) {
    return SafeRead(reinterpret_cast<const void*>(address), &out, sizeof(T));
}

// True if [address, address+size) is committed, readable memory.
bool IsReadable(uintptr_t address, size_t size);

// Reads a NUL-terminated narrow string of at most `maxLength` bytes.
bool ReadCString(uintptr_t address, std::string& out, size_t maxLength = 256);

}  // namespace overseer::memory
