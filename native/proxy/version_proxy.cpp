// version.dll proxy.
//
// SCUMServer.exe (like most Win32 programs) imports version.dll. Windows
// searches the application directory first, so placing this DLL next to the
// executable gets it loaded at startup. Every real export is forwarded to the
// system copy; the only thing we add is loading overseer_native.dll.
//
// If a future SCUM build stops importing version.dll, add another proxy
// (e.g. dwmapi.dll / winhttp.dll) following the same pattern.
#include <windows.h>

#include <stdlib.h>
#include <wchar.h>

#define FORWARD(name, ordinal) \
    __pragma(comment(linker, "/EXPORT:" #name "=C:\\Windows\\System32\\version." #name ",@" #ordinal))

FORWARD(GetFileVersionInfoA, 1)
FORWARD(GetFileVersionInfoByHandle, 2)
FORWARD(GetFileVersionInfoExA, 3)
FORWARD(GetFileVersionInfoExW, 4)
FORWARD(GetFileVersionInfoSizeA, 5)
FORWARD(GetFileVersionInfoSizeExA, 6)
FORWARD(GetFileVersionInfoSizeExW, 7)
FORWARD(GetFileVersionInfoSizeW, 8)
FORWARD(GetFileVersionInfoW, 9)
FORWARD(VerFindFileA, 10)
FORWARD(VerFindFileW, 11)
FORWARD(VerInstallFileA, 12)
FORWARD(VerInstallFileW, 13)
FORWARD(VerLanguageNameA, 14)
FORWARD(VerLanguageNameW, 15)
FORWARD(VerQueryValueA, 16)
FORWARD(VerQueryValueW, 17)

namespace {

HMODULE g_self = nullptr;

DWORD WINAPI LoadOverseer(LPVOID) {
    wchar_t path[MAX_PATH * 2];
    DWORD len = GetModuleFileNameW(g_self, path, static_cast<DWORD>(_countof(path)));
    if (len == 0 || len >= _countof(path)) return 1;

    // Strip file name -> directory of version.dll (Binaries\Win64).
    for (DWORD i = len; i > 0; --i) {
        if (path[i - 1] == L'\\') { path[i] = L'\0'; break; }
    }
    if (wcscat_s(path, L"overseer\\bin\\overseer_native.dll") != 0) return 1;

    // Opt-out switch for server owners: OVERSEER_DISABLE=1
    wchar_t flag[8];
    if (GetEnvironmentVariableW(L"OVERSEER_DISABLE", flag, 8) > 0 && flag[0] == L'1') return 0;

    if (!LoadLibraryW(path)) {
        OutputDebugStringW(L"[overseer] failed to load overseer_native.dll\n");
        return 1;
    }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        // Never LoadLibrary under the loader lock — defer to a worker thread.
        HANDLE thread = CreateThread(nullptr, 0, LoadOverseer, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
