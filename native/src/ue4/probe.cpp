#include "ue4/probe.h"

#include <windows.h>

#include <chrono>
#include <string>
#include <thread>

#include "core/log.h"
#include "ue4/reflection.h"

namespace overseer::ue4 {
namespace {

bool Enabled(const std::filesystem::path& root) {
    wchar_t flag[8];
    if (GetEnvironmentVariableW(L"OVERSEER_REFLECTION_DUMP", flag, 8) > 0 && flag[0] == L'1') return true;
    std::error_code ec;
    return std::filesystem::exists(root / L"config" / L"dump-reflection", ec);
}

bool Contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

void Run(std::filesystem::path root) {
    using namespace std::chrono_literals;
    Reflection reflection;
    std::string error, lastError;
    const auto deadline = std::chrono::steady_clock::now() + 20min;

    while (std::chrono::steady_clock::now() < deadline) {
        if (reflection.Initialize(error)) break;
        if (error != lastError) {
            log::Info("reflection probe: waiting (" + error + ")");
            lastError = error;
        }
        std::this_thread::sleep_for(5s);
    }
    if (!reflection.Ready()) {
        log::Error("reflection probe: gave up: " + error);
        return;
    }

    // Wait for the object count to settle (map and content finished loading).
    int32_t previous = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        const int32_t count = reflection.ObjectCount();
        if (count == previous && count > 50000) break;
        previous = count;
        std::this_thread::sleep_for(15s);
    }

    const auto& l = reflection.GetLayout();
    log::Info("reflection probe: ready; objects=" + std::to_string(reflection.ObjectCount()) +
              " super=" + std::to_string(l.structSuper) + " children=" + std::to_string(l.structChildren) +
              " childProps=" + std::to_string(l.structChildProperties) +
              " fnFlags=" + std::to_string(l.functionFlags) + " fnNative=" + std::to_string(l.functionNative) +
              " processEvent=" + std::to_string(l.processEventIndex));

    const auto path = root / L"logs" / L"reflection-dump.txt";
    const bool ok = reflection.Dump(path.wstring(), [](const std::string& full) {
        if (full.rfind("Class /Script/ConZ.", 0) == 0) return true;
        if (Contains(full, "AdminCommand")) return true;
        if (full.rfind("Function /Script/ConZ.", 0) == 0 &&
            (Contains(full, "Chat") || Contains(full, "Admin") || Contains(full, "Command"))) {
            return true;
        }
        return false;
    }, error);
    if (!ok) log::Error("reflection probe: dump failed: " + error);
}

}  // namespace

void StartProbeIfEnabled(const std::filesystem::path& root) {
    if (!Enabled(root)) return;
    log::Info("reflection probe enabled");
    std::thread(Run, root).detach();
}

}  // namespace overseer::ue4
