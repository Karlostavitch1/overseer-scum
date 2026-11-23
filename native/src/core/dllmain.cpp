#include <windows.h>

#include "core/bootstrap.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // Keep DllMain trivial: no runtime start-up under the loader lock.
        HANDLE thread = CreateThread(nullptr, 0, overseer::Bootstrap, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    // No managed calls on DLL_PROCESS_DETACH: the process is going away and
    // the loader lock is held. Graceful shutdown is driven from managed code.
    return TRUE;
}
