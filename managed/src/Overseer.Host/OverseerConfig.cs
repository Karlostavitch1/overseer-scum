using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Overseer.Host;

public sealed class OverseerConfig
{
    public RconSettings Rcon { get; set; } = new();
}

public sealed class RconSettings
{
    public bool Enabled { get; set; } = true;

    /// <summary>Defaults to loopback. Only expose RCON behind a firewall/VPN — it is unencrypted.</summary>
    public string BindAddress { get; set; } = "127.0.0.1";

    public int Port { get; set; } = 27015;

    /// <summary>Generated on first run if empty.</summary>
    public string Password { get; set; } = string.Empty;

    public int MaxConnections { get; set; } = 8;
}

internal static class ConfigStore
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
    };

    /// <summary>Loads the config, creating it (with a random RCON password) if needed.</summary>
    public static OverseerConfig LoadOrCreate(string path, Action<string> log)
    {
        OverseerConfig config;
        if (File.Exists(path))
        {
            config = JsonSerializer.Deserialize<OverseerConfig>(File.ReadAllText(path), JsonOptions) ?? new OverseerConfig();
        }
        else
        {
            config = new OverseerConfig();
            log($"Creating default config at {path}");
        }

        if (string.IsNullOrWhiteSpace(config.Rcon.Password))
        {
            config.Rcon.Password = GeneratePassword();
            log($"Generated a random RCON password; see 'rcon.password' in {path}");
            Save(path, config);
        }
        else if (!File.Exists(path))
        {
            Save(path, config);
        }

        return config;
    }

    public static void Save(string path, OverseerConfig config)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, JsonSerializer.Serialize(config, JsonOptions));
    }

    private static string GeneratePassword()
    {
        const string alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
        return RandomNumberGenerator.GetString(alphabet, 24);
    }
}
