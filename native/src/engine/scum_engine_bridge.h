#pragma once

#include <filesystem>
#include <memory>

#include "engine/engine_bridge.h"

namespace overseer::engine {

// Engine bridge for SCUM (UE 4.27). Initialises in the background: locates
// reflection data, resolves `MiscStatics.Test_ProcessAdminCommand`, and
// hooks UObject::ProcessEvent to get onto the game thread. Until ready (or if
// anything required is missing) commands fail with a clear message instead of
// touching the game.
std::unique_ptr<EngineBridge> CreateScumEngineBridge(const std::filesystem::path& root);

}  // namespace overseer::engine
