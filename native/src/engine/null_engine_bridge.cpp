#include "engine/engine_bridge.h"

#include "core/log.h"

namespace overseer::engine {
namespace {

class NullEngineBridge final : public EngineBridge {
public:
    const char* Name() const override { return "null (engine integration not implemented)"; }

    bool SubmitAdminCommand(int64_t requestId, std::string command,
                            CommandCallback callback) override {
        log::Debug("null bridge received command: " + command);
        callback(requestId, CommandStatus::Unsupported,
                 "Overseer engine bridge is not implemented yet; command not executed: " +
                     command);
        return true;
    }
};

}  // namespace

std::unique_ptr<EngineBridge> CreateNullEngineBridge() {
    return std::make_unique<NullEngineBridge>();
}

}  // namespace overseer::engine
