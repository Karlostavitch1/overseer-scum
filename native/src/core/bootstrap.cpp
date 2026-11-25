#include "core/bootstrap.h"

#include <filesystem>
#include <memory>
#include <string>

#include "core/dotnet_host.h"
#include "core/log.h"
#include "engine/engine_bridge.h"
#include "engine/scum_engine_bridge.h"
#include "overseer/native_api.h"
#include "ue4/probe.h"
#include "util/module.h"

namespace overseer {
namespace {

std::unique_ptr<engine::EngineBridge> g_bridge;
overseer_native_api g_native{};
overseer_managed_api g_managed{};
std::string g_rootUtf8;

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                  nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr,
                        nullptr);
    return out;
}

// --- Functions exported to managed code via overseer_native_api -------------

void ManagedLog(int32_t level, const char* message) {
    if (level < 0 || level > 3) level = static_cast<int32_t>(log::Level::Info);
    log::Write(static_cast<log::Level>(level), message ? message : "");
}

int32_t ManagedSubmitCommand(int64_t requestId, const char* command) {
    if (!g_bridge || !command) return 1;
    bool queued = g_bridge->SubmitAdminCommand(
        requestId, command,
        [](int64_t id, engine::CommandStatus status, const std::string& output) {
            if (g_managed.on_command_result) {
                g_managed.on_command_result(id, static_cast<int32_t>(status), output.c_str());
            }
        });
    return queued ? 0 : 1;
}

}  // namespace

DWORD WINAPI Bootstrap(LPVOID selfModule) {
    namespace fs = std::filesystem;

    wchar_t modulePath[MAX_PATH * 2];
    DWORD len = GetModuleFileNameW(static_cast<HMODULE>(selfModule), modulePath,
                                   static_cast<DWORD>(_countof(modulePath)));
    if (len == 0 || len >= _countof(modulePath)) return 1;

    // <root>/bin/overseer_native.dll -> <root>
    const fs::path root = fs::path(modulePath).parent_path().parent_path();
    g_rootUtf8 = ToUtf8(root.wstring());

    log::Open(root / L"logs" / L"native.log");
    log::Info("Overseer native " OVERSEER_VERSION " starting; root=" + g_rootUtf8);

    const auto host = module::MainModule();
    char hostInfo[160];
    snprintf(hostInfo, sizeof(hostInfo), "base=0x%llx size=0x%zx timestamp=0x%08x version=%s",
             static_cast<unsigned long long>(host.base), host.size, host.timestamp,
             host.fileVersion.empty() ? "?" : host.fileVersion.c_str());
    log::Info("host process: " + ToUtf8(host.path) + " " + hostInfo);

    ue4::StartProbeIfEnabled(root);

    // Use the SCUM bridge only inside SCUMServer.exe; OVERSEER_BRIDGE=null
    // forces the inert bridge (useful when a game update breaks the hooks).
    wchar_t bridgeOverride[16] = {};
    GetEnvironmentVariableW(L"OVERSEER_BRIDGE", bridgeOverride, 16);
    const bool isScum = _wcsicmp(fs::path(host.path).filename().c_str(), L"SCUMServer.exe") == 0;
    if (isScum && _wcsicmp(bridgeOverride, L"null") != 0) {
        g_bridge = engine::CreateScumEngineBridge(root);
    } else {
        g_bridge = engine::CreateNullEngineBridge();
    }
    log::Info(std::string("engine bridge: ") + g_bridge->Name());

    const fs::path managedDir = root / L"managed";
    std::string error;
    void* entry = dotnet::LoadManagedEntryPoint(
        root, managedDir / L"Overseer.Host.dll", managedDir / L"Overseer.Host.runtimeconfig.json",
        L"Overseer.Host.Interop.NativeEntry, Overseer.Host", L"Initialize", error);
    if (!entry) {
        log::Error("failed to start .NET runtime: " + error);
        return 1;
    }

    g_native.size = sizeof(g_native);
    g_native.version = OVERSEER_ABI_VERSION;
    g_native.root_directory = g_rootUtf8.c_str();
    g_native.log = &ManagedLog;
    g_native.submit_command = &ManagedSubmitCommand;

    g_managed.size = sizeof(g_managed);
    g_managed.version = OVERSEER_ABI_VERSION;

    int32_t rc = reinterpret_cast<overseer_managed_initialize_fn>(entry)(&g_native, &g_managed);
    if (rc != 0) {
        log::Error("Overseer.Host initialize returned " + std::to_string(rc));
        return 1;
    }
    log::Info("Overseer.Host initialized");
    return 0;
}

}  // namespace overseer
