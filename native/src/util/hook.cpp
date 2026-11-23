#include "util/hook.h"

#include <MinHook.h>

#include <mutex>

namespace overseer::hook {
namespace {

std::mutex g_mutex;
bool g_initialized = false;

}  // namespace

bool Initialize(std::string& error) {
    std::lock_guard lock(g_mutex);
    if (g_initialized) return true;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        error = std::string("MH_Initialize: ") + MH_StatusToString(status);
        return false;
    }
    g_initialized = true;
    return true;
}

bool Install(void* target, void* detour, void** original, std::string& error) {
    if (!Initialize(error)) return false;
    std::lock_guard lock(g_mutex);
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        error = std::string("MH_CreateHook: ") + MH_StatusToString(status);
        return false;
    }
    status = MH_EnableHook(target);
    if (status != MH_OK) {
        MH_RemoveHook(target);
        error = std::string("MH_EnableHook: ") + MH_StatusToString(status);
        return false;
    }
    return true;
}

void RemoveAll() {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_initialized = false;
}

}  // namespace overseer::hook
