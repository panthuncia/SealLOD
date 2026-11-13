using System;
using System.Collections.Generic;
using System.Linq;

namespace BasicRenderer.Scripting;

/// <summary>Which parts of a transform an animation writes; the rest stay whatever else sets them.</summary>
[Flags]
public enum Channels
{
    None = 0,
    Position = 1,
    Rotation = 2,
    Scale = 4,
    All = Position | Rotation | Scale,
}

/// <summary>
/// A sequence of steps animating one entity, built fluently and started at once:
/// <code>
/// cube.Animate().MoveTo((0, 2, 0), 1.5, Ease.InOutSine).Wait(0.5).RotateBy(Vec3.UnitY, 90, 1).Loop();
/// </code>
/// Steps run one after another; each starts from where the previous one ended (the first from the entity's transform
/// when the animation first runs). It writes only the channels its steps touch, so a spin and a move on the same entity
/// combine. Await it to resume when it ends.
/// </summary>
public sealed class AnimationBuilder : Animation
{
    private readonly Entity _target;
    private readonly List<Step> _steps = new();
    private Transform? _base;
    private Channels _channels;
    private double _duration;

    internal AnimationBuilder(ScenarioContext ctx, Entity target) : base(ctx) => _target = target;

    public Entity Target => _target;

    public override double Duration => _duration;

    protected override bool TargetGone => !_target.IsAlive;

    // --- Steps -------------------------------------------------------------------------------------------------

    /// <summary>Moves to <paramref name="position"/> (local) over <paramref name="seconds"/>.</summary>
    public AnimationBuilder MoveTo(Vec3 position, double seconds, Easing? ease = null) =>
        Add(Channels.Position, seconds, ease, (from, t) => from.WithPosition(Vec3.Lerp(from.Position, position, (float)t)));

    public AnimationBuilder MoveBy(Vec3 offset, double seconds, Easing? ease = null) =>
        Add(Channels.Position, seconds, ease, (from, t) => from.WithPosition(from.Position + offset * (float)t));

    public AnimationBuilder RotateTo(Quat rotation, double seconds, Easing? ease = null) =>
        Add(Channels.Rotation, seconds, ease, (from, t) => from.WithRotation(Quat.Slerp(from.Rotation, rotation, (float)t)));

    /// <summary>Turns by <paramref name="degrees"/> about <paramref name="axis"/> (local to the parent), any angle, over the duration.</summary>
    public AnimationBuilder RotateBy(Vec3 axis, double degrees, double seconds, Easing? ease = null) =>
        Add(Channels.Rotation, seconds, ease, (from, t) => from.WithRotation(from.Rotation * Quat.AxisAngle(axis, degrees * t)));

    /// <summary>Turns about <paramref name="axis"/> at a constant rate, forever (until stopped). Ends the step list.</summary>
    public AnimationBuilder Spin(Vec3 axis, double degreesPerSecond) =>
        Add(Channels.Rotation, double.PositiveInfinity, null, (from, seconds) => from.WithRotation(from.Rotation * Quat.AxisAngle(axis, degreesPerSecond * seconds)),
            unboundedSeconds: true);

    public AnimationBuilder ScaleTo(Vec3 scale, double seconds, Easing? ease = null) =>
        Add(Channels.Scale, seconds, ease, (from, t) => from.WithScale(Vec3.Lerp(from.Scale, scale, (float)t)));

    public AnimationBuilder ScaleTo(float uniform, double seconds, Easing? ease = null) => ScaleTo(new Vec3(uniform, uniform, uniform), seconds, ease);

    /// <summary>Moves and turns together (a camera move).</summary>
    public AnimationBuilder PoseTo(Vec3 position, Quat rotation, double seconds, Easing? ease = null) =>
        Add(Channels.Position | Channels.Rotation, seconds, ease,
            (from, t) => new Transform(Vec3.Lerp(from.Position, position, (float)t), Quat.Slerp(from.Rotation, rotation, (float)t), from.Scale));

