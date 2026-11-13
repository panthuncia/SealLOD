using System;
using System.Runtime.InteropServices;
using System.Text;

namespace BasicRenderer.Scripting.Interop;

/// <summary>
/// What the API calls. <see cref="NativeHost"/> forwards to the native table; tests substitute a fake. Every method
/// throws <see cref="ScriptApiException"/> for a failing status.
/// </summary>
internal interface IHostApi
{
    void Log(NativeLogLevel level, string message);
    void ReportState(ScenarioState state, string? resultJson);
    void RequestExit(int exitCode);

    ulong InstantiateModel(string path, string name, in Transform transform);
    /// <summary>EntityStates.*; error is the failure for Failed.</summary>
    int GetEntityState(ulong handle, out string? error);
    void DestroyEntity(ulong handle);
    void SetTransform(ulong handle, in Transform transform);
    Transform GetTransform(ulong handle);
    void SetParent(ulong handle, ulong parent);
    /// <summary>False until the entity has been rendered.</summary>
    bool TryGetWorldMatrix(ulong handle, out System.Numerics.Matrix4x4 world);

    ulong GetCamera();
    void SetCameraProjection(float fovYRadians, float zNear, float zFar);
    BRCameraProjection GetCameraProjection();
    void SetCameraInputEnabled(bool enabled);

    ulong CreateLight(in BRLightDesc desc, in Transform transform, string name);
    void SetLight(ulong handle, in BRLightDesc desc);
    BRLightDesc GetLight(ulong handle);

    void SetEnvironment(string name);
    SceneStatus GetSceneStatus();

    /// <summary>The value as bool, int, uint or float by type (SettingTypes.*).</summary>
    object GetSetting(string name, int type);
    void SetSetting(string name, int type, object value);

    string InvokeCommand(string requestJson);
}

/// <summary>The host the runtime talks to: the native table once attached, nothing before (logging falls back to the console).</summary>
internal static class Native
{
    public static IHostApi? Host { get; set; }

    public static bool IsAttached => Host != null;

    public static IHostApi Api => Host ?? throw new InvalidOperationException("no script host is attached (scenario API used outside a run?)");

    /// <summary>Test hook: receives log lines when no host is attached.</summary>
    public static Action<NativeLogLevel, string>? DetachedLogSink { get; set; }

    public static void Log(NativeLogLevel level, string message)
    {
        if (Host != null) {
            Host.Log(level, message);
            return;
        }
        if (DetachedLogSink != null) DetachedLogSink(level, message);
        else Console.Error.WriteLine($"[script:{level.ToString().ToLowerInvariant()}] {message}");
    }

    public static void ReportState(ScenarioState state, string? resultJson) => Host?.ReportState(state, resultJson);

    public static void RequestExit(int exitCode) => Host?.RequestExit(exitCode);

    /// <summary>NUL-terminated UTF-8, for passing to native code under <c>fixed</c>.</summary>
    public static byte[] Utf8(string text)
    {
        var bytes = new byte[Encoding.UTF8.GetByteCount(text) + 1];
        Encoding.UTF8.GetBytes(text, 0, text.Length, bytes, 0);
        return bytes;
    }

    public static unsafe string? FromUtf8(byte* text) => text == null ? null : Marshal.PtrToStringUTF8((IntPtr)text);
}

/// <summary>The native API table behind <see cref="IHostApi"/>.</summary>
internal sealed unsafe class NativeHost(BRScriptApiV1* api) : IHostApi
{
    private readonly BRScriptApiV1* _api = api;
    private void* Context => _api->Context;

    private string LastError()
    {
        int length = _api->GetLastError(Context, null, 0);
        if (length <= 0) return string.Empty;
        var buffer = new byte[length + 1];
        fixed (byte* p = buffer) {
            _api->GetLastError(Context, p, buffer.Length);
        }
        return Encoding.UTF8.GetString(buffer, 0, length);
    }

    private void Check(int status, string operation)
    {
        if (status >= AbiConstants.Ok) return;
        throw new ScriptApiException(status, operation, LastError());
    }

    public void Log(NativeLogLevel level, string message)
    {
        fixed (byte* text = Native.Utf8(message)) {
            _api->Log(Context, (int)level, text);
        }
    }

    public void ReportState(ScenarioState state, string? resultJson)
    {
        fixed (byte* json = resultJson == null ? null : Native.Utf8(resultJson)) {
            _api->ReportState(Context, (int)state, json);
        }
    }

    public void RequestExit(int exitCode) => _api->RequestExit(Context, exitCode);

    public ulong InstantiateModel(string path, string name, in Transform transform)
    {
        ulong handle;
        fixed (byte* p = Native.Utf8(path))
        fixed (byte* n = Native.Utf8(name))
        fixed (Transform* t = &transform) {
            Check(_api->ModelInstantiate(Context, p, n, t, &handle), $"instantiating {path}");
        }
        return handle;
    }

