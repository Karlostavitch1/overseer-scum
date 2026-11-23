#pragma once

#include <windows.h>

namespace overseer {

// Runs on a worker thread after overseer_native.dll is loaded. Sets up
// logging, the engine bridge and the .NET runtime, then hands control to
// Overseer.Host.
DWORD WINAPI Bootstrap(LPVOID selfModule);

}  // namespace overseer
