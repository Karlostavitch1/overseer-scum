namespace Overseer.Host;

public enum LogLevel
{
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
}

/// <summary>
/// The runtime's view of the game. In production this is backed by
/// overseer_native.dll's engine bridge; in development and tests by a fake.
/// </summary>
public interface IEngine
{
    string Name { get; }

    void Log(LogLevel level, string message);

    /// <summary>Executes a native SCUM admin command on the game thread.</summary>
    /// <exception cref="Overseer.Api.ServerCommandException">The command failed or is unsupported.</exception>
    Task<string> ExecuteAdminCommandAsync(string command, CancellationToken cancellationToken);
}
