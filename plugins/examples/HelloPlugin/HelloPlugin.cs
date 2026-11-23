using Overseer.Api;

namespace Overseer.Examples;

[PluginInfo("Hello", "1.0.0", "The Overseer Contributors", Description = "Minimal example plugin")]
public sealed class HelloPlugin : OverseerPlugin
{
    private int _greetings;

    public override void OnLoad() => Log.Info($"Hello plugin loaded; data dir is {DataDirectory}");

    public override void OnUnload() => Log.Info($"Said hello {_greetings} time(s). Goodbye!");

    [Command("hello", Description = "Say hello")]
    public string Hello(CommandContext ctx)
    {
        _greetings++;
        var who = ctx.Args.Count > 0 ? ctx.Args[0] : ctx.Caller;
        return $"Hello, {who}!";
    }

    [Command("hello.announce", Description = "Broadcast a server-wide message: hello.announce <text>")]
    public async Task<string> Announce(CommandContext ctx)
    {
        if (ctx.RawArgs.Length == 0)
        {
            return "usage: hello.announce <text>";
        }

        // Any native SCUM admin command can be issued this way.
        return await Server.ExecuteAdminCommandAsync($"#Announce {ctx.RawArgs}");
    }
}
