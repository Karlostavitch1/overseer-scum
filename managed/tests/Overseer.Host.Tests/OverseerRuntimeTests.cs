using System.Text.Json;
using Overseer.Rcon;

namespace Overseer.Host.Tests;

public sealed class OverseerRuntimeTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), "overseer-tests", Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        try
        {
            Directory.Delete(_root, recursive: true);
        }
        catch (IOException)
        {
        }
    }

    private void WriteConfig(string password, int port)
    {
        Directory.CreateDirectory(Path.Combine(_root, "config"));
        File.WriteAllText(
            Path.Combine(_root, "config", "overseer.json"),
            JsonSerializer.Serialize(new { rcon = new { enabled = true, bindAddress = "127.0.0.1", port, password } }));
    }

    [Fact]
    public async Task FirstRunGeneratesPasswordAndConfig()
    {
        var engine = new TestEngine();
        await using var runtime = new OverseerRuntime(engine, _root);
        // A config with no password: the runtime must generate and persist one.
        // RCON is disabled so the test doesn't bind the default port.
        Directory.CreateDirectory(Path.Combine(_root, "config"));
        File.WriteAllText(runtime.ConfigPath, """{ "rcon": { "enabled": false } }""");

        await runtime.StartAsync();

        Assert.Equal(24, runtime.Config.Rcon.Password.Length);
        Assert.Contains(runtime.Config.Rcon.Password, File.ReadAllText(runtime.ConfigPath));
        Assert.True(Directory.Exists(Path.Combine(_root, "plugins")));
    }

    [Fact]
    public async Task RconEndToEnd()
    {
        WriteConfig("pw-for-tests", port: 0);
        var engine = new TestEngine();
        await using var runtime = new OverseerRuntime(engine, _root);
        await runtime.StartAsync();

        await using var client = await RconClient.ConnectAsync("127.0.0.1", runtime.RconEndPoint!.Port);
        Assert.True(await client.AuthenticateAsync("pw-for-tests"));

        Assert.StartsWith("Overseer ", await client.ExecuteAsync("overseer.version"));
        Assert.Equal("No plugins loaded.", await client.ExecuteAsync("overseer.plugins"));
        Assert.Equal("ok: #ListPlayers", await client.ExecuteAsync("#ListPlayers"));
        Assert.Contains("overseer.reload", await client.ExecuteAsync("overseer.help"));
    }
}
