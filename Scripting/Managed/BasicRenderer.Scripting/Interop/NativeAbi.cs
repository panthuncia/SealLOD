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

internal static class EntityStates
{
    public const int Pending = 0;
    public const int Ready = 1;
    public const int Failed = 2;
}

internal static class SettingTypes
{
    public const int Bool = 1;
    public const int Int32 = 2;
    public const int UInt32 = 3;
    public const int Float = 4;
    public const int UInt8 = 5;
}

internal enum NativeLogLevel
{
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
}

// BRVec3, BRQuat and BRTransform are the public Vec3, Quat and Transform (Api/Math.cs), which share their layouts.

[StructLayout(LayoutKind.Sequential)]
internal struct BRLightDesc
{
    public int Type;
    public int CastsShadows;
    public Vec3 Color;
    public float Intensity;
    public float Range;
    public float SourceRadius;
    public float InnerConeRadians;
    public float OuterConeRadians;
}

[StructLayout(LayoutKind.Sequential)]
internal struct BRCameraProjection
{
    public float FovYRadians;
    public float Aspect;
    public float ZNear;
    public float ZFar;
}

[StructLayout(LayoutKind.Sequential)]
internal struct BRSceneStatus
{
    public ulong Frame;
    public ulong PendingWork;
    public ulong WorkloadSignature;
    public uint PendingShaderCompiles;
    public uint PendingModels;
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

    public delegate* unmanaged[Cdecl]<void*, byte*, byte*, Transform*, ulong*, int> ModelInstantiate;
    public delegate* unmanaged[Cdecl]<void*, ulong, int*, int> EntityGetState;
    public delegate* unmanaged[Cdecl]<void*, ulong, int> EntityDestroy;
    public delegate* unmanaged[Cdecl]<void*, ulong, Transform*, int> EntitySetTransform;
    public delegate* unmanaged[Cdecl]<void*, ulong, Transform*, int> EntityGetTransform;
    public delegate* unmanaged[Cdecl]<void*, ulong, ulong, int> EntitySetParent;
    public delegate* unmanaged[Cdecl]<void*, ulong, float*, int> EntityGetWorldMatrix;

    public delegate* unmanaged[Cdecl]<void*, ulong*, int> CameraGet;
    public delegate* unmanaged[Cdecl]<void*, float, float, float, int> CameraSetProjection;
    public delegate* unmanaged[Cdecl]<void*, BRCameraProjection*, int> CameraGetProjection;
    public delegate* unmanaged[Cdecl]<void*, int, int> CameraSetInputEnabled;

    public delegate* unmanaged[Cdecl]<void*, BRLightDesc*, Transform*, byte*, ulong*, int> LightCreate;
    public delegate* unmanaged[Cdecl]<void*, ulong, BRLightDesc*, int> LightSet;
    public delegate* unmanaged[Cdecl]<void*, ulong, BRLightDesc*, int> LightGet;

    public delegate* unmanaged[Cdecl]<void*, byte*, int> SceneSetEnvironment;
    public delegate* unmanaged[Cdecl]<void*, BRSceneStatus*, int> SceneGetStatus;

    public delegate* unmanaged[Cdecl]<void*, byte*, int, void*, int> SettingGet;
    public delegate* unmanaged[Cdecl]<void*, byte*, int, void*, int> SettingSet;

    public delegate* unmanaged[Cdecl]<void*, byte*, byte*> CommandInvoke;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct BRScriptRuntimeCallbacks
{
    public uint StructSize;
    public uint Version;

    // --- version 1 ---
    public delegate* unmanaged[Cdecl]<byte*, byte*, byte*, int> RunScenario;
    public delegate* unmanaged[Cdecl]<ulong, double, void> Tick;
    public delegate* unmanaged[Cdecl]<byte*, void> Stop;
    public delegate* unmanaged[Cdecl]<void> Shutdown;
}
