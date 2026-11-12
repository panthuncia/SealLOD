// Mirrors of Scripting/include/BasicRendererScripting/ScriptAbi.h. Layouts must match exactly; the host reports its
// sizes in the API table header and ScriptRuntime.Initialize refuses a mismatch. Append-only, like the C header.
using System.Runtime.InteropServices;

namespace BasicRenderer.Scripting.Interop;

internal static class AbiConstants
{
    public const uint Version = 1;

    public const int Ok = 0;
    public const int InvalidArgument = -1;
    public const int StaleHandle = -2;
    public const int NotFound = -3;
    public const int TypeMismatch = -4;
    public const int Unsupported = -5;
    public const int Busy = -6;
    public const int Internal = -100;
}

internal enum NativeLogLevel
{
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
}

[StructLayout(LayoutKind.Sequential)]
internal struct BRVec3
{
    public float X, Y, Z;
}

[StructLayout(LayoutKind.Sequential)]
internal struct BRQuat
{
    public float X, Y, Z, W;
}

[StructLayout(LayoutKind.Sequential)]
internal struct BRTransform
{
    public BRVec3 Position;
    public BRQuat Rotation;
    public BRVec3 Scale;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct BRScriptApiV1
{
    public uint StructSize;
    public uint Version;
    public uint Vec3Size;
    public uint QuatSize;
    public uint TransformSize;
    public uint Reserved0;
    public void* Context;

    // --- version 1 ---
    public delegate* unmanaged[Cdecl]<void*, int, byte*, void> Log;
    public delegate* unmanaged[Cdecl]<void*, byte*, int, int> GetLastError;
    public delegate* unmanaged[Cdecl]<void*, int, byte*, void> ReportState;
    public delegate* unmanaged[Cdecl]<void*, int, void> RequestExit;
    public delegate* unmanaged[Cdecl]<void*, byte*, uint, void*> GetExtension;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct BRScriptRuntimeCallbacks
{
    public uint StructSize;
    public uint Version;

    // --- version 1 ---
    public delegate* unmanaged[Cdecl]<byte*, byte*, byte*, int> RunScenario;
    public delegate* unmanaged[Cdecl]<ulong, double, void> Tick;
    public delegate* unmanaged[Cdecl]<void> Stop;
    public delegate* unmanaged[Cdecl]<void> Shutdown;
}
