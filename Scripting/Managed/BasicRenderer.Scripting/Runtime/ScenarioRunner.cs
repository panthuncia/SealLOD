using System;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using BasicRenderer.Scripting.Interop;

namespace BasicRenderer.Scripting.Runtime;

/// <summary>
/// Drives one scenario instance: starts Run on the first tick, resumes it once per tick, and decides its outcome.
/// Owns no native state; the session around it reports results and unloads code.
/// </summary>
internal sealed class ScenarioRunner
{
    private readonly Scenario _scenario;
    private readonly Assembly? _scenarioAssembly;
    private readonly double _timeoutSeconds;
    private readonly CancellationTokenSource _cancellation = new();
    private readonly FrameScheduler _scheduler;
    private readonly Stopwatch _wall = new();
    private Task? _run;

    public ScenarioRunner(Scenario scenario, string name, string source, double timeoutSeconds)
    {
        _scenario = scenario;
        _scenarioAssembly = scenario.GetType().Assembly;
        _timeoutSeconds = timeoutSeconds;
        _scheduler = new FrameScheduler(_cancellation.Token);
        Context = new ScenarioContext(_scheduler, name, source, _cancellation.Token);
        Result = new ScenarioResult { Scenario = name, Source = source };
    }

    public ScenarioContext Context { get; }

    public ScenarioState State { get; private set; } = ScenarioState.Running;

    public bool IsFinished => State is ScenarioState.Passed or ScenarioState.Failed;

    /// <summary>Filled in progressively; complete once <see cref="IsFinished"/>.</summary>
    public ScenarioResult Result { get; }

    internal FrameScheduler Scheduler => _scheduler;

    /// <summary>One host frame. The first call runs Run up to its first await.</summary>
    public void Tick(double deltaSeconds)
    {
        if (IsFinished) return;
        RunOnScheduler(() =>
        {
            if (_run == null) {
                _wall.Start();
                _run = Start();
            } else {
                _scheduler.Advance(deltaSeconds);
                // Animations first, so code resuming this frame sees this frame's animated state.
                Context.Animator.Evaluate(_scheduler.Time);
                _scheduler.ResumeDue();
            }
            _scheduler.Drain();
            // Again for animations started this frame: they apply their starting state now, not a frame late.
            Context.Animator.Evaluate(_scheduler.Time);
        });
        if (_run!.IsCompleted) {
            Finish(_run);
            return;
        }
        if (_timeoutSeconds > 0 && _scheduler.Time + 1e-9 >= _timeoutSeconds) {
            Terminate($"timed out after {_timeoutSeconds:0.###} s of scenario time (frame {_scheduler.Frame})");
        }
    }

    /// <summary>Cancels the scenario: pending awaits throw OperationCanceledException and it ends Failed.</summary>
    public void Stop(string reason)
    {
        if (IsFinished) return;
        Terminate(reason);
    }

    private Task Start()
    {
        try {
            return _scenario.Run(Context) ?? Task.CompletedTask;
        } catch (Exception e) {
            return Task.FromException(e);
        }
    }

    private void Terminate(string reason)
    {
        _cancellation.Cancel();
        // Let the scenario's finally blocks and cancellation handlers run, on its own context.
        try {
            RunOnScheduler(() =>
            {
                _scheduler.CancelAll();
                _scheduler.Drain();
            });
        } catch (Exception e) {
            Native.Log(NativeLogLevel.Warn, $"[{Context.ScenarioName}] error while cancelling: {e.Message}");
        }
        Complete(ScenarioState.Failed, ErrorResult.FromMessage(reason));
    }

    private void Finish(Task run)
    {
        if (run.IsFaulted) {
            Complete(ScenarioState.Failed, ErrorResult.FromException(run.Exception!, _scenarioAssembly));
        } else if (run.IsCanceled) {
            Complete(ScenarioState.Failed, ErrorResult.FromMessage("the scenario was cancelled"));
        } else if (Context.AnyCheckFailed) {
            int failed = Context.Checks.Count(c => !c.Passed);
            Complete(ScenarioState.Failed, ErrorResult.FromMessage($"{failed} check(s) failed"));
        } else {
            Complete(ScenarioState.Passed, null);
        }
    }

    private void Complete(ScenarioState state, ErrorResult? error)
    {
        _wall.Stop();
        try {
            RunOnScheduler(Context.Finish);
        } catch (Exception e) {
            Native.Log(NativeLogLevel.Warn, $"[{Context.ScenarioName}] error while finishing: {e.Message}");
        }
        State = state;
        Result.Status = ScenarioResult.StatusName(state);
        Result.Frames = _scheduler.Frame + (_run == null ? 0 : 1);
        Result.ScenarioSeconds = _scheduler.Time;
        Result.WallSeconds = _wall.Elapsed.TotalSeconds;
        Result.Checks = Context.Checks.Select(c => new CheckResult(c.Message, c.Passed, c.Frame, c.Time, c.File, c.Line)).ToList();
        Result.Error = error;
    }

    private void RunOnScheduler(Action action)
    {
        var previous = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(_scheduler);
        try {
            action();
        } finally {
            SynchronizationContext.SetSynchronizationContext(previous);
        }
    }
}
