# Engine notes: SCUM 1.3.x (UE 4.27.2)

Findings from the engine-bridge work in #9. Per the
[clean-room policy](../legal-and-fair-play.md) this records *what* exists and
*how* it was found — no addresses, offsets tables or dumps. Everything here is
re-derived at runtime by `native/src/ue4/reflection.cpp`.

## Build identification

- `SCUMServer.exe` file version `4.27.2.0`; SCUM's log prints
  `Engine Version: 4.27.2-<cl>+++scum+release-1.3.6`.
- Ships no PDB. Monolithic shipping build; game module is `/Script/SCUM`
  (asset paths use `ConZ_Files`, C++ classes are prefixed `ConZ`).
- Imports `VERSION.dll`, so the `version.dll` proxy loads. Also imports
  `D3DCOMPILER_43.dll`, which hosts must provide (legacy DirectX June 2010
  runtime) or the server exits with `0xC0000135`.
- BattlEye server (`BEServer_x64.dll`) initialises normally with Overseer
  loaded. The server process refuses `PROCESS_TERMINATE` from other processes
  once running — stop it from its console window or (once supported) via an
  admin command.

## Reflection

Found by structure, with no hard-coded addresses:

- **FNamePool**: the `.data` slot pointing at a block whose first two entries
  are `None` and `ByteProperty`.
- **GUObjectArray**: the `.data` structure matching `FChunkedFixedUObjectArray`
  (consistent element/chunk counts, item *i* reports `InternalIndex == i`).
- UStruct/UFunction offsets discovered from `UClass'Function'` →
  `SuperStruct == UClass'Struct'`, `AActor`'s children/properties and
  `AActor.K2_GetActorLocation` (native, one 12-byte parameter). All matched
  stock UE 4.27; `ProcessEvent` is at the usual vtable slot.
- ~1.66M objects once the island has streamed in.

## Admin commands

- One `UAdminCommand_<Verb>` class per command (~220), deriving from
  `/Script/SCUM.AdminCommand` which carries `_verb`, argument descriptions,
  `_requiredExecutorLevel`, `_shouldExecuteOnServer`/`Client`, cooldowns.
- Entry points visible in reflection:
  - `MiscStatics.Test_ProcessAdminCommand(WorldContextObject, commandText)` —
    static, needs no player. It is a real function (earlier "empty stub" note
    was a misread of an inner callee), but calling it with no executor does
    nothing visible — consistent with the admin system needing a player
    executor (below).
  - `PlayerRpcChannel.Chat_Server_ProcessAdminCommand(commandText)` — server
    RPC whose thunk forwards to a virtual `_Implementation`. Requires a
    `PlayerRpcChannel`, i.e. a connected player.
- Output of admin commands is not written to `SCUM.log`; it is sent back to
  the executing player. Capturing it needs a hook on that reply path.

## Dispatcher and execution model

Traced statically (capstone; see the structure, not the addresses):

- **Registry.** Admin commands live in a `TMap<FString, UAdminCommand*>`
  keyed by verb. The lookup walks the map's hash buckets comparing the verb
  string; a miss produces the `Unknown command` log line. Registration rejects
  a second command with a verb that is already taken
  (`Could not register admin command …`).
- **Executor.** Each `UAdminCommand` is executed against an *executor* — the
  prisoner/player who invoked it. `ExecuteImpl` fetches it from the command
  object and bails out with `… failed, invalid command executor` when it is
  null. So a command cannot simply be run "from nobody"; it needs a valid
  executor set on it first. This is why calling the empty
  `Test_ProcessAdminCommand` stub does nothing, and why the server-side path
  (`Chat_Server_ProcessAdminCommand`, which carries the calling player) is the
  real entry point.
- **Output.** Commands return their result to the executor through a virtual
  slot on `UAdminCommand` (not via the log). Hooking that one virtual captures
  the output of *every* command regardless of how it was invoked — this is the
  intended output-capture mechanism, replacing the first-pass log scraping.
  The slot index is to be resolved at runtime, never hard-coded.

## Executor is an interface (follow-up finding)

Static + reflection analysis refined the executor model:

- `AdminCommandExecutor` is a **UInterface** (`: CoreUObject.Interface`), not a
  plain object. The executor is therefore any UObject implementing
  `IAdminCommandExecutor` — in practice the player side (prisoner/controller).
- Executors carry an `EExecutorStatus`: `Regular < Elevated < Admin <
  SuperAdmin < Developer`. Commands gate on `_requiredExecutorLevel`.
