using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using BasicRenderer.Scripting.Interop;
using BasicRenderer.Scripting.Runtime;

namespace BasicRenderer.Scripting;

/// <summary>
/// Everything a running scenario can do: wait for frames or time, log, record checks, and (from M1) reach the scene,
/// camera, lights and settings. One context per scenario run.
/// </summary>
public sealed class ScenarioContext
{
    private readonly FrameScheduler _scheduler;
    private readonly List<CheckRecord> _checks = new();

    internal ScenarioContext(FrameScheduler scheduler, string scenarioName, CancellationToken cancellation)
    {
        _scheduler = scheduler;
        ScenarioName = scenarioName;
        Cancellation = cancellation;
    }

    public string ScenarioName { get; }

    /// <summary>Frames since the scenario started; 0 during its first frame.</summary>
    public long Frame => _scheduler.Frame;

    /// <summary>Scenario time in seconds: the sum of the host's frame times since the first frame.</summary>
    public double Time => _scheduler.Time;

    /// <summary>This frame's time step in seconds (0 during the first frame).</summary>
    public double DeltaTime => _scheduler.DeltaTime;

    /// <summary>Cancelled when the scenario is stopped, reloaded or times out.</summary>
    public CancellationToken Cancellation { get; }

    internal IReadOnlyList<CheckRecord> Checks => _checks;

    internal bool AnyCheckFailed { get; private set; }

    // --- Waiting -------------------------------------------------------------------------------------------------

    /// <summary>Resumes on the next frame.</summary>
    public Task NextFrame() => _scheduler.WaitFrames(1);

    /// <summary>Resumes after <paramref name="count"/> frames.</summary>
    public Task Frames(int count) => _scheduler.WaitFrames(count);

    /// <summary>Resumes on the first frame at least this many scenario seconds from now (always a later frame).</summary>
    public Task Seconds(double seconds) => _scheduler.WaitSeconds(seconds);

    /// <summary>Resumes on the first frame at least this much scenario time from now (always a later frame).</summary>
    public Task Wait(TimeSpan duration) => _scheduler.WaitSeconds(duration.TotalSeconds);

    /// <summary>
    /// Resumes on the first later frame where <paramref name="condition"/> holds. Throws TimeoutException when a
    /// timeout is given and passes first. The condition is evaluated once per frame, before other scenario code runs.
    /// </summary>
    public Task Until(Func<bool> condition, TimeSpan? timeout = null)
    {
        ArgumentNullException.ThrowIfNull(condition);
        return _scheduler.WaitUntil(condition, timeout?.TotalSeconds ?? 0);
    }

    // --- Logging -------------------------------------------------------------------------------------------------

    public void Log(string message) => Native.Log(NativeLogLevel.Info, Prefix(message));

    public void Warn(string message) => Native.Log(NativeLogLevel.Warn, Prefix(message));

    public void Error(string message) => Native.Log(NativeLogLevel.Error, Prefix(message));

    private string Prefix(string message) => $"[{ScenarioName} f{Frame}] {message}";

    // --- Outcome -------------------------------------------------------------------------------------------------

    /// <summary>Records a check. A failed check fails the scenario but lets it run on. Returns the condition.</summary>
    public bool Check(bool condition, string message,
        [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
    {
        _checks.Add(new CheckRecord(message, condition, Frame, Time, file, line));
        if (!condition) {
            AnyCheckFailed = true;
            Native.Log(NativeLogLevel.Error, Prefix($"check failed: {message} ({System.IO.Path.GetFileName(file)}:{line})"));
        }
        return condition;
    }

    /// <summary>Records a check and, if it fails, ends the scenario immediately.</summary>
    public void Require([DoesNotReturnIf(false)] bool condition, string message,
        [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
    {
        if (!Check(condition, message, file, line)) throw new ScenarioFailedException($"requirement failed: {message}");
    }

    /// <summary>Ends the scenario as failed.</summary>
    [DoesNotReturn]
    public void Fail(string message) => throw new ScenarioFailedException(message);

    /// <summary>Asks the host application to exit with this code when the scenario ends.</summary>
    public void RequestExit(int exitCode = 0) => Native.RequestExit(exitCode);
}

internal sealed record CheckRecord(string Message, bool Passed, long Frame, double Time, string File, int Line);
