using System.Net;
using Overseer.Api;
using Overseer.Host.Plugins;
using Overseer.Rcon;

namespace Overseer.Host;

/// <summary>
/// The Overseer runtime: config, command routing, plugins and the RCON server.
/// Independent of how it is hosted (inside SCUM via native interop, or in the
/// dev host with a fake engine).
/// </summary>
/// <remarks>
/// Directory layout under <c>rootDirectory</c>:
/// <c>config/overseer.json</c>, <c>plugins/</c>, <c>data/&lt;plugin&gt;/</c>, <c>logs/</c>.
/// </remarks>
public sealed class OverseerRuntime : IAsyncDisposable
{
    public static readonly string Version =
        typeof(OverseerRuntime).Assembly.GetName().Version?.ToString(3) ?? "0.0.0";

    private readonly IEngine _engine;
    private readonly PluginManager _plugins;
    private RconServer? _rcon;

    public OverseerRuntime(IEngine engine, string rootDirectory)
    {
        _engine = engine;
        RootDirectory = Path.GetFullPath(rootDirectory);
        Server = new EngineServer(engine);
        Commands = new CommandRouter(Server);
        _plugins = new PluginManager(
            Path.Combine(RootDirectory, "plugins"),
            Path.Combine(RootDirectory, "data"),
            Commands,
            Server,
            engine);
    }

    public string RootDirectory { get; }

    public string ConfigPath => Path.Combine(RootDirectory, "config", "overseer.json");

    public IServer Server { get; }

    public CommandRouter Commands { get; }

    public OverseerConfig Config { get; private set; } = new();

    /// <summary>The RCON endpoint, once started.</summary>
    public IPEndPoint? RconEndPoint => _rcon?.LocalEndPoint;

    public IReadOnlyList<LoadedPluginInfo> Plugins => _plugins.Loaded;

    public Task StartAsync()
    {
        _engine.Log(LogLevel.Info, $"Overseer {Version} starting (engine: {_engine.Name})");
        Config = ConfigStore.LoadOrCreate(ConfigPath, m => _engine.Log(LogLevel.Info, m));

        RegisterBuiltInCommands();
        _plugins.LoadAll();

        if (Config.Rcon.Enabled)
        {
            _rcon = new RconServer(
                new RconServerOptions
                {
                    BindAddress = IPAddress.Parse(Config.Rcon.BindAddress),
                    Port = Config.Rcon.Port,
                    Password = Config.Rcon.Password,
                    MaxConnections = Config.Rcon.MaxConnections,
                },
                (session, command, ct) => Commands.DispatchAsync(session.Caller, command, ct),
                m => _engine.Log(LogLevel.Info, m));
            _rcon.Start();
        }
        else
        {
            _engine.Log(LogLevel.Info, "RCON disabled in config");
        }

        _engine.Log(LogLevel.Info, "Overseer started");
        return Task.CompletedTask;
    }

    public async ValueTask DisposeAsync()
    {
        if (_rcon is not null)
        {
            await _rcon.DisposeAsync().ConfigureAwait(false);
        }

        _plugins.UnloadAll();
    }

    private void RegisterBuiltInCommands()
    {
        const string owner = "overseer";
        Commands.UnregisterOwner(owner);

        Commands.Register(new RegisteredCommand("overseer.help", "List available commands", owner, (_, _) =>
            Task.FromResult<string?>(string.Join('\n', Commands.Commands.Select(c =>
                $"{c.Name,-24} {c.Description ?? string.Empty}  ({c.Owner})"))
                + "\nNative SCUM admin commands (starting with '#') are passed through to the game.")));

        Commands.Register(new RegisteredCommand("overseer.version", "Show Overseer version", owner, (_, _) =>
            Task.FromResult<string?>($"Overseer {Version} (engine: {_engine.Name})")));

        Commands.Register(new RegisteredCommand("overseer.plugins", "List loaded plugins", owner, (_, _) =>
        {
            var plugins = _plugins.Loaded;
            return Task.FromResult<string?>(plugins.Count == 0
                ? "No plugins loaded."
                : string.Join('\n', plugins.Select(p => $"{p.Name} {p.Version} by {p.Author}")));
        }));

        Commands.Register(new RegisteredCommand("overseer.reload", "Unload and reload all plugins", owner, (_, _) =>
        {
            _plugins.UnloadAll();
            _plugins.LoadAll();
            return Task.FromResult<string?>($"Reloaded; {_plugins.Loaded.Count} plugin(s) loaded.");
        }));
    }

    private sealed class EngineServer(IEngine engine) : IServer
    {
        public Task<string> ExecuteAdminCommandAsync(string command, CancellationToken cancellationToken = default)
        {
            if (string.IsNullOrWhiteSpace(command))
            {
                throw new ArgumentException("Command must not be empty.", nameof(command));
            }

            if (command.AsSpan().IndexOfAny('\r', '\n') >= 0)
            {
                throw new ArgumentException("Command must be a single line.", nameof(command));
            }

            return engine.ExecuteAdminCommandAsync(command, cancellationToken);
        }
    }
}
