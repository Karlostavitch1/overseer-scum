using System.Runtime.InteropServices;

namespace Overseer.Host.Interop;

/// <summary>
/// Entry points called by overseer_native.dll through hostfxr
/// (<c>Overseer.Host.Interop.NativeEntry, Overseer.Host</c>).
/// </summary>
public static class NativeEntry
{
    private static NativeEngine? s_engine;
    private static OverseerRuntime? s_runtime;

    /// <returns>0 on success, 1 on unexpected failure, 2 on ABI mismatch.</returns>
    [UnmanagedCallersOnly]
    public static unsafe int Initialize(NativeApi* native, ManagedApi* managed)
    {
        try
        {
            if (native == null || managed == null ||
                native->Version != Abi.Version || native->Size < (uint)sizeof(NativeApi) ||
                managed->Size < (uint)sizeof(ManagedApi))
            {
                return 2;
            }

            if (s_runtime is not null)
            {
                return 0;
            }

            var engine = new NativeEngine(*native);
            s_engine = engine;
            managed->OnCommandResult = &OnCommandResult;

            var root = Marshal.PtrToStringUTF8((nint)native->RootDirectory)
                ?? throw new InvalidOperationException("Native root directory was null.");
            var runtime = new OverseerRuntime(engine, root);
            s_runtime = runtime;

            StartInBackground(runtime, engine);
            return 0;
        }
        catch
        {
            return 1;
        }
    }

    // Never block the native bootstrap thread on start-up work.
    private static void StartInBackground(OverseerRuntime runtime, IEngine engine) =>
        _ = Task.Run(async () =>
        {
            try
            {
                await runtime.StartAsync().ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                engine.Log(LogLevel.Error, $"Overseer failed to start: {ex}");
            }
        });

    [UnmanagedCallersOnly]
    private static unsafe void OnCommandResult(long requestId, int status, byte* output)
    {
        try
        {
            s_engine?.Complete(requestId, status, Marshal.PtrToStringUTF8((nint)output) ?? string.Empty);
        }
        catch
        {
            // Exceptions must never cross back into native code.
        }
    }
}
