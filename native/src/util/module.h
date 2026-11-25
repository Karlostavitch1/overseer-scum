// Helpers for inspecting the host process's PE image.
#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace overseer::module {

struct Info {
    std::wstring path;
    uintptr_t base = 0;
    size_t size = 0;
    uint32_t timestamp = 0;      // IMAGE_FILE_HEADER::TimeDateStamp — identifies the build
    std::string fileVersion;     // e.g. "4.27.2.0", empty if absent
};

// The main executable (e.g. SCUMServer.exe).
Info MainModule();

// Bytes of a named section (e.g. ".text", ".rdata") of a loaded module.
std::optional<std::span<const uint8_t>> Section(HMODULE module, const char* name);

}  // namespace overseer::module