    /// <summary>
    /// A smooth curve (Catmull-Rom) from the current position through <paramref name="points"/>. With
    /// <paramref name="face"/>, the entity turns to look along the curve (cameras look along -Z).
    /// </summary>
    public AnimationBuilder Path(IReadOnlyList<Vec3> points, double seconds, Easing? ease = null, bool face = false)
    {
        if (points.Count == 0) return this;
        var copy = points.ToArray();
        return Add(face ? Channels.Position | Channels.Rotation : Channels.Position, seconds, ease, (from, t) =>
        {
            var all = new Vec3[copy.Length + 1];
            all[0] = from.Position;
            copy.CopyTo(all, 1);
            Vec3 position = CatmullRom(all, t);
            if (!face) return from.WithPosition(position);
            Vec3 ahead = CatmullRom(all, Math.Min(1, t + 1e-3));
            Vec3 direction = ahead - position;
            if (direction.LengthSquared < 1e-12f) direction = position - CatmullRom(all, Math.Max(0, t - 1e-3));
            return new Transform(position, direction.LengthSquared > 1e-12f ? FacingRotation(direction) : from.Rotation, from.Scale);
        });
    }

    /// <summary>
    /// Circles <paramref name="center"/> at <paramref name="radius"/>, <paramref name="height"/> above it, making
    /// <paramref name="turns"/> turns (negative: clockwise from above). Starts at the entity's current angle about the
    /// centre. With <paramref name="lookAtCenter"/> the entity faces the centre (a camera's -Z, a light's +Z).
    /// </summary>
    public AnimationBuilder Orbit(Vec3 center, double radius, double height, double seconds, double turns = 1, bool lookAtCenter = true,
        Easing? ease = null)
    {
        return Add(lookAtCenter ? Channels.Position | Channels.Rotation : Channels.Position, seconds, ease, (from, t) =>
        {
            Vec3 offset = from.Position - center;
            double startAngle = offset.X * offset.X + offset.Z * offset.Z > 1e-8 ? Math.Atan2(offset.Z, offset.X) : 0;
            double angle = startAngle - turns * 2 * Math.PI * t;   // positive turns: counter-clockwise from above (+Y)
            var position = new Vec3(center.X + (float)(radius * Math.Cos(angle)), center.Y + (float)height, center.Z + (float)(radius * Math.Sin(angle)));
            return lookAtCenter ? new Transform(position, FacingRotation(center - position), from.Scale) : from.WithPosition(position);
        });
    }

    /// <summary>Follows a keyframed position track for its length.</summary>
    public AnimationBuilder Follow(Track<Vec3> positions) =>
        Add(Channels.Position, positions.Duration, null, (from, t) => from.WithPosition(positions.Sample(t * positions.Duration)));

    /// <summary>Follows a keyframed rotation track for its length.</summary>
    public AnimationBuilder Follow(Track<Quat> rotations) =>
        Add(Channels.Rotation, rotations.Duration, null, (from, t) => from.WithRotation(rotations.Sample(t * rotations.Duration)));

    /// <summary>Holds for <paramref name="seconds"/>.</summary>
    public AnimationBuilder Wait(double seconds) => Add(Channels.None, seconds, null, (from, _) => from);

    /// <summary>Runs <paramref name="action"/> once each time the sequence reaches this point going forward (in a ping-pong, once per forward pass).</summary>
    public AnimationBuilder Then(Action action)
    {
        long firedCycle = -1;
        _steps.Add(new Step(_duration, 0, null, Channels.None, (from, _) => from, Unbounded: false, cycle =>
        {
            if (cycle == firedCycle) return;
            firedCycle = cycle;
            action();
        }));
        return this;
    }

    // --- Timing ------------------------------------------------------------------------------------------------

    public AnimationBuilder Delay(double seconds)
    {
        SetDelay(seconds);
        return this;
    }

    /// <summary>Repeats the sequence from its start; 0 repeats forever.</summary>
    public AnimationBuilder Loop(int count = 0)
    {
        SetLoops(count);
        return this;
    }

    /// <summary>Plays the sequence forward, then backward, on alternate loops (0 forever).</summary>
    public AnimationBuilder PingPong(int loops = 0)
    {
        SetPingPong();
        SetLoops(loops);
        return this;
    }

    // --- Evaluation --------------------------------------------------------------------------------------------

    protected override void Apply(double cycleTime, long cycle)
    {
        // The starting point: the entity as it is when the animation first runs, so steps begin where it stands.
        _base ??= _target.Transform;
        Transform state = _base.Value;
        foreach (var step in _steps) {
            if (cycleTime + 1e-9 < step.Start) break;
            // Forward passes only: a ping-pong's turn is one moment, not an arrival and a departure.
            if (!IsReverseCycle(cycle)) step.Callback?.Invoke(cycle);
            if (step.Unbounded) {
                state = step.Apply(state, cycleTime - step.Start);
                break;
            }
            double elapsed = cycleTime - step.Start;
            if (step.Duration <= 0 || elapsed >= step.Duration) {
                state = step.Apply(state, 1);
                continue;
            }
            state = step.Apply(state, Ease.Apply(step.Ease, elapsed / step.Duration));
            break;
        }
        if (_channels == Channels.None) return;
        Transform current = _target.Transform;
        _target.Transform = new Transform(
            (_channels & Channels.Position) != 0 ? state.Position : current.Position,
            (_channels & Channels.Rotation) != 0 ? state.Rotation : current.Rotation,
            (_channels & Channels.Scale) != 0 ? state.Scale : current.Scale);
    }

