using System;
using System.Numerics;
using System.Threading.Tasks;
using BasicRenderer.Scripting.Interop;

namespace BasicRenderer.Scripting;

public enum EntityState
{
    /// <summary>A model whose file is still loading, or anything not yet through a frame.</summary>
    Pending,
    Ready,
    /// <summary>It could not be created (see <see cref="Entity.Error"/>), or it was destroyed.</summary>
    Failed,
}

/// <summary>
/// Something in the scene a scenario created or controls. Its transform is local to its parent (the world without
/// one). Writes are buffered and reach the renderer before the next frame's scene update; reading the transform back
/// returns what the script last wrote.
/// </summary>
public abstract class Entity
{
    private bool _destroyed;
    private Entity? _parent;

    private protected Entity(ScenarioContext ctx, ulong handle, string name)
    {
        Context = ctx;
        Handle = handle;
        Name = name;
    }

    public ScenarioContext Context { get; }
    internal ulong Handle { get; }
    public string Name { get; }

    public EntityState State
    {
        get
        {
            if (_destroyed) return EntityState.Failed;
            try {
                return Native.Api.GetEntityState(Handle, out _) switch
                {
                    EntityStates.Ready => EntityState.Ready,
                    EntityStates.Failed => EntityState.Failed,
                    _ => EntityState.Pending,
                };
            } catch (ScriptApiException) {
                return EntityState.Failed;
            }
        }
    }

    /// <summary>Why the entity failed, or null.</summary>
    public string? Error
    {
        get
        {
            if (_destroyed) return "destroyed";
            try {
                return Native.Api.GetEntityState(Handle, out var error) == EntityStates.Failed ? error : null;
            } catch (ScriptApiException e) {
                return e.Message;
            }
        }
    }

    public bool IsReady => State == EntityState.Ready;

    /// <summary>Not destroyed and not failed.</summary>
    public bool IsAlive => !_destroyed && State != EntityState.Failed;

    public Transform Transform
    {
        get => Native.Api.GetTransform(Handle);
        set
        {
            OnTransformWrite();
            Native.Api.SetTransform(Handle, value);
        }
    }

    public Vec3 Position
    {
        get => Transform.Position;
        set => Transform = Transform.WithPosition(value);
    }

    public Quat Rotation
    {
        get => Transform.Rotation;
        set => Transform = Transform.WithRotation(value);
    }

    public Vec3 Scale
    {
        get => Transform.Scale;
        set => Transform = Transform.WithScale(value);
    }

    /// <summary>The world matrix the renderer last used for it (row vectors), or null before its first frame.</summary>
    public Matrix4x4? WorldMatrix => Native.Api.TryGetWorldMatrix(Handle, out var world) ? world : null;

    /// <summary>Where the renderer last placed it, or null before its first frame.</summary>
    public Vec3? WorldPosition => WorldMatrix is { } m ? new Vec3(m.M41, m.M42, m.M43) : null;

    /// <summary>The parent entity (null: the scene root). Reparenting keeps the local transform.</summary>
    public Entity? Parent
    {
        get => _parent;
        set
        {
            Native.Api.SetParent(Handle, value?.Handle ?? 0);
            _parent = value;
        }
    }

    // --- Fluent placement ----------------------------------------------------------------------------------------

    public Entity At(Vec3 position)
    {
        Position = position;
        return this;
    }

    public Entity At(float x, float y, float z) => At(new Vec3(x, y, z));

    public Entity Rotated(Quat rotation)
    {
        Rotation = rotation;
        return this;
    }

    /// <summary>Euler angles in degrees (pitch about X, yaw about Y, roll about Z).</summary>
    public Entity Rotated(double pitchDegrees, double yawDegrees, double rollDegrees = 0) => Rotated(Quat.Euler(pitchDegrees, yawDegrees, rollDegrees));

    public Entity Scaled(float uniform) => Scaled(new Vec3(uniform, uniform, uniform));

    public Entity Scaled(Vec3 scale)
    {
        Scale = scale;
        return this;
    }

    public Entity ChildOf(Entity? parent)
    {
        Parent = parent;
        return this;
    }

