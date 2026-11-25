// Thin wrapper over MinHook (BSD-2-Clause) for inline function hooks.
#pragma once

#include <string>

namespace overseer::hook {

// Initialises MinHook once. Safe to call repeatedly.
bool Initialize(std::string& error);

// Installs and enables a hook on `target`. On success `*original` receives a
// trampoline that calls the unhooked function.
bool Install(void* target, void* detour, void** original, std::string& error);

template <typename Fn>
bool Install(Fn target, Fn detour, Fn* original, std::string& error) {
    return Install(reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour),
                   reinterpret_cast<void**>(original), error);
}

// Disables and removes all hooks (best effort; used on shutdown).
void RemoveAll();

}  // namespace overseer::hook
