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

## Status of the bridge

RCON → command queue → `ProcessEvent` hook on the game thread → reflected
UFunction call works end to end without destabilising the server. What's
missing is a working, player-independent entry into the admin command
dispatcher, and an output capture hook. Next step: follow
`Chat_Server_ProcessAdminCommand_Implementation` to the shared dispatcher and
determine what executor context it needs.