    public int GetEntityState(ulong handle, out string? error)
    {
        int state;
        Check(_api->EntityGetState(Context, handle, &state), "entity_get_state");
        error = state == EntityStates.Failed ? LastError() : null;
        return state;
    }

    public void DestroyEntity(ulong handle) => Check(_api->EntityDestroy(Context, handle), "entity_destroy");

    public void SetTransform(ulong handle, in Transform transform)
    {
        fixed (Transform* t = &transform) {
            Check(_api->EntitySetTransform(Context, handle, t), "entity_set_transform");
        }
    }

    public Transform GetTransform(ulong handle)
    {
        Transform transform;
        Check(_api->EntityGetTransform(Context, handle, &transform), "entity_get_transform");
        return transform;
    }

    public void SetParent(ulong handle, ulong parent) => Check(_api->EntitySetParent(Context, handle, parent), "entity_set_parent");

    public bool TryGetWorldMatrix(ulong handle, out System.Numerics.Matrix4x4 world)
    {
        System.Numerics.Matrix4x4 matrix;
        int status = _api->EntityGetWorldMatrix(Context, handle, (float*)&matrix);
        if (status == AbiConstants.NotFound) {
            world = default;
            return false;
        }
        Check(status, "entity_get_world_matrix");
        world = matrix;
        return true;
    }

    public ulong GetCamera()
    {
        ulong handle;
        Check(_api->CameraGet(Context, &handle), "camera_get");
        return handle;
    }

    public void SetCameraProjection(float fovYRadians, float zNear, float zFar) =>
        Check(_api->CameraSetProjection(Context, fovYRadians, zNear, zFar), "camera_set_projection");

    public BRCameraProjection GetCameraProjection()
    {
        BRCameraProjection projection;
        Check(_api->CameraGetProjection(Context, &projection), "camera_get_projection");
        return projection;
    }

    public void SetCameraInputEnabled(bool enabled) => Check(_api->CameraSetInputEnabled(Context, enabled ? 1 : 0), "camera_set_input_enabled");

    public ulong CreateLight(in BRLightDesc desc, in Transform transform, string name)
    {
        ulong handle;
        fixed (BRLightDesc* d = &desc)
        fixed (Transform* t = &transform)
        fixed (byte* n = Native.Utf8(name)) {
            Check(_api->LightCreate(Context, d, t, n, &handle), "light_create");
        }
        return handle;
    }

    public void SetLight(ulong handle, in BRLightDesc desc)
    {
        fixed (BRLightDesc* d = &desc) {
            Check(_api->LightSet(Context, handle, d), "light_set");
        }
    }

    public BRLightDesc GetLight(ulong handle)
    {
        BRLightDesc desc;
        Check(_api->LightGet(Context, handle, &desc), "light_get");
        return desc;
    }

    public void SetEnvironment(string name)
    {
        fixed (byte* n = Native.Utf8(name)) {
            Check(_api->SceneSetEnvironment(Context, n), "scene_set_environment");
        }
    }

    public SceneStatus GetSceneStatus()
    {
        BRSceneStatus status;
        Check(_api->SceneGetStatus(Context, &status), "scene_get_status");
        return new SceneStatus(status.Frame, status.PendingWork, status.WorkloadSignature, status.PendingShaderCompiles, status.PendingModels);
    }

    public object GetSetting(string name, int type)
    {
        ulong storage = 0;   // large enough for every setting type
        fixed (byte* n = Native.Utf8(name)) {
            Check(_api->SettingGet(Context, n, type, &storage), $"reading setting '{name}'");
        }
        return type switch
        {
            SettingTypes.Bool => *(int*)&storage != 0,
            SettingTypes.Int32 => *(int*)&storage,
            SettingTypes.UInt32 or SettingTypes.UInt8 => *(uint*)&storage,
            SettingTypes.Float => *(float*)&storage,
            _ => throw new ArgumentOutOfRangeException(nameof(type)),
        };
    }

    public void SetSetting(string name, int type, object value)
    {
        ulong storage = 0;
        switch (type) {
        case SettingTypes.Bool: *(int*)&storage = Convert.ToBoolean(value) ? 1 : 0; break;
        case SettingTypes.Int32: *(int*)&storage = Convert.ToInt32(value); break;
        case SettingTypes.UInt32:
        case SettingTypes.UInt8: *(uint*)&storage = Convert.ToUInt32(value); break;
        case SettingTypes.Float: *(float*)&storage = Convert.ToSingle(value); break;
        default: throw new ArgumentOutOfRangeException(nameof(type));
        }
        fixed (byte* n = Native.Utf8(name)) {
            Check(_api->SettingSet(Context, n, type, &storage), $"setting '{name}'");
        }
    }

    public string InvokeCommand(string requestJson)
    {
        fixed (byte* r = Native.Utf8(requestJson)) {
            return Native.FromUtf8(_api->CommandInvoke(Context, r)) ?? "{\"ok\":false,\"error\":\"no reply\"}";
        }
    }
}