- `AdminCommandRegistry` (UObject) holds all commands in `_commands` and is the
  verb→command map the dispatcher searches.

Open question that decides the no-player path: does any **server-side** object
(e.g. the GameMode or a server manager) implement `IAdminCommandExecutor` with
`Admin`/`SuperAdmin` status? If so, RCON can borrow it and run commands with no
players online. If the interface is implemented only by player objects, the
no-player path requires constructing a UObject that implements the interface
(harder), and the interim path is to require a connected admin.

Answering it needs the probe extended to enumerate interface implementers
(walk each `UClass`'s implemented-interfaces array) and dump the registry — then
one server restart to capture it.

### Answer (probed on 1.3.6)

The probe (`ue4/probe` → `logs/executor-report.txt`) discovered the
`UClass::Interfaces` offset by voting and enumerated implementers:

- **Exactly one** class implements `IAdminCommandExecutor`: **`ConZPlayerController`**
  (`… -> PlayerController -> Controller -> Actor`). It is player-side, and with
  no players connected there is **no instance**.
- `AdminCommandRegistry` exists server-side and holds the commands.

So the admin command system is intrinsically player-driven: there is no
server/console executor to borrow, and constructing a `ConZPlayerController`
(a heavy networked actor) just to execute commands is impractical and fragile.

**Design implication.** Overseer should not try to fake an admin chat executor.
Instead:

- **Common server operations** RCON users actually want — announce, kick, ban,
  list players, save, shutdown, set time/weather — are backed by functions on
  server-side objects (GameMode/GameState/registry/net driver). Call those
  directly via reflection; they need no player executor. (Confirming the exact
  functions is the next step.)
- **Full `#command` admin parity** is only possible against a connected admin's
  `ConZPlayerController`; offer it as a passthrough when an admin is online.

## Direct server operations (working)

Rather than impersonate admin chat, the bridge calls server-side functions
directly via reflection, on the game thread, with parameters marshalled from
reflected offsets (nothing hard-coded). Each op validates its function's
signature at start-up and disables itself on mismatch.

- **`announce <message>`** (aliases `broadcast`, `say`) →
  `ConZGameMode.SendHUDMessageToAll(Message, beep)`. Verified live on 1.3.6.
  (Player-visible confirmation needs a connected game client.)
- **`players`** (aliases `list`, `who`) → reads the live `APlayerState*`
  array off the GameState and prints each player's name. The GameState is
  reached in O(1) from `GameMode.GameState` (scanning the object array on the
  game thread for every query would hitch the server); `PlayerArray` and
  `PlayerNamePrivate` are the engine-standard `AGameStateBase`/`APlayerState`
  properties, resolved up the SuperStruct chain. Pure reads, no engine calls.
  Verified live: returns `0 players online.` with none connected.
- **`kick <player> [reason]`** → `ConZGameMode.KickPlayer(Player, kickReason)
  -> ReturnValue`. The `Player` parameter wants a **`ConZPlayerController`**
  (confirmed at runtime), not the player state — so the bridge reads the
  parameter's declared `UClass` (`FObjectPropertyBase::PropertyClass`) and only
  passes a target that `IsA` that class, reaching the controller via
  `AActor::Owner`. A wrong-typed object is a clean failure, never a native call,
  so an unverified kick cannot crash the server. Verified live to the no-target
  path (`no player named '…' is online`); the actual kick path needs a
  connected player to exercise.

Still located but not yet wired: `MiscStatics.BroadcastChatLine(WorldContext,
Text, EChatType)` — a chat-line broadcast needs an `EChatType` enum value
resolved at runtime (no UEnum reader yet) and a client to confirm the channel;
`announce` already covers "server message to everyone" via the HUD.

Unknown verbs return a clear "not implemented server-side; needs a connected
admin" message listing the available ops, instead of silently doing nothing.

## Status of the bridge

RCON → command queue → `ProcessEvent` hook on the game thread → reflected
UFunction call works end to end without destabilising the server. The direct
ops `announce`, `players` and `kick` run on a live 1.3.6 server. Remaining work:

1. **Executor.** Either (a) require a connected admin and route through
   `Chat_Server_ProcessAdminCommand` using that player's channel, or
   (b) construct/borrow a server-side executor so RCON can run commands with no
   players online. (b) is the valuable goal and the harder one; it needs the
   executor type and the registry/dispatcher entry mapped out (Ghidra would
   help here).
2. **Output capture** via the `UAdminCommand` reply virtual (above).
3. Resolve the registry and dispatcher objects through reflection at runtime,
   with signature checks, falling back to the null bridge if anything is
   missing.
