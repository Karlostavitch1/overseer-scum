using System.Net;

namespace Overseer.Rcon.Tests;

public class RconServerTests
{
    private const string Password = "correct horse battery staple";

    private static async Task<RconServer> StartAsync(RconCommandHandler? handler = null)
    {
        var server = new RconServer(
            new RconServerOptions { BindAddress = IPAddress.Loopback, Port = 0, Password = Password },
            handler ?? ((_, command, _) => Task.FromResult($"echo: {command}")));
        server.Start();
        await Task.Yield();
        return server;
    }

    private static Task<RconClient> ConnectAsync(RconServer server) =>
        RconClient.ConnectAsync("127.0.0.1", server.LocalEndPoint!.Port);

    [Fact]
    public void RequiresPassword()
    {
        Assert.Throws<ArgumentException>(() =>
            new RconServer(new RconServerOptions { Password = "" }, (_, _, _) => Task.FromResult("")));
    }

    [Fact]
    public async Task AuthenticatesAndExecutes()
    {
        await using var server = await StartAsync();
        await using var client = await ConnectAsync(server);

        Assert.True(await client.AuthenticateAsync(Password));
        Assert.Equal("echo: #ListPlayers", await client.ExecuteAsync("#ListPlayers"));
        Assert.Equal("echo: second", await client.ExecuteAsync("second"));
    }

    [Fact]
    public async Task RejectsWrongPassword()
    {
        await using var server = await StartAsync();
        await using var client = await ConnectAsync(server);

        Assert.False(await client.AuthenticateAsync("nope"));
    }

    [Fact]
    public async Task DropsConnectionAfterTooManyFailures()
    {
        await using var server = await StartAsync();
        await using var client = await ConnectAsync(server);

        for (var i = 0; i < 3; i++)
        {
            Assert.False(await client.AuthenticateAsync("nope"));
        }

        await Assert.ThrowsAnyAsync<Exception>(() => client.AuthenticateAsync(Password));
    }

    [Fact]
    public async Task ClosesUnauthenticatedCommand()
    {
        await using var server = await StartAsync();
        await using var client = await ConnectAsync(server);

        await Assert.ThrowsAnyAsync<Exception>(() => client.ExecuteAsync("#ListPlayers"));
    }

    [Fact]
    public async Task SplitsLongResponses()
    {
        var big = new string('x', 20_000);
        await using var server = await StartAsync((_, _, _) => Task.FromResult(big));
        await using var client = await ConnectAsync(server);
        Assert.True(await client.AuthenticateAsync(Password));

        Assert.Equal(big, await client.ExecuteAsync("big"));
    }

    [Fact]
    public async Task HandlerExceptionBecomesErrorResponse()
    {
        await using var server = await StartAsync((_, _, _) => throw new InvalidOperationException("boom"));
        await using var client = await ConnectAsync(server);
        Assert.True(await client.AuthenticateAsync(Password));

        Assert.Equal("error: boom", await client.ExecuteAsync("anything"));
    }
}