    /// <summary>Starts an animation of this entity (see <see cref="AnimationBuilder"/>).</summary>
    public AnimationBuilder Animate() => new(Context, this);

    /// <summary>Resumes once the entity is in the scene; throws when it failed (a model file that would not load).</summary>
    public async Task WhenReady(TimeSpan? timeout = null)
    {
        await Context.Until(() => State != EntityState.Pending, timeout ?? TimeSpan.FromSeconds(120));
        if (State == EntityState.Failed) throw new ScenarioFailedException($"{Name} could not be created: {Error}");
    }

    /// <summary>Removes the entity from the scene (at the next frame). Its children move to the scene root.</summary>
    public virtual void Destroy()
    {
        if (_destroyed) return;
        Native.Api.DestroyEntity(Handle);
        _destroyed = true;
    }

    private protected virtual void OnTransformWrite() { }

    public override string ToString() => $"{GetType().Name} '{Name}'";
}

/// <summary>A model file placed in the scene.</summary>
public sealed class Model : Entity
{
    internal Model(ScenarioContext ctx, ulong handle, string name, string path) : base(ctx, handle, name) => Path = path;

    public string Path { get; }

    public new Model At(Vec3 position) => (Model)base.At(position);
    public new Model At(float x, float y, float z) => (Model)base.At(x, y, z);
    public new Model Rotated(Quat rotation) => (Model)base.Rotated(rotation);
    public new Model Rotated(double pitchDegrees, double yawDegrees, double rollDegrees = 0) => (Model)base.Rotated(pitchDegrees, yawDegrees, rollDegrees);
    public new Model Scaled(float uniform) => (Model)base.Scaled(uniform);
    public new Model Scaled(Vec3 scale) => (Model)base.Scaled(scale);
}

public enum LightKind
{
    Point = 0,
    Spot = 1,
    Directional = 2,
}

/// <summary>A light the scenario created. It shines along its local +Z: set <see cref="Direction"/> or the rotation.</summary>
public sealed class Light : Entity
{
    internal Light(ScenarioContext ctx, ulong handle, string name) : base(ctx, handle, name) { }

    private BRLightDesc Desc
    {
        get => Native.Api.GetLight(Handle);
        set => Native.Api.SetLight(Handle, value);
    }

    public LightKind Kind => (LightKind)Desc.Type;
    public bool CastsShadows => Desc.CastsShadows != 0;

    /// <summary>Linear RGB; the renderer normalizes it, so brightness is <see cref="Intensity"/>.</summary>
    public Vec3 Color
    {
        get => Desc.Color;
        set
        {
            var d = Desc;
            d.Color = value;
            Desc = d;
        }
    }

    public float Intensity
    {
        get => Desc.Intensity;
        set
        {
            var d = Desc;
            d.Intensity = value;
            Desc = d;
        }
    }

    /// <summary>Point and spot: where the falloff reaches zero. Setting 0 keeps the current range.</summary>
    public float Range
    {
        get => Desc.Range;
        set
        {
            var d = Desc;
            d.Range = value;
            Desc = d;
        }
    }

    public float SourceRadius
    {
        get => Desc.SourceRadius;
        set
        {
            var d = Desc;
            d.SourceRadius = value;
            Desc = d;
        }
    }

    /// <summary>Spot: the cone's inner and outer half-angles in degrees.</summary>
    public double InnerConeDegrees
    {
        get => Desc.InnerConeRadians * 180.0 / Math.PI;
        set
        {
            var d = Desc;
            d.InnerConeRadians = (float)(value * Math.PI / 180.0);
            Desc = d;
        }
    }

    public double OuterConeDegrees
    {
        get => Desc.OuterConeRadians * 180.0 / Math.PI;
        set
        {
            var d = Desc;
            d.OuterConeRadians = (float)(value * Math.PI / 180.0);
            Desc = d;
        }
    }

    /// <summary>The direction it shines (its +Z in the world when it has no parent).</summary>
    public Vec3 Direction
    {
        get => Rotation.Rotate(Vec3.UnitZ);
        set => Rotation = Quat.PointZ(value);
    }

