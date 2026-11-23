# Architecture

Overseer is split into layers so that the part that depends on SCUM internals
(and breaks when the game updates) is as small as possible.

```
┌──────────────────────── SCUMServer.exe ────────────────────────────────┐
│                                                                        │
│  version.dll (proxy)                                                   │
│    forwards all exports to System32\version.dll                        │
│    on load: LoadLibrary(overseer\bin\overseer_native.dll)              │
│                                                                        │
│  overseer_native.dll (C++)                                             │
│    Bootstrap thread ── logging (logs/native.log)                       │
│                     ── EngineBridge  ◄── the only SCUM-specific code   │
│                     ── hostfxr: start .NET 8, call NativeEntry.Initialize
│                                                                        │
│  ── ABI: native/include/overseer/native_api.h ─────────────────────────│
│                                                                        │
│  Overseer.Host (.NET 8)                                                │
│    OverseerRuntime ── ConfigStore     config/overseer.json             │
│                    ── CommandRouter   "#..." → engine, else registry   │
│                    ── PluginManager   plugins/*.dll, collectible ALCs  │
│                    ── RconServer      Source RCON on 127.0.0.1:27015   │
└────────────────────────────────────────────────────────────────────────┘
        ▲
        │ TCP (Source RCON)
   RCON clients: overseer-rcon, mcrcon, web panels, Discord bots …
```

## Layers

### 1. Proxy loader — `native/proxy`
A `version.dll` placed next to `SCUMServer.exe`. Windows loads DLLs from the
application directory first, so the server picks it up automatically. All 17
real exports are linker-forwarded to `C:\Windows\System32\version.dll`. Its
only job is to load `overseer_native.dll` from a worker thread (never under the
loader lock). Setting `OVERSEER_DISABLE=1` skips loading.

### 2. Native core — `native/src`
- **Bootstrap** derives the Overseer root from its own path
  (`<root>/bin/overseer_native.dll`), opens the log, creates the engine bridge
  and starts .NET.
- **dotnet_host** finds `hostfxr.dll` (app-local `<root>/dotnet`, then
  `DOTNET_ROOT`, then `%ProgramFiles%\dotnet`), initialises the runtime from
  `managed/Overseer.Host.runtimeconfig.json` and resolves the
  `[UnmanagedCallersOnly]` entry point. No .NET SDK headers are needed to build.
- **EngineBridge** (`native/src/engine/engine_bridge.h`) is the interface to the
  game. Today only `NullEngineBridge` exists, which answers every command with
  `Unsupported`. See the roadmap for the real implementation.

### 3. Native ⇄ managed ABI
Defined once in [`native_api.h`](../native/include/overseer/native_api.h) and
mirrored in `managed/src/Overseer.Host/Interop/NativeApi.cs`. Plain C structs of
function pointers, versioned with `OVERSEER_ABI_VERSION`. Strings are UTF-8.
Commands are asynchronous: managed calls `submit_command(id, text)`, native
later calls `on_command_result(id, status, output)` — from the game thread once
a real bridge exists.

### 4. Managed runtime — `managed/src/Overseer.Host`
Host-agnostic: `OverseerRuntime` only needs an `IEngine`. In SCUM that is
`NativeEngine`; in `Overseer.DevHost` and the tests it is a fake. That is what
lets almost all development happen without a game server.

Command routing:
- Lines starting with `#` are SCUM admin commands and go straight to the engine.
- Anything else is looked up in the registry (built-ins `overseer.*` and plugin
  commands).

### 5. Plugins — `Overseer.Api`
Plugins reference only `Overseer.Api`. Each plugin DLL loads into its own
collectible `AssemblyLoadContext` (loaded from a byte copy so the file can be
replaced), with `Overseer.Api` shared from the host so type identity holds even
though hostfxr itself loads the host into an isolated context.
`overseer.reload` unloads and reloads all plugins.

## Server-side layout

```
SCUM/Binaries/Win64/
├─ SCUMServer.exe
├─ version.dll                         proxy
└─ overseer/                           Overseer root
   ├─ bin/overseer_native.dll
   ├─ managed/Overseer.Host.dll        + Overseer.Api.dll, Overseer.Rcon.dll,
   │                                     *.deps.json, *.runtimeconfig.json
   ├─ dotnet/                          optional app-local .NET 8 runtime
   ├─ config/overseer.json
   ├─ plugins/
   ├─ data/<plugin>/
   └─ logs/native.log
```

## Threading

- Native bootstrap runs on its own thread; managed start-up is then pushed to
  the thread pool so `Initialize` returns immediately.
- RCON connections are independent; commands on one connection run in order.
- The engine bridge is responsible for marshalling admin commands onto the game
  thread and calling back with the result. Managed code never touches game
  memory directly.
