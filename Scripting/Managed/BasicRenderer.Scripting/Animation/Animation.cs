using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using BasicRenderer.Scripting.Interop;

namespace BasicRenderer.Scripting;

/// <summary>
/// Something evaluated once per frame from scenario time: an entity animation (<see cref="AnimationBuilder"/>) or a
/// value tween (<see cref="Tween"/>). It starts when created, runs alongside the scenario, and can be awaited:
/// <c>await cube.Animate().MoveTo((0, 2, 0), 1.5)</c> resumes when it ends.
/// </summary>
/// <remarks>
/// Progress is computed from the absolute scenario time since the animation started, never by adding up frame times,
/// so it does not drift and two runs with the same fixed time step produce the same frames.
/// </remarks>
public abstract class Animation
{
    private readonly TaskCompletionSource _completion = new();
    private double _delay;
    private double _pausedTotal;
    private double? _pausedAt;
    private int _loops = 1;
    private bool _pingPong;
    private long _lastCycle = -1;

    private protected Animation(ScenarioContext ctx)
    {
        Context = ctx;
        StartTime = ctx.Time;
        ctx.Animator.Add(this);
    }

    protected ScenarioContext Context { get; }

    /// <summary>Scenario time the animation was created at.</summary>
    public double StartTime { get; }

    public bool IsFinished => _completion.Task.IsCompleted;

    /// <summary>Completes when the animation ends: its last loop finished, or <see cref="Stop"/>.</summary>
    public Task Completion => _completion.Task;

    public TaskAwaiter GetAwaiter() => _completion.Task.GetAwaiter();

    /// <summary>Ends the animation where it is. Awaiting it resumes.</summary>
    public void Stop() => Finish();

    public void Pause() => _pausedAt ??= Context.Time;

    public void Resume()
    {
        if (_pausedAt is { } at) _pausedTotal += Context.Time - at;
        _pausedAt = null;
    }

    /// <summary>The length of one cycle in seconds; infinity for one that never ends (a spin).</summary>
    public abstract double Duration { get; }

    protected void SetDelay(double seconds) => _delay = Math.Max(0, seconds);

    protected void SetLoops(int count) => _loops = Math.Max(0, count);

    protected void SetPingPong() => _pingPong = true;

    /// <summary>Applies the state at <paramref name="cycleTime"/> seconds into cycle <paramref name="cycle"/>.</summary>
    protected abstract void Apply(double cycleTime, long cycle);

    /// <summary>Whether the animation's target is gone (an animation of a destroyed entity ends).</summary>
    protected virtual bool TargetGone => false;

    /// <summary>True once finished. Called by the scenario's animator.</summary>
    internal bool Evaluate(double now)
    {
        if (IsFinished) return true;
        if (TargetGone) {
            Finish();
            return true;
        }
        if (_pausedAt != null) return false;
        double local = now - StartTime - _delay - _pausedTotal;
        if (local < -1e-9) return false;
        local = Math.Max(local, 0);
        try {
            double duration = Duration;
            if (double.IsPositiveInfinity(duration)) {
                Apply(local, 0);
                return false;
            }
            if (duration <= 0) {
                Apply(0, 0);
                Finish();
                return true;
            }
            long cycle = (long)Math.Floor(local / duration + 1e-9);
            long finalCycle = _loops > 0 ? Math.Min(cycle, _loops - 1) : cycle;
            // A cycle can end between two frames: settle each one that ended since the last evaluation, so what happens
            // at its end (a Then step) happens once per cycle. At most a few: a long stall does not replay many.
            if (_lastCycle >= 0) {
                for (long ended = Math.Max(_lastCycle, finalCycle - 4); ended < finalCycle; ended++) Apply(CycleEnd(ended, duration), ended);
            }
            _lastCycle = finalCycle;
            if (_loops > 0 && cycle >= _loops) {
                Apply(CycleEnd(finalCycle, duration), finalCycle);
                Finish();
                return true;
            }
            double within = Math.Clamp(local - cycle * duration, 0, duration);
            if (_pingPong && cycle % 2 == 1) within = duration - within;
            Apply(within, cycle);
            return false;
        } catch (Exception e) {
            Native.Log(NativeLogLevel.Error, $"[{Context.ScenarioName} f{Context.Frame}] animation failed: {e.Message}");
            _completion.TrySetException(e);
            return true;
        }
    }

    private double CycleEnd(long cycle, double duration) => IsReverseCycle(cycle) ? 0 : duration;

    /// <summary>Whether this cycle of a ping-pong plays backward.</summary>
    protected bool IsReverseCycle(long cycle) => _pingPong && cycle % 2 == 1;

    internal void Cancel(CancellationToken token) => _completion.TrySetCanceled(token);

    private void Finish() => _completion.TrySetResult();
}

/// <summary>A number animated from one value to another, handed to a callback each frame.</summary>
public sealed class Tween : Animation
{
    private readonly double _from, _to, _seconds;
    private readonly Easing? _ease;
    private readonly Action<double> _apply;

    internal Tween(ScenarioContext ctx, double from, double to, double seconds, Action<double> apply, Easing? ease) : base(ctx)
    {
        _from = from;
        _to = to;
        _seconds = Math.Max(0, seconds);
        _apply = apply;
        _ease = ease;
    }

    public override double Duration => _seconds;

    protected override void Apply(double cycleTime, long cycle)
    {
        double t = _seconds > 0 ? cycleTime / _seconds : 1;
        _apply(_from + (_to - _from) * Ease.Apply(_ease, t));
    }

    public Tween Delay(double seconds)
    {
        SetDelay(seconds);
        return this;
    }

    /// <summary>Repeats; 0 repeats forever.</summary>
    public Tween Loop(int count = 0)
    {
        SetLoops(count);
        return this;
    }

    /// <summary>Plays forward, then backward, on alternate loops.</summary>
    public Tween PingPong(int loops = 0)
    {
        SetPingPong();
        SetLoops(loops);
        return this;
    }
}

/// <summary>Evaluates a scenario's animations each frame, in creation order.</summary>
internal sealed class Animator
{
    private readonly List<Animation> _active = new();

    public int Count => _active.Count;

    public void Add(Animation animation) => _active.Add(animation);

    public void Evaluate(double now)
    {
        if (_active.Count == 0) return;
        // A copy: completing an animation runs continuations that may start others.
        foreach (var animation in _active.ToArray()) {
            if (animation.Evaluate(now)) _active.Remove(animation);
        }
    }

    public void CancelAll(CancellationToken token)
    {
        var all = _active.ToArray();
        _active.Clear();
        foreach (var animation in all) animation.Cancel(token);
    }
}
