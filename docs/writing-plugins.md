# Writing plugins

Plugins are .NET 8 class libraries that reference `Overseer.Api`.

## 1. Create the project

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
    <EnableDynamicLoading>true</EnableDynamicLoading>
  </PropertyGroup>
  <ItemGroup>
    <!-- Until the NuGet package is published, reference the project or a built Overseer.Api.dll. -->
    <Reference Include="Overseer.Api" HintPath="path/to/Overseer.Api.dll" Private="false" />
  </ItemGroup>
</Project>
```

`Private=false` matters: the host provides `Overseer.Api`, and a second copy
next to your plugin would be ignored anyway.

## 2. Write the plugin

```csharp
using Overseer.Api;

[PluginInfo("Greeter", "1.0.0", "you", Description = "Greets people")]
public sealed class GreeterPlugin : OverseerPlugin
{
    public override void OnLoad() => Log.Info("ready");

    [Command("greet", Description = "greet <name>")]
    public string Greet(CommandContext ctx) =>
        ctx.Args.Count == 0 ? "usage: greet <name>" : $"Hello, {ctx.Args[0]}!";

    [Command("greet.all")]
    public Task<string> GreetAll(CommandContext ctx) =>
        Server.ExecuteAdminCommandAsync("#Announce Hello everyone!");
}
```

### What you get
| Member | Purpose |
| --- | --- |
| `Server.ExecuteAdminCommandAsync` | Run any native SCUM admin command |
| `Log` | Plugin-scoped logging into the Overseer log |
| `DataDirectory` | `overseer/data/<PluginName>/`, created for you |
| `OnLoad` / `OnUnload` | Lifecycle (also called on `overseer.reload`) |
| `[Command]` | Expose a method; signatures: `string`, `Task<string>`, `void`, `Task`, with an optional `CommandContext` parameter |

Command names are case-insensitive, must not contain spaces or start with `#`,
and must be unique across plugins — prefix them with your plugin name
(`shop.buy`) to avoid clashes.

## 3. Test without a server

```bash
dotnet build
cp bin/Debug/net8.0/Greeter.dll <repo>/devroot/plugins/
dotnet run --project <repo>/managed/tools/Overseer.DevHost
overseer-rcon --password <pw> greet Bob
```

The dev host's fake engine echoes admin commands, so you can see exactly what
your plugin would send to the game.

## Guidelines
- Don't block: use `async` for anything slow. Commands time out after 30 s.
- Clean up timers, sockets and threads in `OnUnload`, or reload will leak them.
- Validate input — RCON callers are admins, but future in-game commands won't be.
- `Overseer.Api` follows semantic versioning; breaking changes bump the major version.
