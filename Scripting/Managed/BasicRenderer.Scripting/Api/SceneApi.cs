using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using BasicRenderer.Scripting.Interop;

namespace BasicRenderer.Scripting;

/// <summary>Scene status as the host reports it.</summary>
/// <param name="Frame">Frames the renderer has rendered.</param>
/// <param name="PendingWork">Queued streaming, uploads, texture reloads and background tasks.</param>
/// <param name="WorkloadSignature">Changes while textures reach full resolution and geometry becomes resident.</param>
/// <param name="PendingShaderCompiles">Pipelines requested and not yet built.</param>
/// <param name="PendingModels">The scenario's models still loading.</param>
public readonly record struct SceneStatus(ulong Frame, ulong PendingWork, ulong WorkloadSignature, uint PendingShaderCompiles, uint PendingModels)
{
    public bool NothingPending => PendingWork == 0 && PendingShaderCompiles == 0 && PendingModels == 0;
}

/// <summary>Creates models and lights. Everything a scenario creates is removed when the next scenario starts.</summary>
public sealed class SceneApi
{
    private readonly ScenarioContext _ctx;
    private readonly List<Entity> _entities = new();

    internal SceneApi(ScenarioContext ctx) => _ctx = ctx;

    /// <summary>The entities this scenario created, in creation order.</summary>
    public IReadOnlyList<Entity> Entities => _entities;

    /// <summary>Switches the environment (sky and image-based lighting) by name, as the menu does.</summary>
    public void Environment(string name) => Native.Api.SetEnvironment(name);

    public SceneStatus Status
    {
        get
        {
            var s = Native.Api.GetSceneStatus();
            return s;
        }
    }

    /// <summary>
    /// Places a model at once; it appears when its file has loaded (await <see cref="Entity.WhenReady"/>, or use
    /// <see cref="LoadModel"/>). A relative path is looked up beside the scenario's source first, then from the
    /// working directory. Loads are cached, so many instances of one file load it once.
    /// </summary>
    public Model Instantiate(string path, Vec3? position = null, string? name = null)
    {
        var resolved = _ctx.ResolvePath(path);
        var transform = Transform.At(position ?? Vec3.Zero);
        var label = name ?? $"{Path.GetFileNameWithoutExtension(resolved)}#{_entities.Count}";
        var model = new Model(_ctx, Native.Api.InstantiateModel(resolved, label, transform), label, resolved);
        _entities.Add(model);
        return model;
    }

    /// <summary><see cref="Instantiate"/>, resuming once the model is in the scene. Throws when it does not load.</summary>
    public async Task<Model> LoadModel(string path, Vec3? position = null, string? name = null, TimeSpan? timeout = null)
    {
        var model = Instantiate(path, position, name);
        await model.WhenReady(timeout);
        return model;
    }

    /// <summary>A sun-like light shining along <paramref name="direction"/>.</summary>
    public Light DirectionalLight(Vec3 direction, float intensity = 5f, Vec3? color = null, bool shadows = true, string? name = null) =>
        CreateLight(LightKind.Directional, Vec3.Zero, direction, intensity, color, 0, 0, 0, 0, shadows, name);

    /// <summary>A point light. A range of 0 picks one from the intensity.</summary>
    public Light PointLight(Vec3 position, float intensity, Vec3? color = null, float range = 0, float sourceRadius = 0, bool shadows = true,
        string? name = null) =>
        CreateLight(LightKind.Point, position, Vec3.UnitZ, intensity, color, range, sourceRadius, 0, 0, shadows, name);

    /// <summary>A spot light; cone angles are half-angles in degrees.</summary>
    public Light SpotLight(Vec3 position, Vec3 direction, float intensity, double innerConeDegrees = 20, double outerConeDegrees = 30, Vec3? color = null,
        float range = 0, float sourceRadius = 0, bool shadows = true, string? name = null) =>
        CreateLight(LightKind.Spot, position, direction, intensity, color, range, sourceRadius, innerConeDegrees, outerConeDegrees, shadows, name);

    private Light CreateLight(LightKind kind, Vec3 position, Vec3 direction, float intensity, Vec3? color, float range, float sourceRadius,
        double innerDegrees, double outerDegrees, bool shadows, string? name)
    {
        var desc = new BRLightDesc
        {
            Type = (int)kind,
            CastsShadows = shadows ? 1 : 0,
            Color = color ?? Vec3.One,
            Intensity = intensity,
            Range = range,
            SourceRadius = sourceRadius,
            InnerConeRadians = (float)(innerDegrees * Math.PI / 180.0),
            OuterConeRadians = (float)(outerDegrees * Math.PI / 180.0),
        };
        var label = name ?? $"{kind.ToString().ToLowerInvariant()} light #{_entities.Count}";
        var transform = new Transform(position, Quat.PointZ(direction), Vec3.One);
        var light = new Light(_ctx, Native.Api.CreateLight(desc, transform, label), label);
        _entities.Add(light);
        return light;
    }
}
