// Overseer dev host: the full managed runtime (config, plugins, RCON) with a
// fake engine that pretends to be SCUM. Lets you build plugins and RCON
// tooling without a game server.
//
//   dotnet run --project managed/tools/Overseer.DevHost [-- <rootDirectory>]

using Overseer.Host;

var root = args.Length > 0 ? args[0] : Path.Combine(Environment.CurrentDirectory, "devroot");
var engine = new FakeEngine();
await using var runtime = new OverseerRuntime(engine, root);
await runtime.StartAsync();

Console.WriteLine();
Console.WriteLine($"Root:          {runtime.RootDirectory}");
Console.WriteLine($"Plugins dir:   {Path.Combine(runtime.RootDirectory, "plugins")}");
if (runtime.RconEndPoint is { } endPoint)
{
    Console.WriteLine($"RCON:          {endPoint}");
    Console.WriteLine($"RCON password: {runtime.Config.Rcon.Password}");
}

Console.WriteLine("Press Ctrl+C to stop.");

var stop = new TaskCompletionSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    stop.TrySetResult();
};
await stop.Task;

/// <summary>Pretends to be SCUM: logs to the console and echoes admin commands.</summary>
internal sealed class FakeEngine : IEngine
{
    public string Name => "fake (dev host)";

    public void Log(LogLevel level, string message) =>
        Console.WriteLine($"{DateTime.Now:HH:mm:ss} [{level,-5}] {message}");

    public Task<string> ExecuteAdminCommandAsync(string command, CancellationToken cancellationToken)
    {
        Log(LogLevel.Info, $"(fake) game executed: {command}");
        return Task.FromResult($"[fake SCUM] executed: {command}");
    }
}
