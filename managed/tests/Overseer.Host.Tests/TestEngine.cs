using System.Collections.Concurrent;
using Overseer.Api;

namespace Overseer.Host.Tests;

internal sealed class TestEngine : IEngine
{
    public ConcurrentQueue<string> Executed { get; } = new();

    public ConcurrentQueue<string> Logs { get; } = new();

    public bool Fail { get; set; }

    public string Name => "test";

    public void Log(LogLevel level, string message) => Logs.Enqueue($"{level}: {message}");

    public Task<string> ExecuteAdminCommandAsync(string command, CancellationToken cancellationToken)
    {
        Executed.Enqueue(command);
        return Fail
            ? Task.FromException<string>(new ServerCommandException($"failed: {command}"))
            : Task.FromResult($"ok: {command}");
    }
}

internal sealed class TestServer(IEngine engine) : IServer
{
    public Task<string> ExecuteAdminCommandAsync(string command, CancellationToken cancellationToken = default) =>
        engine.ExecuteAdminCommandAsync(command, cancellationToken);
}
