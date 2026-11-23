namespace Overseer.Api;

/// <summary>
/// Base class for all Overseer plugins. Derive from it, add a
/// <see cref="PluginInfoAttribute"/>, and expose commands with
/// <see cref="CommandAttribute"/>.
/// </summary>
public abstract class OverseerPlugin
{
    /// <summary>Access to the game server.</summary>
    public IServer Server { get; internal set; } = null!;

    /// <summary>Logger scoped to this plugin.</summary>
    public IPluginLogger Log { get; internal set; } = null!;

    /// <summary>A writable directory reserved for this plugin's data and config.</summary>
    public string DataDirectory { get; internal set; } = string.Empty;

    /// <summary>Called once after the plugin is constructed and wired up.</summary>
    public virtual void OnLoad() { }

    /// <summary>Called before the plugin is unloaded (reload or shutdown).</summary>
    public virtual void OnUnload() { }
}
