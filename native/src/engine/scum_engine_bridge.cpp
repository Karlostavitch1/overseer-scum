#include "engine/scum_engine_bridge.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <string>
#include <thread>
#include <utility>
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

using ProcessEventFn = void(__fastcall*)(void* object, void* function, void* params);

// Builds a UFunction parameter block from reflection, so offsets are never
// hard-coded. Strings it writes stay alive for the lifetime of the Params.
class Params {
public:
    Params(const ue4::Reflection& r, uintptr_t function) : reflection_(r), function_(function) {
        buffer_.resize(static_cast<size_t>(r.ParmsSize(function)), 0);
    }

    bool SetString(const char* name, const std::wstring& value) {
        int32_t off = reflection_.PropertyOffset(function_, name);
        if (off < 0 || off + static_cast<int32_t>(sizeof(FString)) > Size()) return false;
        strings_.push_back(value);
        auto& s = strings_.back();
        FString fs{s.c_str(), static_cast<int32_t>(s.size() + 1), static_cast<int32_t>(s.size() + 1)};
        std::memcpy(buffer_.data() + off, &fs, sizeof(fs));
        return true;
    }

    bool SetBool(const char* name, bool value) {
        int32_t off = reflection_.PropertyOffset(function_, name);
        if (off < 0 || off >= Size()) return false;
        buffer_[off] = value ? 1 : 0;
        return true;
    }

    bool SetObject(const char* name, uintptr_t object) {
        int32_t off = reflection_.PropertyOffset(function_, name);
        if (off < 0 || off + 8 > Size()) return false;
        std::memcpy(buffer_.data() + off, &object, sizeof(object));
        return true;
    }

    bool GetBool(const char* name) const {
        int32_t off = reflection_.PropertyOffset(function_, name);
        return off >= 0 && off < Size() && buffer_[off] != 0;
    }

    void* Data() { return buffer_.data(); }
    int32_t Size() const { return static_cast<int32_t>(buffer_.size()); }

private:
    const ue4::Reflection& reflection_;
    uintptr_t function_;
    std::vector<uint8_t> buffer_;
    std::deque<std::wstring> strings_;  // stable addresses
};

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

    const char* Name() const override { return "scum (UE 4.27, direct server ops)"; }

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

        const auto& layout = reflection_.GetLayout();
        if (!layout.processEventIndex) return Fail("UObject::ProcessEvent not located");

        // Wait for a running world: the GameMode is the call target/world context.
        while (!(gameMode_ = FindGameMode())) {
            if (std::chrono::steady_clock::now() > deadline) return Fail("no GameMode instance found");
            std::this_thread::sleep_for(5s);
        }

        // Resolve the server operations we implement directly (reflection, so no
        // hard-coded offsets). A missing one disables just that op.
        announceFn_ = ResolveOp("Function /Script/SCUM.ConZGameMode.SendHUDMessageToAll",
                                {{"Message", "StrProperty"}, {"beep", "BoolProperty"}});

        uintptr_t vtable = 0, processEvent = 0;
        if (!memory::Read(gameMode_, vtable) ||
            !memory::Read(vtable + layout.processEventIndex * sizeof(void*), processEvent)) {
            return Fail("cannot read UObject vtable");
        }

        gameThreadId_ = FindMainThreadId();
        if (!gameThreadId_) return Fail("cannot identify game thread");

        if (!hook::Install(reinterpret_cast<ProcessEventFn>(processEvent), &ProcessEventDetour,
                           &g_originalProcessEvent, error)) {
            return Fail("ProcessEvent hook: " + error);
        }
        ready_.store(true);
        log::Info("scum bridge: ready (game thread " + std::to_string(gameThreadId_) + ", world " +
                  reflection_.FullName(gameMode_) + ", announce=" + (announceFn_ ? "ok" : "unavailable") + ")");
    }

    // Returns the UFunction only if every (name,type) property matches — so a
    // changed signature disables the op instead of corrupting memory.
    uintptr_t ResolveOp(const std::string& path,
                        std::initializer_list<std::pair<const char*, const char*>> expected) {
        uintptr_t fn = reflection_.FindObject(path);
        if (!fn) {
            log::Warn("scum bridge: op not found: " + path);
            return 0;
        }
        auto props = reflection_.Properties(fn);
        for (auto [name, type] : expected) {
            bool ok = false;
            for (const auto& p : props) ok = ok || (p.name == name && p.type == type);
            if (!ok) {
                log::Warn("scum bridge: op signature mismatch, disabled: " + path);
                return 0;
            }
        }
        return fn;
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

    // Called on the game thread. Parses "verb args" (a leading '#' is allowed)
    // and dispatches to a directly-implemented server operation.
    void Execute(PendingCommand& command) {
        if (!WorldContextStillValid() && !(gameMode_ = FindGameMode())) {
            command.callback(command.requestId, CommandStatus::Failed, "no running world");
            return;
        }

        std::string line = command.command;
        size_t start = line.find_first_not_of(" \t#");
        if (start == std::string::npos) {
            command.callback(command.requestId, CommandStatus::Ok, "");
            return;
        }
        size_t sp = line.find_first_of(" \t", start);
        std::string verb = line.substr(start, sp == std::string::npos ? std::string::npos : sp - start);
        std::string args = sp == std::string::npos ? "" : line.substr(line.find_first_not_of(" \t", sp));
        for (char& c : verb) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

        CommandStatus status = CommandStatus::Ok;
        std::string result = Dispatch(verb, args, status);
        command.callback(command.requestId, status, result);
    }

    std::string Dispatch(const std::string& verb, const std::string& args, CommandStatus& status) {
        if (verb == "announce" || verb == "broadcast" || verb == "say") {
            return Announce(args, status);
        }
        status = CommandStatus::Unsupported;
        return "'" + verb +
               "' is not implemented server-side yet. Overseer runs a curated set of operations "
               "directly (no player required); full admin commands need a connected admin. "
               "Available: announce <message>.";
    }

    std::string Announce(const std::string& message, CommandStatus& status) {
        if (!announceFn_) {
            status = CommandStatus::Failed;
            return "announce unavailable (SendHUDMessageToAll not found on this build)";
        }
        if (message.empty()) {
            status = CommandStatus::Failed;
            return "usage: announce <message>";
        }
        Params p(reflection_, announceFn_);
        if (!p.SetString("Message", Utf8ToWide(message)) || !p.SetBool("beep", true)) {
            status = CommandStatus::Failed;
            return "announce: failed to marshal parameters";
        }
        // Call the trampoline directly so our own hook doesn't re-enter.
        g_originalProcessEvent(reinterpret_cast<void*>(gameMode_), reinterpret_cast<void*>(announceFn_), p.Data());
        log::Info("scum bridge: announced: " + message);
        return "announced to all players: " + message;
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
    uintptr_t gameMode_ = 0;
    uintptr_t announceFn_ = 0;
    DWORD gameThreadId_ = 0;
};

}  // namespace

std::unique_ptr<EngineBridge> CreateScumEngineBridge(const std::filesystem::path& root) {
    return std::make_unique<ScumEngineBridge>(root);
}

}  // namespace overseer::engine
