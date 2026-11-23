using System.Reflection;
using Overseer.Api;

namespace Overseer.Host.Plugins;

public sealed record LoadedPluginInfo(string Name, string Version, string Author, string File);

/// <summary>Discovers, loads and unloads compiled plugin assemblies.</summary>
/// <remarks>
/// Layout: <c>plugins/MyPlugin.dll</c> or <c>plugins/MyPlugin/MyPlugin.dll</c>
/// (the latter lets a plugin ship its own dependencies).
/// </remarks>
internal sealed class PluginManager(string pluginsDirectory, string dataDirectory, CommandRouter router, IServer server, IEngine engine)
{
    private readonly object _gate = new();
    private readonly List<(PluginLoadContext Context, List<(OverseerPlugin Plugin, LoadedPluginInfo Info)> Plugins)> _loaded = [];

    public IReadOnlyList<LoadedPluginInfo> Loaded
    {
        get
        {
            lock (_gate)
            {
                return _loaded.SelectMany(l => l.Plugins.Select(p => p.Info)).ToList();
            }
        }
    }

    public void LoadAll()
    {
        Directory.CreateDirectory(pluginsDirectory);
        var files = Directory.EnumerateFiles(pluginsDirectory, "*.dll", SearchOption.TopDirectoryOnly)
            .Concat(Directory.EnumerateDirectories(pluginsDirectory)
                .Select(dir => Path.Combine(dir, Path.GetFileName(dir) + ".dll"))
                .Where(File.Exists));

        foreach (var file in files)
        {
            try
            {
                LoadFile(file);
            }
            catch (Exception ex)
            {
                engine.Log(LogLevel.Error, $"Failed to load plugin {file}: {ex}");
            }
        }
    }

    public void UnloadAll()
    {
        lock (_gate)
        {
            foreach (var (context, plugins) in _loaded)
            {
                foreach (var (plugin, info) in plugins)
                {
                    try
                    {
                        plugin.OnUnload();
                    }
                    catch (Exception ex)
                    {
                        engine.Log(LogLevel.Error, $"Plugin {info.Name} threw in OnUnload: {ex}");
                    }

                    router.UnregisterOwner(OwnerKey(info));
                    engine.Log(LogLevel.Info, $"Unloaded plugin {info.Name}");
                }

                context.Unload();
            }

            _loaded.Clear();
        }
    }

    private void LoadFile(string file)
    {
        var context = new PluginLoadContext(Path.GetFullPath(file));
        Assembly assembly;
        // Load from a byte copy so the DLL on disk stays replaceable for hot reload.
        using (var stream = new MemoryStream(File.ReadAllBytes(file)))
        {
            assembly = context.LoadFromStream(stream);
        }

        var plugins = new List<(OverseerPlugin, LoadedPluginInfo)>();
        foreach (var type in assembly.GetTypes().Where(t => t is { IsAbstract: false, IsClass: true } && typeof(OverseerPlugin).IsAssignableFrom(t)))
        {
            var attribute = type.GetCustomAttribute<PluginInfoAttribute>();
            if (attribute is null)
            {
                engine.Log(LogLevel.Warn, $"{type.FullName} in {file} has no [PluginInfo]; skipping");
                continue;
            }

            var info = new LoadedPluginInfo(attribute.Name, attribute.Version, attribute.Author, file);
            var plugin = (OverseerPlugin)Activator.CreateInstance(type)!;
            plugin.Server = server;
            plugin.Log = new PluginLogger(engine, attribute.Name);
            plugin.DataDirectory = Path.Combine(dataDirectory, attribute.Name);
            Directory.CreateDirectory(plugin.DataDirectory);

            try
            {
                RegisterCommands(plugin, info);
                plugin.OnLoad();
            }
            catch (Exception ex)
            {
                router.UnregisterOwner(OwnerKey(info));
                engine.Log(LogLevel.Error, $"Plugin {info.Name} failed to load: {ex}");
                continue;
            }

            plugins.Add((plugin, info));
            engine.Log(LogLevel.Info, $"Loaded plugin {info.Name} {info.Version} by {info.Author}");
        }

        if (plugins.Count == 0)
        {
            context.Unload();
            return;
        }

        lock (_gate)
        {
            _loaded.Add((context, plugins));
        }
    }

    private void RegisterCommands(OverseerPlugin plugin, LoadedPluginInfo info)
    {
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        foreach (var method in plugin.GetType().GetMethods(flags))
        {
            foreach (var command in method.GetCustomAttributes<CommandAttribute>())
            {
                router.Register(new RegisteredCommand(
                    command.Name,
                    command.Description,
                    OwnerKey(info),
                    CommandInvoker.Create(plugin, method)));
            }
        }
    }

    private static string OwnerKey(LoadedPluginInfo info) => $"plugin:{info.Name}";
}

internal sealed class PluginLogger(IEngine engine, string pluginName) : IPluginLogger
{
    public void Debug(string message) => engine.Log(LogLevel.Debug, $"[{pluginName}] {message}");

    public void Info(string message) => engine.Log(LogLevel.Info, $"[{pluginName}] {message}");

    public void Warn(string message) => engine.Log(LogLevel.Warn, $"[{pluginName}] {message}");

    public void Error(string message, Exception? exception = null) =>
        engine.Log(LogLevel.Error, exception is null ? $"[{pluginName}] {message}" : $"[{pluginName}] {message}: {exception}");
}
