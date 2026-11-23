using Overseer.Api;
using Overseer.Host.Plugins;

namespace Overseer.Host.Tests;

public class CommandRouterTests
{
    private readonly TestEngine _engine = new();
    private readonly CommandRouter _router;

    public CommandRouterTests() => _router = new CommandRouter(new TestServer(_engine));

    [Fact]
    public async Task HashCommandsPassThroughToEngine()
    {
        var result = await _router.DispatchAsync("test", "  #Announce hi  ", CancellationToken.None);

        Assert.Equal("ok: #Announce hi", result);
        Assert.Equal(new[] { "#Announce hi" }, _engine.Executed);
    }

    [Fact]
    public async Task EngineFailureBecomesMessage()
    {
        _engine.Fail = true;
        Assert.Equal("failed: #Bad", await _router.DispatchAsync("test", "#Bad", CancellationToken.None));
    }

    [Fact]
    public async Task RegisteredCommandReceivesContext()
    {
        CommandContext? seen = null;
        _router.Register(new RegisteredCommand("greet", null, "test", (ctx, _) =>
        {
            seen = ctx;
            return Task.FromResult<string?>("hi");
        }));

        var result = await _router.DispatchAsync("rcon:1", "GREET bob \"big dave\"", CancellationToken.None);

        Assert.Equal("hi", result);
        Assert.NotNull(seen);
        Assert.Equal("rcon:1", seen.Caller);
        Assert.Equal(new[] { "bob", "big dave" }, seen.Args);
        Assert.Equal("bob \"big dave\"", seen.RawArgs);
    }

    [Fact]
    public async Task UnknownCommandIsReported()
    {
        var result = await _router.DispatchAsync("test", "nope", CancellationToken.None);
        Assert.StartsWith("Unknown command 'nope'", result);
    }

    [Fact]
    public void DuplicateRegistrationThrows()
    {
        _router.Register(new RegisteredCommand("x", null, "a", (_, _) => Task.FromResult<string?>(null)));
        Assert.Throws<InvalidOperationException>(() =>
            _router.Register(new RegisteredCommand("X", null, "b", (_, _) => Task.FromResult<string?>(null))));
    }

    [Theory]
    [InlineData("")]
    [InlineData("has space")]
    [InlineData("#hash")]
    public void InvalidNamesRejected(string name)
    {
        Assert.Throws<ArgumentException>(() =>
            _router.Register(new RegisteredCommand(name, null, "a", (_, _) => Task.FromResult<string?>(null))));
    }

    [Fact]
    public async Task UnregisterOwnerRemovesOnlyThatOwner()
    {
        _router.Register(new RegisteredCommand("a", null, "one", (_, _) => Task.FromResult<string?>("a")));
        _router.Register(new RegisteredCommand("b", null, "two", (_, _) => Task.FromResult<string?>("b")));

        _router.UnregisterOwner("one");

        Assert.StartsWith("Unknown", await _router.DispatchAsync("t", "a", CancellationToken.None));
        Assert.Equal("b", await _router.DispatchAsync("t", "b", CancellationToken.None));
    }

    [Fact]
    public async Task CommandInvokerSupportsAllSignatures()
    {
        var target = new SignatureSamples();
        var ctx = new CommandContext("t", "n", [], "");

        Assert.Equal("sync", await Invoke(nameof(SignatureSamples.Sync)));
        Assert.Equal("async", await Invoke(nameof(SignatureSamples.Async)));
        Assert.Null(await Invoke(nameof(SignatureSamples.NoResult)));
        Assert.Null(await Invoke(nameof(SignatureSamples.NoResultAsync)));
        Assert.Equal("noargs", await Invoke(nameof(SignatureSamples.NoArgs)));
        await Assert.ThrowsAsync<InvalidOperationException>(() => Invoke(nameof(SignatureSamples.Throws)));
        Assert.Throws<InvalidOperationException>(() =>
            CommandInvoker.Create(target, typeof(SignatureSamples).GetMethod(nameof(SignatureSamples.BadReturn))!));

        Task<string?> Invoke(string name) =>
            CommandInvoker.Create(target, typeof(SignatureSamples).GetMethod(name)!)(ctx, CancellationToken.None);
    }

    private sealed class SignatureSamples
    {
        public string Sync(CommandContext _) => "sync";

        public Task<string> Async(CommandContext _) => Task.FromResult("async");

        public void NoResult(CommandContext _)
        {
        }

        public Task NoResultAsync(CommandContext _) => Task.CompletedTask;

        public string NoArgs() => "noargs";

        public string Throws(CommandContext _) => throw new InvalidOperationException("boom");

        public int BadReturn(CommandContext _) => 1;
    }
}
