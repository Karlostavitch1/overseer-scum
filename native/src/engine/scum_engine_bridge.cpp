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

// UE4 TArray header: { ElementType* Data; int32 ArrayNum; int32 ArrayMax }.
struct FArrayHeader {
    uintptr_t data;
    int32_t num;
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

std::string WideToUtf8(const wchar_t* data, int len) {
    if (!data || len <= 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, data, len, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, len, s.data(), n, nullptr, nullptr);
    return s;
}

// Reads a UE FString at `address` into UTF-8, bounding the length defensively.
std::string ReadFString(uintptr_t address) {
    FString fs{};
    if (!memory::Read(address, fs) || !fs.data || fs.num <= 1 || fs.num > 4096) return {};
    std::wstring buf(static_cast<size_t>(fs.num), L'\0');  // num includes the NUL
    if (!memory::SafeRead(fs.data, buf.data(), static_cast<size_t>(fs.num) * sizeof(wchar_t))) return {};
    return WideToUtf8(buf.data(), fs.num - 1);  // drop the terminating NUL
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
        ResolvePlayerList();
        ResolveKick();

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
                  reflection_.FullName(gameMode_) + ", announce=" + (announceFn_ ? "ok" : "unavailable") +
                  ", players=" + (playerArrayOffset_ >= 0 ? "ok" : "unavailable") +
                  ", kick=" + (kickFn_ ? "ok" : "unavailable") + ")");
    }

    // Resolves the inherited offsets used to read the online-player list:
    // AGameStateBase::PlayerArray (TArray<APlayerState*>) and
    // APlayerState::PlayerNamePrivate (FString). Offsets only — no reads here.
    void ResolvePlayerList() {
        const uintptr_t gameStateBase = reflection_.FindObject("Class /Script/Engine.GameStateBase");
        const uintptr_t playerStateClass = reflection_.FindObject("Class /Script/Engine.PlayerState");
        if (!gameStateBase || !playerStateClass) {
            log::Warn("scum bridge: player list unavailable (engine classes not found)");
            return;
        }
        // GameState reached in O(1) from the GameMode; scanning the object array
        // on the game thread for each query would hitch the server.
        if (auto gs = reflection_.FindProperty(reflection_.ObjectClass(gameMode_), "GameState");
            gs && gs->type == "ObjectProperty") {
            gameStateOffset_ = gs->offset;
        }
        if (auto arr = reflection_.FindProperty(gameStateBase, "PlayerArray");
            arr && arr->type == "ArrayProperty") {
            playerArrayOffset_ = arr->offset;
        }
        if (auto nm = reflection_.FindProperty(playerStateClass, "PlayerNamePrivate");
            nm && nm->type == "StrProperty") {
            playerNameOffset_ = nm->offset;
        }
        if (gameStateOffset_ < 0 || playerArrayOffset_ < 0 || playerNameOffset_ < 0) {
            log::Warn("scum bridge: player list unavailable (GameState/PlayerArray/name not resolved)");
            gameStateOffset_ = playerArrayOffset_ = playerNameOffset_ = -1;
        }
    }

    // Resolves ConZGameMode.KickPlayer and, crucially, the exact UClass its
    // Player parameter expects, so we never pass a wrong-typed object to the
    // native call (which would crash the server). AActor::Owner lets us reach a
    // player's controller when the parameter wants a controller rather than the
    // player state.
    void ResolveKick() {
        kickFn_ = ResolveOp("Function /Script/SCUM.ConZGameMode.KickPlayer",
                            {{"Player", "ObjectProperty"},
                             {"kickReason", "StrProperty"},
                             {"ReturnValue", "BoolProperty"}});
        if (!kickFn_) return;
        const uintptr_t playerField = reflection_.PropertyField(kickFn_, "Player", false);
        kickTargetClass_ = reflection_.PropertyClassOf(playerField);
        if (!kickTargetClass_) {
            log::Warn("scum bridge: kick disabled (could not resolve Player parameter type)");
            kickFn_ = 0;
            return;
        }
        if (const uintptr_t actorClass = reflection_.FindObject("Class /Script/Engine.Actor")) {
            if (auto owner = reflection_.FindProperty(actorClass, "Owner")) actorOwnerOffset_ = owner->offset;
        }
        log::Info("scum bridge: kick target type = " + reflection_.FullName(kickTargetClass_));
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
        if (verb == "players" || verb == "listplayers" || verb == "list" || verb == "who") {
            return Players(status);
        }
        if (verb == "kick") {
            return Kick(args, status);
        }
        status = CommandStatus::Unsupported;
        return "'" + verb +
               "' is not implemented server-side yet. Overseer runs a curated set of operations "
               "directly (no player required); full admin commands need a connected admin. "
               "Available: #announce <message>, #players, #kick <player> [reason].";
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

    uintptr_t FindGameState() {
        if (gameStateOffset_ < 0) return 0;
        uintptr_t gameState = 0;
        memory::Read(gameMode_ + gameStateOffset_, gameState);
        return gameState;
    }

    // Reads the live APlayerState* array off the GameState. Read-only.
    std::vector<uintptr_t> PlayerStates() {
        std::vector<uintptr_t> players;
        const uintptr_t gameState = FindGameState();
        if (!gameState || playerArrayOffset_ < 0) return players;
        FArrayHeader arr{};
        if (!memory::Read(gameState + playerArrayOffset_, arr) || !arr.data || arr.num < 0 ||
            arr.num > 1024) {
            return players;
        }
        for (int32_t i = 0; i < arr.num; ++i) {
            uintptr_t ps = 0;
            if (memory::Read(arr.data + static_cast<uintptr_t>(i) * sizeof(uintptr_t), ps) && ps) {
                players.push_back(ps);
            }
        }
        return players;
    }

    std::string PlayerName(uintptr_t playerState) {
        if (playerNameOffset_ < 0) return {};
        return ReadFString(playerState + playerNameOffset_);
    }

    std::string Players(CommandStatus& status) {
        if (playerArrayOffset_ < 0) {
            status = CommandStatus::Failed;
            return "player list unavailable on this build";
        }
        const auto players = PlayerStates();
        if (players.empty()) return "0 players online.";
        std::string out = std::to_string(players.size()) +
                          (players.size() == 1 ? " player online:" : " players online:");
        for (uintptr_t ps : players) {
            std::string name = PlayerName(ps);
            out += "\n  " + (name.empty() ? std::string("(unknown)") : name);
        }
        return out;
    }

    // Picks the object KickPlayer's Player parameter expects for a given player
    // state: the state itself, or its owning controller (AActor::Owner). Returns
    // 0 unless the result IsA the resolved parameter class — so a mismatch is a
    // clean failure, never a wrong-typed native call.
    uintptr_t ResolveKickTarget(uintptr_t playerState) {
        if (reflection_.IsA(playerState, kickTargetClass_)) return playerState;
        if (actorOwnerOffset_ >= 0) {
            uintptr_t owner = 0;
            if (memory::Read(playerState + actorOwnerOffset_, owner) && owner &&
                reflection_.IsA(owner, kickTargetClass_)) {
                return owner;
            }
        }
        return 0;
    }

    std::string Kick(const std::string& args, CommandStatus& status) {
        if (!kickFn_ || !kickTargetClass_) {
            status = CommandStatus::Failed;
            return "kick unavailable (KickPlayer not found on this build)";
        }
        // "name" or "name reason words..."; names with spaces aren't supported —
        // the first token is the player, the rest is the reason.
        size_t sp = args.find_first_of(" \t");
        std::string name = sp == std::string::npos ? args : args.substr(0, sp);
        std::string reason = sp == std::string::npos ? "" : args.substr(args.find_first_not_of(" \t", sp));
        if (name.empty()) {
            status = CommandStatus::Failed;
            return "usage: kick <player> [reason]";
        }

        uintptr_t match = 0;
        bool ambiguous = false;
        for (uintptr_t ps : PlayerStates()) {
            if (EqualsIgnoreCase(PlayerName(ps), name)) {
                if (match) ambiguous = true;
                match = ps;
            }
        }
        if (!match) {
            status = CommandStatus::Failed;
            return "no player named '" + name + "' is online";
        }
        if (ambiguous) {
            status = CommandStatus::Failed;
            return "more than one player matches '" + name + "'; be more specific";
        }

        const uintptr_t target = ResolveKickTarget(match);
        if (!target) {
            status = CommandStatus::Failed;
            return "kick: could not resolve a '" + reflection_.ObjectName(kickTargetClass_) +
                   "' for that player";
        }

        Params p(reflection_, kickFn_);
        if (!p.SetObject("Player", target) ||
            !p.SetString("kickReason", Utf8ToWide(reason.empty() ? "Kicked by server" : reason))) {
            status = CommandStatus::Failed;
            return "kick: failed to marshal parameters";
        }
        g_originalProcessEvent(reinterpret_cast<void*>(gameMode_), reinterpret_cast<void*>(kickFn_), p.Data());
        log::Info("scum bridge: kicked: " + name);
        return "kicked " + name + (reason.empty() ? "" : " (" + reason + ")");
    }

    static bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
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
    // Player list (read-only): inherited offsets resolved once at start-up.
    int32_t gameStateOffset_ = -1;
    int32_t playerArrayOffset_ = -1;
    int32_t playerNameOffset_ = -1;
    // Kick: function, the exact UClass its Player parameter expects, and the
    // AActor::Owner offset used to reach a player's controller.
    uintptr_t kickFn_ = 0;
    uintptr_t kickTargetClass_ = 0;
    int32_t actorOwnerOffset_ = -1;
    DWORD gameThreadId_ = 0;
};

}  // namespace

std::unique_ptr<EngineBridge> CreateScumEngineBridge(const std::filesystem::path& root) {
    return std::make_unique<ScumEngineBridge>(root);
}

}  // namespace overseer::engine
