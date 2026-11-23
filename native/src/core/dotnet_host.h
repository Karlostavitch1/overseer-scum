#pragma once

#include <filesystem>
#include <string>

namespace overseer::dotnet {

// Loads the .NET runtime via hostfxr and resolves an [UnmanagedCallersOnly]
// static method. Returns nullptr and sets `error` on failure.
//
// hostfxr is located, in order:
//   1. <root>/dotnet/host/fxr/<newest>/hostfxr.dll   (app-local runtime)
//   2. %DOTNET_ROOT%/host/fxr/<newest>/hostfxr.dll
//   3. %ProgramFiles%/dotnet/host/fxr/<newest>/hostfxr.dll
void* LoadManagedEntryPoint(const std::filesystem::path& root,
                            const std::filesystem::path& assemblyPath,
                            const std::filesystem::path& runtimeConfigPath,
                            const wchar_t* typeName, const wchar_t* methodName,
                            std::string& error);

}  // namespace overseer::dotnet
