// Thread-safe hand-off of admin commands from any thread (RCON, plugins) to
// the game thread. Producers call Push(); a game-thread hook calls Drain()
// once per tick.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

#include "engine/engine_bridge.h"

namespace overseer::engine {

struct PendingCommand {
    int64_t requestId = 0;
    std::string command;
    CommandCallback callback;
};

class CommandQueue {
public:
    explicit CommandQueue(size_t capacity = 256) : capacity_(capacity) {}

    // Returns false if the queue is full (the caller should report failure).
    bool Push(PendingCommand command) {
        std::lock_guard lock(mutex_);
        if (items_.size() >= capacity_) return false;
        items_.push_back(std::move(command));
        return true;
    }

    // Runs `execute` for up to `maxItems` queued commands, outside the lock so
    // the executor may take as long as it needs (or push more work).
    size_t Drain(const std::function<void(PendingCommand&)>& execute, size_t maxItems = 16) {
        std::deque<PendingCommand> batch;
        {
            std::lock_guard lock(mutex_);
            size_t n = items_.size() < maxItems ? items_.size() : maxItems;
            for (size_t i = 0; i < n; ++i) {
                batch.push_back(std::move(items_.front()));
                items_.pop_front();
            }
        }
        for (auto& item : batch) execute(item);
        return batch.size();
    }

    // Fails every queued command, e.g. when the bridge shuts down.
    void FailAll(CommandStatus status, const std::string& reason) {
        Drain([&](PendingCommand& c) { c.callback(c.requestId, status, reason); }, SIZE_MAX);
    }

    size_t Size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<PendingCommand> items_;
    size_t capacity_;
};

}  // namespace overseer::engine
