namespace Overseer.Api;

/// <summary>Identifies a plugin. Required on every <see cref="OverseerPlugin"/>.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class PluginInfoAttribute(string name, string version, string author) : Attribute
{
    public string Name { get; } = name;
    public string Version { get; } = version;
    public string Author { get; } = author;
    public string? Description { get; init; }
}

/// <summary>
/// Marks a plugin method as a command. Supported signatures, where the
/// parameter is optional:
/// <c>string? M(CommandContext)</c>, <c>Task&lt;string?&gt; M(CommandContext)</c>,
/// <c>void M(CommandContext)</c>, <c>Task M(CommandContext)</c>.
/// </summary>
[AttributeUsage(AttributeTargets.Method, AllowMultiple = true)]
public sealed class CommandAttribute(string name) : Attribute
{
    /// <summary>Command name, case-insensitive, no spaces (e.g. "hello" or "shop.buy").</summary>
    public string Name { get; } = name;

    public string? Description { get; init; }

    /// <summary>
    /// Permission required to run the command from in-game chat. RCON callers
    /// are always fully privileged. Reserved until in-game commands land.
    /// </summary>
    public string? Permission { get; init; }
}
