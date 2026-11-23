# Overseer

**An open source, server-side framework for SCUM dedicated servers — RCON, admin tooling and C# plugins.**

SCUM ships no remote console. Overseer adds one: it loads into `SCUMServer.exe`,
hosts the .NET 8 runtime, exposes a standard
[Source RCON](docs/rcon-protocol.md) endpoint, and lets server owners extend
their server with C# plugins.

> **Status: pre-alpha.** The managed runtime, RCON server, plugin loader and
> dev host work today. The engine bridge — the piece that actually executes
> SCUM admin commands inside the game — is not implemented yet. See the
> [roadmap](docs/roadmap.md). Contributions very welcome.

## Why

- **Open.** MIT licensed, built in the open, no closed binaries.
- **Standard protocols.** Any Source-RCON client (mcrcon, rcon-cli, BattleMetrics-style tools) can talk to it.
- **Plugins in C#.** A small, stable API (`Overseer.Api`) with commands, logging and server access.
- **Safe defaults.** RCON binds to localhost with a randomly generated password until you change it.
- **Server-side only.** Overseer never touches game clients. See [fair play](docs/legal-and-fair-play.md).

## How it fits together

```
SCUMServer.exe
 └─ version.dll  (proxy, forwards to System32\version.dll)
     └─ overseer/bin/overseer_native.dll   C++: bootstrap, logging, engine bridge
         └─ .NET 8 (hostfxr)
             └─ Overseer.Host               runtime, config, command router
                 ├─ Overseer.Rcon           Source RCON server  ◄── RCON clients
                 └─ plugins/*.dll           your plugins (Overseer.Api)
```

Read [docs/architecture.md](docs/architecture.md) for the details.

## Repository layout

| Path | What |
| --- | --- |
| `native/` | C++ (CMake): `version.dll` proxy and `overseer_native.dll` core |
| `managed/src/Overseer.Api` | Public plugin API — the only assembly plugins reference |
| `managed/src/Overseer.Rcon` | Source RCON protocol: codec, server, client |
| `managed/src/Overseer.Host` | Runtime loaded into the server: config, plugins, routing, native interop |
| `managed/tools/Overseer.DevHost` | Runs the runtime outside SCUM with a fake engine — develop without a server |
| `managed/tools/Overseer.RconCli` | Minimal RCON command-line client |
| `managed/tests/` | xUnit tests |
| `plugins/examples/` | Example plugins |
| `docs/` | Architecture, protocol, install and plugin guides |

## Quick start (no SCUM server needed)

Requires the [.NET 8 SDK](https://dotnet.microsoft.com/download).

```bash
dotnet build managed/Overseer.sln
dotnet run --project managed/tools/Overseer.DevHost
```

The dev host prints the generated RCON password and writes it to
`devroot/config/overseer.json`. In another terminal:

```bash
dotnet run --project managed/tools/Overseer.RconCli -- --password <password> overseer.version
```

Building the native side requires Visual Studio 2022 (Desktop C++) and CMake:

```bash
cmake -S native -B build/native -A x64
cmake --build build/native --config Release
```

`scripts/package.ps1` builds everything and lays it out as it would be
installed on a server. See [docs/installing.md](docs/installing.md).

## Writing a plugin

```csharp
using Overseer.Api;

[PluginInfo("Hello", "1.0.0", "you")]
public sealed class HelloPlugin : OverseerPlugin
{
    [Command("hello", Description = "Say hello")]
    public string Hello(CommandContext ctx) => $"Hello, {ctx.Caller}!";

    [Command("announce", Description = "Broadcast a message")]
    public Task<string> Announce(CommandContext ctx) =>
        Server.ExecuteAdminCommandAsync($"#Announce {ctx.RawArgs}");
}
```

Full guide: [docs/writing-plugins.md](docs/writing-plugins.md).

## Contributing

Start with [CONTRIBUTING.md](CONTRIBUTING.md). The most valuable help right
now is reverse-engineering work on the engine bridge — see the roadmap.

## License

[MIT](LICENSE). Overseer is not affiliated with or endorsed by Gamepires or
Jagex. "SCUM" is a trademark of its respective owners.
