#pragma once

#include <filesystem>
#include <memory>

#include "engine/engine_bridge.h"

namespace overseer::engine {

// Engine bridge for SCUM (UE 4.27). Initialises in the background: locates
// reflection data and hooks UObject::ProcessEvent to get onto the game thread,
// where queued admin commands are run via a reflected UFunction. Until ready
// (or if anything required is missing) commands fail with a clear message
// instead of touching the game.
//
// NOTE: the entry point used today (MiscStatics.Test_ProcessAdminCommand) is a
// compiled-out stub in shipping builds, so commands reach the game thread but
// have no effect yet. The real dispatcher needs a valid command *executor*;
// see docs/engine-notes/scum-1.3.md and issue #9.
std::unique_ptr<EngineBridge> CreateScumEngineBridge(const std::filesystem::path& root);

}  // namespace overseer::engine
