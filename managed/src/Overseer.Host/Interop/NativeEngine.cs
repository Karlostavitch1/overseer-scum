using System.Collections.Concurrent;
using System.Text;
using Overseer.Api;

namespace Overseer.Host.Interop;

/// <summary><see cref="IEngine"/> backed by overseer_native.dll.</summary>
internal sealed class NativeEngine(NativeApi api) : IEngine
{
    private static readonly TimeSpan CommandTimeout = TimeSpan.FromSeconds(30);

    private readonly ConcurrentDictionary<long, TaskCompletionSource<string>> _pending = new();
    private long _nextRequestId;

    public string Name => "native";

    public unsafe void Log(LogLevel level, string message)
    {
        var bytes = Utf8Z(message);
        fixed (byte* p = bytes)
        {
            api.Log((int)level, p);
        }
    }

    public async Task<string> ExecuteAdminCommandAsync(string command, CancellationToken cancellationToken)
    {
        var id = Interlocked.Increment(ref _nextRequestId);
        var completion = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = completion;

        var rc = Submit(id, command);
        if (rc != 0)
        {
            _pending.TryRemove(id, out _);
            throw new ServerCommandException($"The engine rejected the command (code {rc}).");
        }

        try
        {
            return await completion.Task.WaitAsync(CommandTimeout, cancellationToken).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            throw new ServerCommandException($"Timed out after {CommandTimeout.TotalSeconds:0}s waiting for the game to run the command.");
        }
        finally
        {
            _pending.TryRemove(id, out _);
        }
    }

    /// <summary>Called from native code (any thread) when a command finishes.</summary>
    public void Complete(long requestId, int status, string output)
    {
        if (!_pending.TryRemove(requestId, out var completion))
        {
            return;
        }

        if (status == 0)
        {
            completion.TrySetResult(output);
        }
        else
        {
            completion.TrySetException(new ServerCommandException(output));
        }
    }

    private unsafe int Submit(long requestId, string command)
    {
        var bytes = Utf8Z(command);
        fixed (byte* p = bytes)
        {
            return api.SubmitCommand(requestId, p);
        }
    }

    private static byte[] Utf8Z(string s)
    {
        var bytes = new byte[Encoding.UTF8.GetByteCount(s) + 1];
        Encoding.UTF8.GetBytes(s, bytes);
        return bytes;
    }
}