    private AnimationBuilder Add(Channels channels, double seconds, Easing? ease, Func<Transform, double, Transform> apply, bool unboundedSeconds = false)
    {
        if (double.IsPositiveInfinity(_duration)) throw new InvalidOperationException("nothing can follow a step that never ends (Spin)");
        if (!unboundedSeconds && (double.IsNaN(seconds) || seconds < 0)) throw new ArgumentOutOfRangeException(nameof(seconds), "durations are seconds >= 0");
        _steps.Add(new Step(_duration, unboundedSeconds ? 0 : seconds, ease, channels, apply, unboundedSeconds, null));
        _channels |= channels;
        _duration = unboundedSeconds ? double.PositiveInfinity : _duration + seconds;
        return this;
    }

    /// <summary>Cameras look along -Z; everything else (lights, models) faces along +Z.</summary>
    private Quat FacingRotation(Vec3 direction) => _target is Camera ? Quat.LookRotation(direction) : Quat.PointZ(direction);

    private static Vec3 CatmullRom(Vec3[] points, double t)
    {
        if (points.Length == 1) return points[0];
        double scaled = Math.Clamp(t, 0, 1) * (points.Length - 1);
        int segment = Math.Min((int)scaled, points.Length - 2);
        float u = (float)(scaled - segment);
        Vec3 p0 = points[Math.Max(segment - 1, 0)], p1 = points[segment], p2 = points[segment + 1], p3 = points[Math.Min(segment + 2, points.Length - 1)];
        float u2 = u * u, u3 = u2 * u;
        return 0.5f * (2 * p1 + (p2 - p0) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u2 + (3 * p1 - p0 - 3 * p2 + p3) * u3);
    }

    /// <summary>One step: Apply(from, progress) where progress is eased [0, 1], or seconds for an unbounded step.</summary>
    private sealed record Step(double Start, double Duration, Easing? Ease, Channels Channels, Func<Transform, double, Transform> Apply, bool Unbounded,
        Action<long>? Callback);
}

/// <summary>Keyframed values sampled by time: <c>Track.Vec3().Key(0, a).Key(2, b, Ease.InOutSine)</c>.</summary>
public sealed class Track<T>
{
    private readonly List<(double Time, T Value, Easing? Ease)> _keys = new();
    private readonly Func<T, T, float, T> _lerp;

    internal Track(Func<T, T, float, T> lerp) => _lerp = lerp;

    /// <summary>The time of the last key.</summary>
    public double Duration => _keys.Count == 0 ? 0 : _keys[^1].Time;

    public int Count => _keys.Count;

    /// <summary>Adds a key at <paramref name="time"/> seconds. The ease shapes the approach to this key from the previous one.</summary>
    public Track<T> Key(double time, T value, Easing? ease = null)
    {
        if (_keys.Count > 0 && time < _keys[^1].Time) throw new ArgumentException("keys must be added in time order", nameof(time));
        _keys.Add((time, value, ease));
        return this;
    }

    /// <summary>The value at <paramref name="time"/>, held at the first and last keys outside their range.</summary>
    public T Sample(double time)
    {
        if (_keys.Count == 0) throw new InvalidOperationException("the track has no keys");
        if (time <= _keys[0].Time) return _keys[0].Value;
        for (int i = 1; i < _keys.Count; i++) {
            var (t1, v1, ease) = _keys[i];
            if (time > t1) continue;
            var (t0, v0, _) = _keys[i - 1];
            double span = t1 - t0;
            return span <= 0 ? v1 : _lerp(v0, v1, (float)Ease.Apply(ease, (time - t0) / span));
        }
        return _keys[^1].Value;
    }
}

public static class Track
{
    public static Track<Vec3> Vec3() => new(Scripting.Vec3.Lerp);
    public static Track<Quat> Quat() => new(Scripting.Quat.Slerp);
    public static Track<float> Float() => new((a, b, t) => a + (b - a) * t);
    public static Track<double> Double() => new((a, b, t) => a + (b - a) * t);
}
