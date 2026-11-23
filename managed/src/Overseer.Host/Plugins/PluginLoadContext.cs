using System.Reflection;
using System.Runtime.Loader;
using Overseer.Api;

namespace Overseer.Host.Plugins;

/// <summary>
/// A collectible load context per plugin, so plugins can be unloaded and
/// reloaded, and can carry their own dependency versions.
/// </summary>
internal sealed class PluginLoadContext(string pluginPath)
    : AssemblyLoadContext(name: Path.GetFileNameWithoutExtension(pluginPath), isCollectible: true)
{
    private static readonly Assembly ApiAssembly = typeof(OverseerPlugin).Assembly;
    private readonly AssemblyDependencyResolver _resolver = new(pluginPath);

    protected override Assembly? Load(AssemblyName assemblyName)
    {
        // Overseer.Api must be shared with the host so plugin types derive
        // from the *same* OverseerPlugin. Return the instance the host uses —
        // not the Default context's — because when hosted via hostfxr the host
        // itself lives in an isolated load context.
        if (string.Equals(assemblyName.Name, ApiAssembly.GetName().Name, StringComparison.Ordinal))
        {
            return ApiAssembly;
        }

        var path = _resolver.ResolveAssemblyToPath(assemblyName);
        return path is null ? null : LoadFromAssemblyPath(path);
    }

    protected override IntPtr LoadUnmanagedDll(string unmanagedDllName)
    {
        var path = _resolver.ResolveUnmanagedDllToPath(unmanagedDllName);
        return path is null ? IntPtr.Zero : LoadUnmanagedDllFromPath(path);
    }
}