    /// <summary>Animates the intensity to <paramref name="intensity"/>.</summary>
    public Tween FadeTo(float intensity, double seconds, Easing? ease = null) =>
        Context.Tween(Intensity, intensity, seconds, v => Intensity = (float)v, ease);
}

/// <summary>
/// The primary camera. Moving it takes it from the fly camera (keyboard and mouse) until <see cref="ReleaseControl"/>
/// or the scenario ends. It looks along its local -Z.
/// </summary>
public sealed class Camera : Entity
{
    private bool _controlled;

    internal Camera(ScenarioContext ctx, ulong handle) : base(ctx, handle, "camera") { }

    /// <summary>Turns the fly camera off so the script alone moves the camera. Writing the transform does this too.</summary>
    public Camera TakeControl()
    {
        if (!_controlled) {
            Native.Api.SetCameraInputEnabled(false);
            _controlled = true;
        }
        return this;
    }

    /// <summary>Gives the camera back to the fly camera.</summary>
    public Camera ReleaseControl()
    {
        if (_controlled) {
            Native.Api.SetCameraInputEnabled(true);
            _controlled = false;
        }
        return this;
    }

    private protected override void OnTransformWrite() => TakeControl();

    /// <summary>Vertical field of view in degrees.</summary>
    public double FieldOfView
    {
        get => Native.Api.GetCameraProjection().FovYRadians * 180.0 / Math.PI;
        set => Native.Api.SetCameraProjection((float)(value * Math.PI / 180.0), 0, 0);
    }

    public float Near
    {
        get => Native.Api.GetCameraProjection().ZNear;
        set => Native.Api.SetCameraProjection(0, value, 0);
    }

    public float Far
    {
        get => Native.Api.GetCameraProjection().ZFar;
        set => Native.Api.SetCameraProjection(0, 0, value);
    }

    /// <summary>Width over height, from the window.</summary>
    public float Aspect => Native.Api.GetCameraProjection().Aspect;

    /// <summary>The direction it looks (its -Z).</summary>
    public Vec3 Forward => Rotation.Rotate(Vec3.Forward);

    public Camera Fov(double degrees)
    {
        FieldOfView = degrees;
        return this;
    }

    /// <summary>Places the camera at <paramref name="eye"/> looking at <paramref name="target"/>.</summary>
    public Camera LookAt(Vec3 eye, Vec3 target, Vec3? up = null)
    {
        Transform = new Transform(eye, Quat.LookRotation(target - eye, up), Vec3.One);
        return this;
    }

    /// <summary>Turns the camera, where it stands, to look at <paramref name="target"/>.</summary>
    public Camera LookAt(Vec3 target, Vec3? up = null) => LookAt(Position, target, up);

    /// <summary>Moves to <paramref name="eye"/> looking at <paramref name="target"/> over the duration.</summary>
    public AnimationBuilder FlyTo(Vec3 eye, Vec3 target, double seconds, Easing? ease = null)
    {
        TakeControl();
        return Animate().PoseTo(eye, Quat.LookRotation(target - eye), seconds, ease ?? Ease.InOutSine);
    }

    /// <summary>Circles <paramref name="center"/>, looking at it (see <see cref="AnimationBuilder.Orbit"/>).</summary>
    public AnimationBuilder Orbit(Vec3 center, double radius, double height, double seconds, double turns = 1, Easing? ease = null)
    {
        TakeControl();
        return Animate().Orbit(center, radius, height, seconds, turns, lookAtCenter: true, ease);
    }

    /// <summary>Keeps the camera at <paramref name="offset"/> from <paramref name="target"/>, looking at it, until stopped.</summary>
    public Animation Follow(Entity target, Vec3 offset)
    {
        TakeControl();
        return new FollowAnimation(Context, this, target, offset);
    }

    public override void Destroy() => throw new InvalidOperationException("the camera cannot be destroyed");

    private sealed class FollowAnimation(ScenarioContext ctx, Camera camera, Entity target, Vec3 offset) : Animation(ctx)
    {
        public override double Duration => double.PositiveInfinity;

        protected override bool TargetGone => !target.IsAlive;

        protected override void Apply(double cycleTime, long cycle)
        {
            Vec3 focus = target.Position;
            camera.LookAt(focus + offset, focus);
        }
    }
}
