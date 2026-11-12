using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace BasicRenderer.Scripting.Runtime;

/// <summary>
/// Runs a scenario's async code one frame at a time on the host's main thread.
/// </summary>
/// <remarks>
/// It is the SynchronizationContext installed while a scenario's tick runs, so every <c>await</c> inside the scenario
/// resumes here, never on a pool thread. Frame and time waits complete only in <see cref="ResumeDue"/>, at the start of
/// a later tick, in the order they were registered. Work posted from other threads (a continuation of a Task.Run, for
/// example) runs at the next <see cref="Drain"/>.
/// </remarks>
internal sealed class FrameScheduler : SynchronizationContext
{
    private const int DrainLimit = 1_000_000;

    private readonly object _postedLock = new();
    private Queue<(SendOrPostCallback Callback, object? State)> _posted = new();
    private Queue<(SendOrPostCallback Callback, object? State)> _draining = new();
    private readonly List<Waiter> _waiters = new();
    private readonly CancellationToken _cancellation;

    public FrameScheduler(CancellationToken cancellation) => _cancellation = cancellation;

    /// <summary>Frames since the scenario started: 0 during its first frame.</summary>
    public long Frame { get; private set; }

    /// <summary>Scenario seconds: the sum of the host's frame times since the first frame, 0 during it.</summary>
    public double Time { get; private set; }

    /// <summary>The host's time step for the current frame (0 during the first frame).</summary>
    public double DeltaTime { get; private set; }

    public int PendingWaiters => _waiters.Count;

    public override void Post(SendOrPostCallback d, object? state)
    {
        lock (_postedLock) {
            _posted.Enqueue((d, state));
        }
    }

    public override void Send(SendOrPostCallback d, object? state)
    {
        if (Current == this) {
            d(state);
            return;
        }
        throw new NotSupportedException("Scenario code runs on the host's main thread; Send from another thread would deadlock the frame.");
    }

    public override SynchronizationContext CreateCopy() => this;

    /// <summary>Starts the next frame.</summary>
    public void Advance(double deltaSeconds)
    {
        Frame += 1;
        DeltaTime = deltaSeconds;
        Time += deltaSeconds;
    }

    /// <summary>Completes every wait that is now due, oldest first. Continuations run inline, on this thread.</summary>
    public void ResumeDue()
    {
        if (_waiters.Count == 0) return;
        // Snapshot first: a resumed continuation may register new waiters, which wait for a later frame.
        List<(Waiter Waiter, Exception? Error)>? due = null;
        for (int i = 0; i < _waiters.Count; i++) {
            var waiter = _waiters[i];
            Exception? error = null;
            bool ready;
            try {
                ready = waiter.IsDue(this);
            } catch (Exception e) {
                ready = true;
                error = e;
            }
            if (!ready) continue;
            (due ??= new()).Add((waiter, error));
            _waiters.RemoveAt(i);
            i--;
        }
        if (due == null) return;
        foreach (var (waiter, error) in due) {
            if (error != null) waiter.Completion.TrySetException(error);
            else waiter.Complete();
        }
    }

    /// <summary>Runs posted work until none is left.</summary>
    public void Drain()
    {
        for (int ran = 0; ; ) {
            lock (_postedLock) {
                if (_posted.Count == 0) return;
                (_posted, _draining) = (_draining, _posted);
            }
            while (_draining.Count > 0) {
                var (callback, state) = _draining.Dequeue();
                callback(state);
                if (++ran > DrainLimit) {
                    throw new InvalidOperationException(
                        "Scenario code posted work without ever waiting for a frame. Await ctx.NextFrame() inside loops.");
                }
            }
        }
    }

    /// <summary>Cancels every pending wait. Their awaits throw OperationCanceledException.</summary>
    public void CancelAll()
    {
        var waiters = _waiters.ToArray();
        _waiters.Clear();
        foreach (var waiter in waiters) waiter.Completion.TrySetCanceled(_cancellation);
    }

    public Task WaitFrames(long frames)
    {
        if (frames <= 0) return Task.CompletedTask;
        return Register(new Waiter(this) { TargetFrame = Frame + frames });
    }

    public Task WaitSeconds(double seconds)
    {
        // A zero or negative wait still yields to the next frame, so loops of Seconds(0) advance.
        return Register(new Waiter(this) { TargetFrame = Frame + 1, TargetTime = Time + Math.Max(seconds, 0.0) });
    }

    public Task WaitUntil(Func<bool> condition, double timeoutSeconds)
    {
        double deadline = timeoutSeconds > 0 ? Time + timeoutSeconds : double.PositiveInfinity;
        return Register(new Waiter(this) { TargetFrame = Frame + 1, Condition = condition, Deadline = deadline, TimeoutSeconds = timeoutSeconds });
    }

    private Task Register(Waiter waiter)
    {
        if (_cancellation.IsCancellationRequested) return Task.FromCanceled(_cancellation);
        _waiters.Add(waiter);
        return waiter.Completion.Task;
    }

    private sealed class Waiter(FrameScheduler owner)
    {
        // Continuations run inline when completed on the scheduler's own thread and context; that is the point.
        public readonly TaskCompletionSource Completion = new(TaskCreationOptions.None);
        public long TargetFrame;
        public double TargetTime = double.NegativeInfinity;
        public Func<bool>? Condition;
        public double Deadline = double.PositiveInfinity;
        public double TimeoutSeconds;
        private bool _timedOut;

        public bool IsDue(FrameScheduler scheduler)
        {
            if (scheduler.Frame < TargetFrame) return false;
            // Time comparisons get a small tolerance so 60 frames of 1/60 s reliably reach one second.
            if (scheduler.Time + 1e-9 < TargetTime) return false;
            if (Condition == null) return true;
            if (Condition()) return true;
            if (scheduler.Time + 1e-9 >= Deadline) {
                _timedOut = true;
                return true;
            }
            return false;
        }

        public void Complete()
        {
            if (_timedOut) Completion.TrySetException(new TimeoutException($"condition not met within {TimeoutSeconds:0.###} s (frame {owner.Frame})"));
            else Completion.TrySetResult();
        }
    }
}
