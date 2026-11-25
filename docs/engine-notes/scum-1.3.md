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
    static, needs no player. **It is an empty stub in shipping builds** (the
    native implementation is a bare `ret`). The bridge can call it safely but
    it does nothing. Found by disassembling the UFunction's native thunk.
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

## Status of the bridge

RCON → command queue → `ProcessEvent` hook on the game thread → reflected
UFunction call works end to end without destabilising the server. Remaining
work for a functioning bridge:

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
