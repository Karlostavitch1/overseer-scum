#include "core/dotnet_host.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

#include "core/log.h"

namespace overseer::dotnet {
namespace {

// Minimal subset of hostfxr.h / coreclr_delegates.h (MIT, dotnet/runtime),
// declared inline so building needs no .NET SDK headers.
using hostfxr_handle = void*;
using hostfxr_initialize_for_runtime_config_fn = int32_t(__cdecl*)(
    const wchar_t* runtimeConfigPath, const void* parameters, hostfxr_handle* handle);
using hostfxr_get_runtime_delegate_fn = int32_t(__cdecl*)(hostfxr_handle handle, int32_t type,
                                                          void** delegate);
using hostfxr_close_fn = int32_t(__cdecl*)(hostfxr_handle handle);
using load_assembly_and_get_function_pointer_fn = int32_t(__stdcall*)(
    const wchar_t* assemblyPath, const wchar_t* typeName, const wchar_t* methodName,
    const wchar_t* delegateTypeName, void* reserved, void** delegate);

constexpr int32_t kHdtLoadAssemblyAndGetFunctionPointer = 5;
// UNMANAGEDCALLERSONLY_METHOD sentinel from coreclr_delegates.h.
const wchar_t* const kUnmanagedCallersOnly = reinterpret_cast<const wchar_t*>(-1);

std::optional<std::filesystem::path> NewestHostFxr(const std::filesystem::path& dotnetRoot) {
    std::error_code ec;
    auto fxrDir = dotnetRoot / L"host" / L"fxr";
    if (!std::filesystem::is_directory(fxrDir, ec)) return std::nullopt;

    // Pick the highest version directory that contains hostfxr.dll. Version
    // directories are semver; compare numerically component by component.
    auto parse = [](const std::wstring& s) {
        std::vector<int> parts;
        size_t pos = 0;
        while (pos < s.size()) {
            parts.push_back(_wtoi(s.c_str() + pos));
            size_t dot = s.find_first_of(L".-", pos);
            if (dot == std::wstring::npos || s[dot] == L'-') break;
            pos = dot + 1;
        }
        return parts;
    };

    std::optional<std::filesystem::path> best;
    std::vector<int> bestVersion;
    for (const auto& entry : std::filesystem::directory_iterator(fxrDir, ec)) {
        auto candidate = entry.path() / L"hostfxr.dll";
        if (!std::filesystem::exists(candidate, ec)) continue;
        auto version = parse(entry.path().filename().wstring());
        if (!best || version > bestVersion) {
            best = candidate;
            bestVersion = version;
        }
    }
    return best;
}

std::optional<std::filesystem::path> FindHostFxr(const std::filesystem::path& root) {
    if (auto p = NewestHostFxr(root / L"dotnet")) return p;

    wchar_t buffer[MAX_PATH];
    if (GetEnvironmentVariableW(L"DOTNET_ROOT", buffer, MAX_PATH) > 0) {
        if (auto p = NewestHostFxr(buffer)) return p;
    }
    if (GetEnvironmentVariableW(L"ProgramFiles", buffer, MAX_PATH) > 0) {
        if (auto p = NewestHostFxr(std::filesystem::path(buffer) / L"dotnet")) return p;
    }
    return std::nullopt;
}

std::string Hex(int32_t rc) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08X", static_cast<uint32_t>(rc));
    return buf;
}

}  // namespace

void* LoadManagedEntryPoint(const std::filesystem::path& root,
                            const std::filesystem::path& assemblyPath,
                            const std::filesystem::path& runtimeConfigPath,
                            const wchar_t* typeName, const wchar_t* methodName,
                            std::string& error) {
    auto hostfxrPath = FindHostFxr(root);
    if (!hostfxrPath) {
        error = "could not find hostfxr.dll; install the .NET 8 runtime (x64)";
        return nullptr;
    }
    log::Info("using hostfxr: " + hostfxrPath->string());

    HMODULE hostfxr = LoadLibraryW(hostfxrPath->c_str());
    if (!hostfxr) {
        error = "LoadLibrary(hostfxr) failed";
        return nullptr;
    }

    auto initialize = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        GetProcAddress(hostfxr, "hostfxr_initialize_for_runtime_config"));
    auto getDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        GetProcAddress(hostfxr, "hostfxr_get_runtime_delegate"));
    auto close = reinterpret_cast<hostfxr_close_fn>(GetProcAddress(hostfxr, "hostfxr_close"));
    if (!initialize || !getDelegate || !close) {
        error = "hostfxr.dll is missing required exports (need .NET 6+)";
        return nullptr;
    }

    hostfxr_handle handle = nullptr;
    int32_t rc = initialize(runtimeConfigPath.c_str(), nullptr, &handle);
    // 0 = Success, 1 = Success_HostAlreadyInitialized, 2 = Success_DifferentRuntimeProperties
    if (rc < 0 || !handle) {
        error = "hostfxr_initialize_for_runtime_config failed: " + Hex(rc);
        if (handle) close(handle);
        return nullptr;
    }

    void* loadFn = nullptr;
    rc = getDelegate(handle, kHdtLoadAssemblyAndGetFunctionPointer, &loadFn);
    close(handle);
    if (rc < 0 || !loadFn) {
        error = "hostfxr_get_runtime_delegate failed: " + Hex(rc);
        return nullptr;
    }

    void* entry = nullptr;
    rc = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(loadFn)(
        assemblyPath.c_str(), typeName, methodName, kUnmanagedCallersOnly, nullptr, &entry);
    if (rc < 0 || !entry) {
        error = "load_assembly_and_get_function_pointer failed: " + Hex(rc);
        return nullptr;
    }
    return entry;
}

}  // namespace overseer::dotnet
