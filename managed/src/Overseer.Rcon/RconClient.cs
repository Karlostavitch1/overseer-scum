using System.Net.Sockets;
using System.Text;

namespace Overseer.Rcon;

/// <summary>Minimal Source RCON client, used by the CLI and the tests.</summary>
public sealed class RconClient : IAsyncDisposable
{
    private readonly TcpClient _tcp;
    private readonly NetworkStream _stream;
    private int _nextId = 1;

    private RconClient(TcpClient tcp)
    {
        _tcp = tcp;
        _stream = tcp.GetStream();
    }

    public static async Task<RconClient> ConnectAsync(string host, int port, CancellationToken cancellationToken = default)
    {
        var tcp = new TcpClient();
        try
        {
            await tcp.ConnectAsync(host, port, cancellationToken).ConfigureAwait(false);
            return new RconClient(tcp);
        }
        catch
        {
            tcp.Dispose();
            throw;
        }
    }

    /// <returns><c>true</c> if the password was accepted.</returns>
    public async Task<bool> AuthenticateAsync(string password, CancellationToken cancellationToken = default)
    {
        var id = _nextId++;
        await RconCodec.WriteAsync(_stream, new RconPacket(id, RconPacketType.Auth, password), cancellationToken).ConfigureAwait(false);
        while (true)
        {
            var packet = await RconCodec.ReadAsync(_stream, cancellationToken).ConfigureAwait(false)
                ?? throw new EndOfStreamException("Server closed the connection during authentication.");
            if (packet.Type == RconPacketType.AuthResponse)
            {
                return packet.Id == id;
            }
        }
    }

    /// <summary>Runs a command and returns the full (possibly multi-packet) response.</summary>
    public async Task<string> ExecuteAsync(string command, CancellationToken cancellationToken = default)
    {
        var id = _nextId++;
        var terminatorId = _nextId++;
        await RconCodec.WriteAsync(_stream, new RconPacket(id, RconPacketType.ExecCommand, command), cancellationToken).ConfigureAwait(false);
        await RconCodec.WriteAsync(_stream, new RconPacket(terminatorId, RconPacketType.ResponseValue, ""), cancellationToken).ConfigureAwait(false);

        var response = new StringBuilder();
        while (true)
        {
            var packet = await RconCodec.ReadAsync(_stream, cancellationToken).ConfigureAwait(false)
                ?? throw new EndOfStreamException("Server closed the connection.");
            if (packet.Id == terminatorId)
            {
                return response.ToString();
            }

            if (packet.Id == id)
            {
                response.Append(packet.Body);
            }
        }
    }

    public ValueTask DisposeAsync()
    {
        _stream.Dispose();
        _tcp.Dispose();
        return ValueTask.CompletedTask;
    }
}
