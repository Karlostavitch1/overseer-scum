#include "util/module.h"

#include <cstring>
#include <vector>

#pragma comment(lib, "version.lib")

namespace overseer::module {
namespace {

const IMAGE_NT_HEADERS* NtHeaders(HMODULE module) {
    auto base = reinterpret_cast<const uint8_t*>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

std::string FileVersion(const std::wstring& path) {
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) {
        return {};
    }
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." +
           std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(info->dwFileVersionLS)) + "." +
           std::to_string(LOWORD(info->dwFileVersionLS));
}

}  // namespace

Info MainModule() {
    Info info;
    HMODULE module = GetModuleHandleW(nullptr);
    info.base = reinterpret_cast<uintptr_t>(module);

    wchar_t path[MAX_PATH * 2];
    DWORD len = GetModuleFileNameW(module, path, static_cast<DWORD>(_countof(path)));
    if (len > 0 && len < _countof(path)) info.path.assign(path, len);

    if (auto nt = NtHeaders(module)) {
        info.size = nt->OptionalHeader.SizeOfImage;
        info.timestamp = nt->FileHeader.TimeDateStamp;
    }
    if (!info.path.empty()) info.fileVersion = FileVersion(info.path);
    return info;
}

std::optional<std::span<const uint8_t>> Section(HMODULE module, const char* name) {
    auto nt = NtHeaders(module);
    if (!nt) return std::nullopt;
    auto base = reinterpret_cast<const uint8_t*>(module);
    auto section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        char sectionName[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
        std::memcpy(sectionName, section->Name, IMAGE_SIZEOF_SHORT_NAME);
        if (std::strcmp(sectionName, name) == 0) {
            return std::span<const uint8_t>(base + section->VirtualAddress,
                                            section->Misc.VirtualSize);
        }
    }
    return std::nullopt;
}

}  // namespace overseer::module
