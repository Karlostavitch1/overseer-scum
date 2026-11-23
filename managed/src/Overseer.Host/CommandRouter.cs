using System.Collections.Concurrent;
using System.Text;
using Overseer.Api;

namespace Overseer.Host;

/// <summary>A command registered by the runtime or a plugin.</summary>
public sealed record RegisteredCommand(
    string Name,
    string? Description,
    string Owner,
    Func<CommandContext, CancellationToken, Task<string?>> Handler);

/// <summary>
/// Routes a command line to the right handler:
/// lines starting with <c>#</c> go straight to SCUM's admin command system,
/// anything else is looked up in the registry of Overseer/plugin commands.
/// </summary>
public sealed class CommandRouter(IServer server)
{
    private readonly ConcurrentDictionary<string, RegisteredCommand> _commands = new(StringComparer.OrdinalIgnoreCase);

    public IEnumerable<RegisteredCommand> Commands => _commands.Values.OrderBy(c => c.Name, StringComparer.OrdinalIgnoreCase);

    public void Register(RegisteredCommand command)
    {
        if (string.IsNullOrWhiteSpace(command.Name) || command.Name.Any(char.IsWhiteSpace) || command.Name.StartsWith('#'))
        {
            throw new ArgumentException($"Invalid command name '{command.Name}'.", nameof(command));
        }

        if (!_commands.TryAdd(command.Name, command))
        {
            var existing = _commands[command.Name];
            throw new InvalidOperationException($"Command '{command.Name}' is already registered by {existing.Owner}.");
        }
    }

    public void UnregisterOwner(string owner)
    {
        foreach (var (name, command) in _commands)
        {
            if (command.Owner == owner)
            {
                _commands.TryRemove(name, out _);
            }
        }
    }

    public async Task<string> DispatchAsync(string caller, string line, CancellationToken cancellationToken)
    {
        line = line.Trim();
        if (line.Length == 0)
        {
            return string.Empty;
        }

        if (line[0] == '#')
        {
            try
            {
                return await server.ExecuteAdminCommandAsync(line, cancellationToken).ConfigureAwait(false);
            }
            catch (ServerCommandException ex)
            {
                return ex.Message;
            }
        }

        var space = line.IndexOfAny([' ', '\t']);
        var name = space < 0 ? line : line[..space];
        var rawArgs = space < 0 ? string.Empty : line[(space + 1)..].Trim();

        if (!_commands.TryGetValue(name, out var command))
        {
            return $"Unknown command '{name}'. Native SCUM admin commands start with '#'. Try 'overseer.help'.";
        }

        var context = new CommandContext(caller, name, Tokenize(rawArgs), rawArgs);
        return await command.Handler(context, cancellationToken).ConfigureAwait(false) ?? string.Empty;
    }

    /// <summary>Splits on whitespace, honouring double quotes.</summary>
    public static IReadOnlyList<string> Tokenize(string input)
    {
        var args = new List<string>();
        var current = new StringBuilder();
        var inQuotes = false;
        var hasToken = false;

        foreach (var c in input)
        {
            if (c == '"')
            {
                inQuotes = !inQuotes;
                hasToken = true;
            }
            else if (char.IsWhiteSpace(c) && !inQuotes)
            {
                if (hasToken)
                {
                    args.Add(current.ToString());
                    current.Clear();
                    hasToken = false;
                }
            }
            else
            {
                current.Append(c);
                hasToken = true;
            }
        }

        if (hasToken)
        {
            args.Add(current.ToString());
        }

        return args;
    }
}
