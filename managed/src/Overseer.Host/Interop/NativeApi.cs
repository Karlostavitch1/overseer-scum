using System.Runtime.InteropServices;

namespace Overseer.Host.Interop;

// Managed mirror of native/include/overseer/native_api.h. Keep in sync and
// bump AbiVersion on any layout change.

internal static class Abi
{
    public const uint Version = 1;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct NativeApi
{
    public uint Size;
    public uint Version;
    public byte* RootDirectory;
    public delegate* unmanaged<int, byte*, void> Log;
    public delegate* unmanaged<long, byte*, int> SubmitCommand;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct ManagedApi
{
    public uint Size;
    public uint Version;
    public delegate* unmanaged<long, int, byte*, void> OnCommandResult;
}
