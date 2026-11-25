#include "engine/scum_engine_bridge.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "engine/command_queue.h"
#include "ue4/reflection.h"
#include "util/hook.h"
#include "util/safe_memory.h"

namespace overseer::engine {
namespace {

using namespace std::chrono_literals;

// UE4 FString as laid out in memory: TArray<TCHAR> { Data, ArrayNum, ArrayMax }.
struct FString {
    const wchar_t* data;
    int32_t num;  // includes the terminating NUL
    int32_t max;
};

// Parameter block of MiscStatics.Test_ProcessAdminCommand, verified against
// reflection at start-up (WorldContextObject @0, commandText @8, size 24).
struct TestProcessAdminCommandParams {
    void* worldContextObject;
    FString commandText;
};
static_assert(sizeof(TestProcessAdminCommandParams) == 24);

using ProcessEventFn = void(__fastcall*)(void* object, void* function, void* params);

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The UE game thread is the process's main thread: the earliest-created one.
DWORD FindMainThreadId() {
    DWORD best = 0;
    ULONGLONG bestTime = ~0ull;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
            ULONGLONG t = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            if (t < bestTime) {
                bestTime = t;
                best = entry.th32ThreadID;
            }
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return best;
}

class ScumEngineBridge;
ScumEngineBridge* g_instance = nullptr;
ProcessEventFn g_originalProcessEvent = nullptr;
thread_local bool t_inPump = false;

class ScumEngineBridge final : public EngineBridge {
public:
    explicit ScumEngineBridge(std::filesystem::path root) : root_(std::move(root)) {
        g_instance = this;
        std::thread([this] { InitializeInBackground(); }).detach();
    }

    const char* Name() const override { return "scum (UE 4.27, MiscStatics.Test_ProcessAdminCommand)"; }

    bool SubmitAdminCommand(int64_t requestId, std::string command, CommandCallback callback) override {
        if (!ready_.load()) {
            callback(requestId, CommandStatus::Failed,
                     failed_.load() ? "SCUM engine bridge failed to initialise: " + failure_
                                    : "SCUM engine bridge is still initialising; try again shortly");
            return true;
        }
        if (!queue_.Push({requestId, std::move(command), std::move(callback)})) return false;
        pending_.store(true, std::memory_order_release);
        return true;
    }

    // Called from the ProcessEvent hook on every engine event; must be cheap.
    void OnProcessEvent() {
        if (!pending_.load(std::memory_order_acquire) || GetCurrentThreadId() != gameThreadId_ || t_inPump) {
            return;
        }
        t_inPump = true;
        pending_.store(false, std::memory_order_release);
        queue_.Drain([this](PendingCommand& c) { Execute(c); }, 8);
        if (queue_.Size() > 0) pending_.store(true, std::memory_order_release);
        t_inPump = false;
    }

private:
    void Fail(const std::string& reason) {
        failure_ = reason;
        failed_.store(true);
        log::Error("scum bridge: " + reason);
    }

    void InitializeInBackground() {
        std::string error, lastError;
        const auto deadline = std::chrono::steady_clock::now() + 20min;
        while (!reflection_.Initialize(error)) {
            if (std::chrono::steady_clock::now() > deadline) return Fail("reflection: " + error);
            if (error != lastError) log::Info("scum bridge: waiting for engine (" + (lastError = error) + ")");
            std::this_thread::sleep_for(2s);
        }
        log::Info("scum bridge: reflection ready (" + std::to_string(reflection_.ObjectCount()) + " objects)");

        function_ = reflection_.FindObject("Function /Script/SCUM.MiscStatics.Test_ProcessAdminCommand");
        statics_ = reflection_.FindObject("MiscStatics /Script/SCUM.Default__MiscStatics");
        if (!function_ || !statics_) return Fail("MiscStatics.Test_ProcessAdminCommand not found");
        if (!VerifySignature()) return Fail("Test_ProcessAdminCommand signature changed; refusing to call it");

        const auto& layout = reflection_.GetLayout();
        if (!layout.processEventIndex) return Fail("UObject::ProcessEvent not located");
        uintptr_t vtable = 0, processEvent = 0;
        if (!memory::Read(statics_, vtable) ||
            !memory::Read(vtable + layout.processEventIndex * sizeof(void*), processEvent)) {
            return Fail("cannot read UObject vtable");
        }

        // Wait for a running world: the GameMode doubles as world context.
        while (!(gameMode_ = FindGameMode())) {
            if (std::chrono::steady_clock::now() > deadline) return Fail("no GameMode instance found");
            std::this_thread::sleep_for(5s);
        }

        gameThreadId_ = FindMainThreadId();
        if (!gameThreadId_) return Fail("cannot identify game thread");

        if (!hook::Install(reinterpret_cast<ProcessEventFn>(processEvent), &ProcessEventDetour,
                           &g_originalProcessEvent, error)) {
            return Fail("ProcessEvent hook: " + error);
        }
        processEvent_ = reinterpret_cast<ProcessEventFn>(processEvent);
        ready_.store(true);
        log::Info("scum bridge: ready (game thread " + std::to_string(gameThreadId_) + ", world context " +
                  reflection_.FullName(gameMode_) + ")");
    }

