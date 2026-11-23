// The engine bridge is the only part of Overseer that knows about SCUM's
// internals. Everything above it (the .NET runtime, RCON, plugins) talks to
// the game exclusively through this interface, which keeps the
// reverse-engineered surface small and swappable between game versions.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace overseer::engine {

enum class CommandStatus : int32_t { Ok = 0, Failed = 1, Unsupported = 2 };

using CommandCallback =
    std::function<void(int64_t requestId, CommandStatus status, const std::string& output)>;

class EngineBridge {
public:
    virtual ~EngineBridge() = default;

    // Human-readable bridge name for logs.
    virtual const char* Name() const = 0;

    // Queue an admin command. Implementations must execute it on the game
    // thread and invoke `callback` exactly once, from any thread.
    // Returns false if the command could not be queued.
    virtual bool SubmitAdminCommand(int64_t requestId, std::string command,
                                    CommandCallback callback) = 0;
};

// Placeholder bridge used until the SCUM integration exists: answers every
// command with CommandStatus::Unsupported.
std::unique_ptr<EngineBridge> CreateNullEngineBridge();

}  // namespace overseer::engine
