// overseer-rcon: minimal Source RCON client.
//
//   overseer-rcon [--host 127.0.0.1] [--port 27020] [--password <pw>] [command...]
//
// The password can also come from OVERSEER_RCON_PASSWORD. With no command, an
// interactive prompt starts.

using Overseer.Rcon;

var host = "127.0.0.1";
var port = 27020;
var password = Environment.GetEnvironmentVariable("OVERSEER_RCON_PASSWORD");
var commandParts = new List<string>();

for (var i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--host" when i + 1 < args.Length:
            host = args[++i];
            break;
        case "--port" when i + 1 < args.Length:
            port = int.Parse(args[++i]);
            break;
        case "--password" when i + 1 < args.Length:
            password = args[++i];
            break;
        case "-h" or "--help":
            Console.WriteLine("usage: overseer-rcon [--host H] [--port P] [--password PW] [command...]");
            return 0;
        default:
            commandParts.Add(args[i]);
            break;
    }
}

if (string.IsNullOrEmpty(password))
{
    Console.Error.WriteLine("No password: pass --password or set OVERSEER_RCON_PASSWORD.");
    return 2;
}

await using var client = await RconClient.ConnectAsync(host, port);
if (!await client.AuthenticateAsync(password))
{
    Console.Error.WriteLine("Authentication failed.");
    return 1;
}

if (commandParts.Count > 0)
{
    Console.WriteLine(await client.ExecuteAsync(string.Join(' ', commandParts)));
    return 0;
}

Console.WriteLine($"Connected to {host}:{port}. Type 'exit' to quit.");
while (true)
{
    Console.Write("rcon> ");
    var line = Console.ReadLine();
    if (line is null || line.Trim() is "exit" or "quit")
    {
        return 0;
    }

    if (line.Trim().Length > 0)
    {
        Console.WriteLine(await client.ExecuteAsync(line));
    }
}