    bool VerifySignature() {
        auto params = reflection_.Properties(function_);
        uint16_t parmsSize = 0;
        memory::Read(function_ + reflection_.GetLayout().functionParmsSize, parmsSize);
        return params.size() == 2 && parmsSize == sizeof(TestProcessAdminCommandParams) &&
               params[0].name == "WorldContextObject" && params[0].type == "ObjectProperty" && params[0].offset == 0 &&
               params[1].name == "commandText" && params[1].type == "StrProperty" && params[1].offset == 8;
    }

    uintptr_t FindGameMode() {
        const uintptr_t gameModeBase = reflection_.FindObject("Class /Script/Engine.GameModeBase");
        if (!gameModeBase) return 0;
        uintptr_t found = 0;
        reflection_.ForEachObject([&](int32_t, uintptr_t object) {
            if (found) return;
            const std::string name = reflection_.ObjectName(object);
            if (name.rfind("Default__", 0) == 0) return;
            if (reflection_.IsA(object, gameModeBase)) found = object;
        });
        return found;
    }

    // GameMode can be replaced on map change; re-resolve if the cached object
    // is no longer at its slot in GUObjectArray.
    bool WorldContextStillValid() {
        int32_t index = -1;
        return memory::Read(gameMode_ + reflection_.GetLayout().objectIndex, index) &&
               reflection_.ObjectAt(index) == gameMode_;
    }

    void Execute(PendingCommand& command) {
        if (!WorldContextStillValid() && !(gameMode_ = FindGameMode())) {
            command.callback(command.requestId, CommandStatus::Failed, "no running world");
            return;
        }

        std::wstring text = Utf8ToWide(command.command);
        TestProcessAdminCommandParams params{};
        params.worldContextObject = reinterpret_cast<void*>(gameMode_);
        params.commandText = {text.c_str(), static_cast<int32_t>(text.size() + 1),
                              static_cast<int32_t>(text.size() + 1)};

        const auto logOffset = LogSize();
        log::Info("scum bridge: executing " + command.command);
        // Call the trampoline directly so our own hook doesn't see this call.
        g_originalProcessEvent(reinterpret_cast<void*>(statics_), reinterpret_cast<void*>(function_), &params);

        // Output capture (first iteration): the game reports results through
        // its log, written asynchronously. Collect new lines off-thread.
        std::thread([this, offset = logOffset, c = std::move(command)]() mutable {
            std::this_thread::sleep_for(750ms);
            c.callback(c.requestId, CommandStatus::Ok, ReadLogSince(offset));
        }).detach();
    }

    std::filesystem::path LogPath() const {
        // <root> = SCUM/Binaries/Win64/overseer -> SCUM/Saved/Logs/SCUM.log
        return root_.parent_path().parent_path().parent_path() / L"Saved" / L"Logs" / L"SCUM.log";
    }

    uintmax_t LogSize() const {
        std::error_code ec;
        auto size = std::filesystem::file_size(LogPath(), ec);
        return ec ? 0 : size;
    }

    std::string ReadLogSince(uintmax_t offset) const {
        std::ifstream in(LogPath(), std::ios::binary);
        if (!in) return "(command executed; game log unavailable)";
        in.seekg(static_cast<std::streamoff>(offset));
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // UE writes the log as UTF-16 only if forced; SCUM's is UTF-8/ANSI.
        std::string out;
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            start = end == std::string::npos ? text.size() : end + 1;
            if (line.find("Global Stats") != std::string::npos ||
                line.find("AuthorityTick took") != std::string::npos) {
                continue;  // periodic noise
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) out += line + "\n";
        }
        return out.empty() ? "(command executed; no output in game log)" : out;
    }

    static void __fastcall ProcessEventDetour(void* object, void* function, void* params) {
        g_originalProcessEvent(object, function, params);
        if (g_instance) g_instance->OnProcessEvent();
    }

    std::filesystem::path root_;
    ue4::Reflection reflection_;
    CommandQueue queue_;
    std::atomic<bool> pending_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
    std::string failure_;
    uintptr_t function_ = 0;
    uintptr_t statics_ = 0;
    uintptr_t gameMode_ = 0;
    DWORD gameThreadId_ = 0;
    ProcessEventFn processEvent_ = nullptr;
};

}  // namespace

std::unique_ptr<EngineBridge> CreateScumEngineBridge(const std::filesystem::path& root) {
    return std::make_unique<ScumEngineBridge>(root);
}

}  // namespace overseer::engine
