using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using BasicRenderer.Scripting.Interop;

namespace BasicRenderer.Scripting.Runtime;

/// <summary>
/// The native host's entry points (BRScriptRuntimeInitializeFn and BRScriptRuntimeCallbacks). Nothing may throw across
/// this boundary: every entry catches, logs and returns a status.
/// </summary>
/// <remarks>
/// Keep this type free of Roslyn types in its signatures and fields: Initialize installs the resolver that lets the
/// default load context find Roslyn beside this assembly, and must run before anything needs it.
/// </remarks>
public static unsafe class ScriptRuntime
{
    private static ScriptSession? s_session;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static int Initialize(void* apiTable, void* callbackTable)
    {
        // void* keeps the ABI mirrors internal; the layouts are checked below.
        var api = (BRScriptApiV1*)apiTable;
        var callbacks = (BRScriptRuntimeCallbacks*)callbackTable;
        try {
            if (api == null || callbacks == null) return AbiConstants.InvalidArgument;
            // The header fields and Log are present in every version, so a mismatch can still be reported.
            bool headerUsable = api->StructSize >= 40 && api->Log != null;
            void Refuse(string message)
            {
                if (headerUsable) {
                    fixed (byte* text = Native.Utf8($"scripting runtime: {message}")) {
                        api->Log(api->Context, (int)NativeLogLevel.Error, text);
                    }
                }
            }
            if (api->Version < 1 || api->StructSize < (uint)sizeof(BRScriptApiV1)) {
                Refuse($"host API table v{api->Version} ({api->StructSize} bytes) is older than this runtime needs (v{AbiConstants.Version}, {sizeof(BRScriptApiV1)} bytes)");
                return AbiConstants.Unsupported;
            }
            if (api->Vec3Size != sizeof(BRVec3) || api->QuatSize != sizeof(BRQuat) || api->TransformSize != sizeof(BRTransform)) {
                Refuse($"value layout mismatch: host vec3/quat/transform {api->Vec3Size}/{api->QuatSize}/{api->TransformSize} bytes, " +
                    $"runtime {sizeof(BRVec3)}/{sizeof(BRQuat)}/{sizeof(BRTransform)}");
                return AbiConstants.Unsupported;
            }
            if (callbacks->StructSize < (uint)sizeof(BRScriptRuntimeCallbacks)) {
                Refuse($"host callback table is {callbacks->StructSize} bytes; this runtime fills {sizeof(BRScriptRuntimeCallbacks)}");
                return AbiConstants.Unsupported;
            }

            Native.Attach(api);
            InstallDependencyResolver();
            s_session = new ScriptSession();

            callbacks->Version = AbiConstants.Version;
            callbacks->RunScenario = &RunScenario;
            callbacks->Tick = &Tick;
            callbacks->Stop = &Stop;
            callbacks->Shutdown = &Shutdown;
            Native.Log(NativeLogLevel.Info, $"scripting runtime ready (.NET {Environment.Version}, ABI v{AbiConstants.Version})");
            return AbiConstants.Ok;
        } catch (Exception e) {
            try {
                Native.Log(NativeLogLevel.Error, $"scripting runtime failed to initialize: {e}");
            } catch {
                // Nothing else can be reported.
            }
            return AbiConstants.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RunScenario(byte* sourcePath, byte* scenarioName, byte* optionsJson)
    {
        try {
            var source = Native.FromUtf8(sourcePath);
            if (string.IsNullOrEmpty(source) || s_session == null) return AbiConstants.InvalidArgument;
            return s_session.Run(source, Native.FromUtf8(scenarioName), Native.FromUtf8(optionsJson));
        } catch (Exception e) {
            Native.Log(NativeLogLevel.Error, $"run_scenario: {e}");
            return AbiConstants.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Tick(ulong hostFrameIndex, double deltaSeconds)
    {
        try {
            s_session?.Tick(hostFrameIndex, deltaSeconds);
        } catch (Exception e) {
            Native.Log(NativeLogLevel.Error, $"tick: {e}");
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Stop()
    {
        try {
            s_session?.Stop("stopped by the host");
        } catch (Exception e) {
            Native.Log(NativeLogLevel.Error, $"stop: {e}");
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Shutdown()
    {
        try {
            s_session?.Stop("the host is shutting down");
        } catch {
            // Shutting down regardless.
        }
        s_session = null;
        Native.Detach();
    }

    private static bool s_resolverInstalled;

    /// <summary>
    /// The host loads this assembly into the default context without a deps.json, so its dependencies (Roslyn) are
    /// resolved from its own directory on demand.
    /// </summary>
    private static void InstallDependencyResolver()
    {
        if (s_resolverInstalled) return;
        s_resolverInstalled = true;
        var directory = Path.GetDirectoryName(typeof(ScriptRuntime).Assembly.Location) ?? AppContext.BaseDirectory;
        AssemblyLoadContext.Default.Resolving += (context, name) =>
        {
            var candidate = Path.Combine(directory, name.Name + ".dll");
            return File.Exists(candidate) ? context.LoadFromAssemblyPath(candidate) : null;
        };
    }
}
