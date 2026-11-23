using System.Buffers.Binary;
using System.Text;

namespace Overseer.Rcon;

/// <summary>Source RCON packet type values. Note that 2 is reused by the protocol.</summary>
public static class RconPacketType
{
    public const int ResponseValue = 0;
    public const int ExecCommand = 2;
    public const int AuthResponse = 2;
    public const int Auth = 3;
}

/// <summary>A single Source RCON packet.</summary>
public readonly record struct RconPacket(int Id, int Type, string Body);

/// <summary>
/// Encodes and decodes Source RCON packets:
/// <c>int32 size | int32 id | int32 type | body (null-terminated) | 0x00</c>,
/// all little-endian, where size excludes its own 4 bytes.
/// </summary>
public static class RconCodec
{
    /// <summary>Smallest legal size field: id + type + two terminators.</summary>
    public const int MinPacketSize = 10;

    /// <summary>Largest size field we accept from a peer (Source's limit is 4096).</summary>
    public const int MaxPacketSize = 4096;

    /// <summary>Largest body we put in one outgoing packet.</summary>
    public const int MaxBodyBytes = MaxPacketSize - MinPacketSize;

    public static byte[] Encode(RconPacket packet)
    {
        var body = Encoding.UTF8.GetBytes(packet.Body);
        var size = MinPacketSize + body.Length;
        var buffer = new byte[4 + size];
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(0), size);
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(4), packet.Id);
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(8), packet.Type);
        body.CopyTo(buffer, 12);
        // Trailing two zero bytes already present.
        return buffer;
    }

    public static async Task WriteAsync(Stream stream, RconPacket packet, CancellationToken cancellationToken)
    {
        await stream.WriteAsync(Encode(packet), cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    /// <summary>Reads one packet, or returns <c>null</c> if the peer closed the connection cleanly.</summary>
    /// <exception cref="InvalidDataException">The peer sent a malformed packet.</exception>
    public static async Task<RconPacket?> ReadAsync(Stream stream, CancellationToken cancellationToken)
    {
        var header = new byte[4];
        if (!await ReadExactAsync(stream, header, cancellationToken).ConfigureAwait(false))
        {
            return null;
        }

        var size = BinaryPrimitives.ReadInt32LittleEndian(header);
        if (size < MinPacketSize || size > MaxPacketSize)
        {
            throw new InvalidDataException($"RCON packet size {size} out of range.");
        }

        var payload = new byte[size];
        if (!await ReadExactAsync(stream, payload, cancellationToken).ConfigureAwait(false))
        {
            throw new EndOfStreamException("Connection closed mid-packet.");
        }

        return Decode(payload);
    }

    /// <summary>Decodes a packet payload (everything after the size field).</summary>
    public static RconPacket Decode(ReadOnlySpan<byte> payload)
    {
        var id = BinaryPrimitives.ReadInt32LittleEndian(payload);
        var type = BinaryPrimitives.ReadInt32LittleEndian(payload[4..]);
        var body = payload[8..^2];
        // Be lenient: some clients put stray nulls inside the body.
        var nul = body.IndexOf((byte)0);
        if (nul >= 0)
        {
            body = body[..nul];
        }

        return new RconPacket(id, type, Encoding.UTF8.GetString(body));
    }

    /// <summary>
    /// Splits <paramref name="text"/> into chunks whose UTF-8 encoding fits in
    /// one packet, never splitting a surrogate pair.
    /// </summary>
    public static IEnumerable<string> SplitBody(string text, int maxBytes = MaxBodyBytes)
    {
        if (text.Length == 0)
        {
            yield return string.Empty;
            yield break;
        }

        var start = 0;
        var bytes = 0;
        for (var i = 0; i < text.Length; i++)
        {
            int charBytes;
            int width;
            if (char.IsHighSurrogate(text[i]) && i + 1 < text.Length && char.IsLowSurrogate(text[i + 1]))
            {
                charBytes = 4;
                width = 2;
            }
            else
            {
                charBytes = Encoding.UTF8.GetByteCount(text.AsSpan(i, 1));
                width = 1;
            }

            if (bytes + charBytes > maxBytes)
            {
                yield return text[start..i];
                start = i;
                bytes = 0;
            }

            bytes += charBytes;
            i += width - 1;
        }

        yield return text[start..];
    }

    private static async Task<bool> ReadExactAsync(Stream stream, byte[] buffer, CancellationToken cancellationToken)
    {
        var read = 0;
        while (read < buffer.Length)
        {
            var n = await stream.ReadAsync(buffer.AsMemory(read), cancellationToken).ConfigureAwait(false);
            if (n == 0)
            {
                if (read == 0)
                {
                    return false;
                }

                throw new EndOfStreamException("Connection closed mid-packet.");
            }

            read += n;
        }

        return true;
    }
}
