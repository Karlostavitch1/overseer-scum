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

// Writes to logs/executor-report.txt: which classes implement the admin command
// executor interface, whether any have live (non-CDO) instances, and their
// parent chain (to tell server-side from player-side). This is the fact that
// decides whether RCON can run commands with no players online.
void InvestigateExecutor(Reflection& r, const std::filesystem::path& root) {
    std::string line = "executor report: ";
    FILE* f = nullptr;
    const auto path = (root / L"logs" / L"executor-report.txt").wstring();
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        log::Error("executor report: cannot open file");
        return;
    }

    if (!r.DiscoverInterfaceOffset()) {
        fprintf(f, "Could not discover UClass::Interfaces offset.\n");
        fclose(f);
        log::Warn(line + "interface offset not found");
        return;
    }
    fprintf(f, "UClass::Interfaces offset = 0x%x\n\n", r.GetLayout().classInterfaces);

    const uintptr_t iface = r.FindObject("Class /Script/SCUM.AdminCommandExecutor");
    if (!iface) {
        fprintf(f, "AdminCommandExecutor interface class not found.\n");
        fclose(f);
        log::Warn(line + "interface class not found");
        return;
    }

    auto parents = [&](uintptr_t cls) {
        std::string chain;
        int depth = 0;
        for (uintptr_t c = r.SuperStruct(cls); c && depth < 12; c = r.SuperStruct(c), ++depth) {
            chain += " -> " + r.ObjectName(c);
        }
        return chain;
    };

    const auto implementers = r.Implementers(iface);
    fprintf(f, "%zu class(es) implement IAdminCommandExecutor:\n", implementers.size());
    int serverSideLive = 0;
    for (uintptr_t cls : implementers) {
        const std::string name = r.ObjectName(cls);
        const std::string chain = parents(cls);
        const uintptr_t live = r.FindLiveInstance(cls);
        // Heuristic: player-side types mention Controller/Pawn/Character/Prisoner/Player.
        const bool playerish = Contains(name + chain, "Controller") || Contains(name + chain, "Pawn") ||
                               Contains(name + chain, "Character") || Contains(name + chain, "Prisoner") ||
                               Contains(name + chain, "Player");
        fprintf(f, "  %-44s%s\n      super:%s\n      live non-CDO instance: %s%s\n", name.c_str(),
                playerish ? "  [player-side]" : "  [server-side?]", chain.c_str(),
                live ? r.FullName(live).c_str() : "(none)",
                (live && !playerish) ? "   <== usable server executor candidate" : "");
        if (live && !playerish) ++serverSideLive;
    }

    // Registry sanity: how many commands are registered.
    const uintptr_t registry = r.FindLiveInstance(r.FindObject("Class /Script/SCUM.AdminCommandRegistry"));
    if (registry) fprintf(f, "\nAdminCommandRegistry live instance: %s\n", r.FullName(registry).c_str());

    fprintf(f, "\nConclusion: %s\n",
            serverSideLive > 0
                ? "a server-side executor instance exists -> no-player RCON execution looks feasible."
                : "no non-player executor instance found -> commands likely require a connected admin.");
    fclose(f);
    log::Info(line + std::to_string(implementers.size()) + " implementer(s), " +
              std::to_string(serverSideLive) + " server-side live candidate(s)");
}

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

    InvestigateExecutor(reflection, root);

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
