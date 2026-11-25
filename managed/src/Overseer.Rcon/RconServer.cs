using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;

namespace Overseer.Rcon;

public sealed class RconServerOptions
{
    public IPAddress BindAddress { get; init; } = IPAddress.Loopback;

    /// <summary>TCP port (default 27020; SCUM itself uses 7777-7779 and query port 27015). Use 0 to pick a free port.</summary>
    public int Port { get; init; } = 27020;

    /// <summary>Required. The server refuses to start with an empty password.</summary>
    public required string Password { get; init; }

    public int MaxConnections { get; init; } = 8;

    /// <summary>Failed auth attempts allowed per connection before it is dropped.</summary>
    public int MaxAuthFailures { get; init; } = 3;

    public TimeSpan IdleTimeout { get; init; } = TimeSpan.FromMinutes(10);
}

/// <summary>Information about a connected RCON client.</summary>
public sealed record RconSession(Guid Id, EndPoint? RemoteEndPoint)
{
    public string Caller => $"rcon:{RemoteEndPoint}";
}

/// <summary>Handles an authenticated command and returns the text response.</summary>
public delegate Task<string> RconCommandHandler(RconSession session, string command, CancellationToken cancellationToken);

/// <summary>
/// A Source RCON server. Commands on a single connection are processed in
/// order; connections are independent.
/// </summary>
public sealed class RconServer : IAsyncDisposable
{
    private readonly RconServerOptions _options;
    private readonly RconCommandHandler _handler;
    private readonly Action<string> _log;
    private readonly byte[] _passwordBytes;
    private readonly CancellationTokenSource _shutdown = new();
    private TcpListener? _listener;
    private Task? _acceptLoop;
    private int _connections;

    public RconServer(RconServerOptions options, RconCommandHandler handler, Action<string>? log = null)
    {
        if (string.IsNullOrEmpty(options.Password))
        {
            throw new ArgumentException("An RCON password is required.", nameof(options));
        }

        _options = options;
        _handler = handler;
        _log = log ?? (_ => { });
        _passwordBytes = Encoding.UTF8.GetBytes(options.Password);
    }

    /// <summary>The bound endpoint, available after <see cref="Start"/>.</summary>
    public IPEndPoint? LocalEndPoint => _listener?.LocalEndpoint as IPEndPoint;

    public void Start()
    {
        if (_listener is not null)
        {
            throw new InvalidOperationException("Already started.");
        }

        _listener = new TcpListener(_options.BindAddress, _options.Port);
        _listener.Start();
        _log($"RCON listening on {LocalEndPoint}");
        _acceptLoop = AcceptLoopAsync(_shutdown.Token);
    }

    public async ValueTask DisposeAsync()
    {
        _shutdown.Cancel();
        _listener?.Stop();
        if (_acceptLoop is not null)
        {
            try
            {
                await _acceptLoop.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
        }

        // _shutdown is intentionally not disposed: in-flight connection
        // handlers may still observe its (cancelled) token.
    }

    private async Task AcceptLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            TcpClient client;
            try
            {
                client = await _listener!.AcceptTcpClientAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (Exception ex) when (ex is OperationCanceledException or ObjectDisposedException or SocketException)
            {
                return;
            }

            if (Interlocked.Increment(ref _connections) > _options.MaxConnections)
            {
                Interlocked.Decrement(ref _connections);
                _log($"RCON connection from {client.Client.RemoteEndPoint} rejected: too many connections");
                client.Dispose();
                continue;
            }

            _ = Task.Run(async () =>
            {
                try
                {
                    await HandleClientAsync(client, cancellationToken).ConfigureAwait(false);
                }
                finally
                {
                    Interlocked.Decrement(ref _connections);
                }
            }, CancellationToken.None);
        }
    }

    private async Task HandleClientAsync(TcpClient client, CancellationToken cancellationToken)
    {
        using var _ = client;
        var session = new RconSession(Guid.NewGuid(), client.Client.RemoteEndPoint);
        var stream = client.GetStream();
        var authenticated = false;
        var failures = 0;

        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                RconPacket? received;
                using (var idle = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
                {
                    idle.CancelAfter(_options.IdleTimeout);
                    received = await RconCodec.ReadAsync(stream, idle.Token).ConfigureAwait(false);
                }

                if (received is not { } packet)
                {
                    return;
                }

                if (packet.Type == RconPacketType.Auth)
                {
                    if (CheckPassword(packet.Body))
                    {
                        authenticated = true;
                        // Source servers send an empty RESPONSE_VALUE before the auth response.
                        await RconCodec.WriteAsync(stream, new RconPacket(packet.Id, RconPacketType.ResponseValue, ""), cancellationToken).ConfigureAwait(false);
                        await RconCodec.WriteAsync(stream, new RconPacket(packet.Id, RconPacketType.AuthResponse, ""), cancellationToken).ConfigureAwait(false);
                        _log($"RCON {session.Caller} authenticated");
                    }
                    else
                    {
                        failures++;
                        await RconCodec.WriteAsync(stream, new RconPacket(-1, RconPacketType.AuthResponse, ""), cancellationToken).ConfigureAwait(false);
                        _log($"RCON {session.Caller} failed authentication ({failures}/{_options.MaxAuthFailures})");
                        if (failures >= _options.MaxAuthFailures)
                        {
                            return;
                        }
                    }

                    continue;
                }

                if (!authenticated)
                {
                    _log($"RCON {session.Caller} sent a command before authenticating; closing");
                    return;
                }

                if (packet.Type == RconPacketType.ResponseValue)
                {
                    // Multi-packet terminator trick: clients send an empty
                    // RESPONSE_VALUE after a command; echo it so they know the
                    // previous response is complete.
                    await RconCodec.WriteAsync(stream, new RconPacket(packet.Id, RconPacketType.ResponseValue, ""), cancellationToken).ConfigureAwait(false);
                    continue;
                }

                if (packet.Type != RconPacketType.ExecCommand)
                {
                    continue;
                }

                string response;
                try
                {
                    response = await _handler(session, packet.Body, cancellationToken).ConfigureAwait(false);
                }
                catch (Exception ex) when (ex is not OperationCanceledException)
                {
                    _log($"RCON command '{packet.Body}' from {session.Caller} threw: {ex}");
                    response = $"error: {ex.Message}";
                }

                foreach (var chunk in RconCodec.SplitBody(response))
                {
                    await RconCodec.WriteAsync(stream, new RconPacket(packet.Id, RconPacketType.ResponseValue, chunk), cancellationToken).ConfigureAwait(false);
                }
            }
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or EndOfStreamException or OperationCanceledException or SocketException)
        {
            // Disconnect, malformed input, idle timeout or shutdown — just drop the connection.
        }
    }

    private bool CheckPassword(string candidate)
    {
        var bytes = Encoding.UTF8.GetBytes(candidate);
        return CryptographicOperations.FixedTimeEquals(bytes, _passwordBytes);
    }
}
