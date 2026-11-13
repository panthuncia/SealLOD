// An in-memory IHostApi: entities with transforms and parents, a camera, lights, settings, and a few commands. Enough
// to run scenarios (the examples included) without a renderer, so the API, animations and runtime can be tested on
// any machine.
using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;
using System.Text.Json.Nodes;
using BasicRenderer.Scripting;
using BasicRenderer.Scripting.Interop;

internal sealed class FakeHost : IHostApi
{
    private sealed class FakeEntity
    {
        public string Name = "";
        public string Kind = "model";
        public Transform Transform = Transform.Identity;
        public ulong Parent;
        public BRLightDesc Light;
        public int FramesUntilReady;
        public string? Failure;
    }

    private readonly Dictionary<ulong, FakeEntity> _entities = new();
    private ulong _next = 1;
    private ulong _camera;
    private BRCameraProjection _projection = new() { FovYRadians = 1.0f, Aspect = 16f / 9f, ZNear = 0.1f, ZFar = 1000f };
    private int _jobPolls;

    public readonly List<string> Logs = new();
    public readonly Dictionary<string, (int Type, object Value)> Settings = new()
    {
        ["enableBloom"] = (SettingTypes.Bool, false),
        ["exposure"] = (SettingTypes.Float, 1.0f),
        ["maxShadowCascades"] = (SettingTypes.UInt32, 4u),
    };
    public bool CameraInputEnabled = true;
    public string? Environment;
    public ulong PendingWork;
    public ulong Signature = 7;
    public int ModelLoadFrames;   // frames a model stays pending (0: ready at once)
    public readonly HashSet<string> MissingFiles = new();

    /// <summary>Advances fake time: models count down to ready. Call once per frame, as the host's tick would.</summary>
    public void Frame()
    {
        foreach (var entity in _entities.Values) {
            if (entity.FramesUntilReady > 0) entity.FramesUntilReady--;
        }
    }

    public int EntityCount => _entities.Count - (_camera != 0 ? 1 : 0);

    public void Log(NativeLogLevel level, string message) => Logs.Add(message);
    public void ReportState(ScenarioState state, string? resultJson) { }
    public void RequestExit(int exitCode) { }

    public ulong InstantiateModel(string path, string name, in Transform transform)
    {
        ulong handle = _next++;
        _entities[handle] = new FakeEntity
        {
            Name = name,
            Transform = transform,
            FramesUntilReady = ModelLoadFrames,
            Failure = MissingFiles.Contains(System.IO.Path.GetFileName(path)) ? $"could not load '{path}'" : null,
        };
        return handle;
    }

    private FakeEntity Get(ulong handle) =>
        _entities.TryGetValue(handle, out var entity) ? entity : throw new ScriptApiException(AbiConstants.StaleHandle, "fake", $"handle {handle} is not live");

    public int GetEntityState(ulong handle, out string? error)
    {
        var entity = Get(handle);
        error = entity.Failure;
        if (entity.Failure != null) return EntityStates.Failed;
        return entity.FramesUntilReady > 0 ? EntityStates.Pending : EntityStates.Ready;
    }

    public void DestroyEntity(ulong handle)
    {
        Get(handle);
        _entities.Remove(handle);
        foreach (var entity in _entities.Values) {
            if (entity.Parent == handle) entity.Parent = 0;
        }
    }

    public void SetTransform(ulong handle, in Transform transform) => Get(handle).Transform = transform;
    public Transform GetTransform(ulong handle) => Get(handle).Transform;
    public void SetParent(ulong handle, ulong parent) => Get(handle).Parent = parent;

    public bool TryGetWorldMatrix(ulong handle, out Matrix4x4 world)
    {
        world = Matrix4x4.Identity;
        for (ulong current = handle; current != 0; current = Get(current).Parent) {
            var t = Get(current).Transform;
            var local = Matrix4x4.CreateScale(t.Scale) * Matrix4x4.CreateFromQuaternion(t.Rotation) * Matrix4x4.CreateTranslation(t.Position);
            world *= local;   // row vectors: child first, then each parent
        }
        return true;
    }

    public ulong GetCamera()
    {
        if (_camera == 0) {
            _camera = _next++;
            _entities[_camera] = new FakeEntity { Name = "camera", Kind = "camera", Transform = Transform.At((0, 2, 10)) };
        }
        return _camera;
    }

    public void SetCameraProjection(float fovYRadians, float zNear, float zFar)
    {
        if (fovYRadians > 0) _projection.FovYRadians = fovYRadians;
        if (zNear > 0) _projection.ZNear = zNear;
        if (zFar > 0) _projection.ZFar = zFar;
    }

    public BRCameraProjection GetCameraProjection() => _projection;
    public void SetCameraInputEnabled(bool enabled) => CameraInputEnabled = enabled;

    public ulong CreateLight(in BRLightDesc desc, in Transform transform, string name)
    {
        ulong handle = _next++;
        _entities[handle] = new FakeEntity { Name = name, Kind = "light", Transform = transform, Light = desc };
        return handle;
    }

    public void SetLight(ulong handle, in BRLightDesc desc)
    {
        var entity = Get(handle);
        var updated = desc;
        updated.Type = entity.Light.Type;
        updated.CastsShadows = entity.Light.CastsShadows;
        entity.Light = updated;
    }

    public BRLightDesc GetLight(ulong handle) => Get(handle).Light;
    public void SetEnvironment(string name) => Environment = name;

    public SceneStatus GetSceneStatus()
    {
        uint pending = 0;
        foreach (var entity in _entities.Values) {
            if (entity.FramesUntilReady > 0) pending++;
        }
        return new SceneStatus(0, PendingWork, Signature, 0, pending);
    }

    public object GetSetting(string name, int type)
    {
        if (!Settings.TryGetValue(name, out var setting)) throw new ScriptApiException(AbiConstants.NotFound, "fake", $"no setting named '{name}'");
        if (setting.Type != type) throw new ScriptApiException(AbiConstants.TypeMismatch, "fake", $"setting '{name}' has another type");
        return setting.Value;
    }

    public void SetSetting(string name, int type, object value)
    {
        GetSetting(name, type);
        Settings[name] = (type, type switch
        {
            SettingTypes.Bool => Convert.ToBoolean(value),
            SettingTypes.Float => Convert.ToSingle(value),
            SettingTypes.Int32 => Convert.ToInt32(value),
            _ => (object)Convert.ToUInt32(value),
        });
    }

    /// <summary>echo, a job that publishes on its third poll ("fake.job"), and job.status.</summary>
    public string InvokeCommand(string requestJson)
    {
        var request = JsonNode.Parse(requestJson)!.AsObject();
        var command = (string?)request["command"];
        var reply = new JsonObject { ["ok"] = true };
        switch (command) {
        case "echo":
            reply["echo"] = request.DeepClone();
            break;
        case "fake.job":
            _jobPolls = 0;
            reply["job_id"] = 42;
            break;
        case "job.status":
            _jobPolls++;
            reply["job"] = new JsonObject { ["id"] = 42, ["state"] = _jobPolls >= 3 ? "published" : "waiting" };
            break;
        default:
            return JsonSerializer.Serialize(new { ok = false, error = $"unknown command '{command}'" });
        }
        return reply.ToJsonString();
    }
}
