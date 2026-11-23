namespace Overseer.Api;

/// <summary>Access to the running SCUM server.</summary>
public interface IServer
{
    /// <summary>
    /// Runs a native SCUM admin command (e.g. <c>#Announce Hello</c>) on the
    /// game thread and returns its output.
    /// </summary>
    /// <exception cref="ServerCommandException">The command failed or is unsupported.</exception>
    Task<string> ExecuteAdminCommandAsync(string command, CancellationToken cancellationToken = default);
}

/// <summary>Logger scoped to a plugin.</summary>
public interface IPluginLogger
{
    void Debug(string message);
    void Info(string message);
    void Warn(string message);
    void Error(string message, Exception? exception = null);
}

/// <summary>Context passed to a command handler.</summary>
/// <param name="Caller">Who invoked the command, e.g. <c>rcon:127.0.0.1:51234</c>.</param>
/// <param name="Name">The command name as invoked.</param>
/// <param name="Args">Whitespace-separated arguments.</param>
/// <param name="RawArgs">Everything after the command name, untouched.</param>
public sealed record CommandContext(string Caller, string Name, IReadOnlyList<string> Args, string RawArgs);

/// <summary>Raised when the game rejects or cannot run an admin command.</summary>
public sealed class ServerCommandException(string message) : Exception(message);
