using System.Buffers.Binary;
using System.Text;

namespace Overseer.Rcon.Tests;

public class RconCodecTests
{
    [Fact]
    public void Encode_ProducesSourceLayout()
    {
        var bytes = RconCodec.Encode(new RconPacket(7, RconPacketType.Auth, "pw"));

        Assert.Equal(4 + 10 + 2, bytes.Length);
        Assert.Equal(12, BinaryPrimitives.ReadInt32LittleEndian(bytes));
        Assert.Equal(7, BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(4)));
        Assert.Equal(3, BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(8)));
        Assert.Equal("pw", Encoding.ASCII.GetString(bytes, 12, 2));
        Assert.Equal(0, bytes[^2]);
        Assert.Equal(0, bytes[^1]);
    }

    [Fact]
    public async Task RoundTrip_PreservesPacket()
    {
        var original = new RconPacket(42, RconPacketType.ExecCommand, "#Announce héllo 🌍");
        using var stream = new MemoryStream(RconCodec.Encode(original));

        var decoded = await RconCodec.ReadAsync(stream, CancellationToken.None);

        Assert.Equal(original, decoded);
    }

    [Fact]
    public async Task Read_ReturnsNullOnCleanEof()
    {
        using var stream = new MemoryStream();
        Assert.Null(await RconCodec.ReadAsync(stream, CancellationToken.None));
    }

    [Theory]
    [InlineData(5)]
    [InlineData(5000)]
    [InlineData(-1)]
    public async Task Read_RejectsOutOfRangeSize(int size)
    {
        var bytes = new byte[4 + 16];
        BinaryPrimitives.WriteInt32LittleEndian(bytes, size);
        using var stream = new MemoryStream(bytes);

        await Assert.ThrowsAsync<InvalidDataException>(() => RconCodec.ReadAsync(stream, CancellationToken.None));
    }

    [Fact]
    public async Task Read_ThrowsOnTruncatedPacket()
    {
        var bytes = RconCodec.Encode(new RconPacket(1, 2, "hello"))[..8];
        using var stream = new MemoryStream(bytes);

        await Assert.ThrowsAsync<EndOfStreamException>(() => RconCodec.ReadAsync(stream, CancellationToken.None));
    }

    [Fact]
    public void SplitBody_RespectsByteLimitAndSurrogates()
    {
        var text = string.Concat(Enumerable.Repeat("a🌍", 3000));

        var chunks = RconCodec.SplitBody(text, maxBytes: 101).ToList();

        Assert.Equal(text, string.Concat(chunks));
        Assert.All(chunks, c => Assert.True(Encoding.UTF8.GetByteCount(c) <= 101));
        Assert.All(chunks, c => Assert.False(char.IsHighSurrogate(c[^1])));
    }

    [Fact]
    public void SplitBody_EmptyYieldsOneEmptyChunk()
    {
        Assert.Equal(new[] { string.Empty }, RconCodec.SplitBody(string.Empty));
    }
}
