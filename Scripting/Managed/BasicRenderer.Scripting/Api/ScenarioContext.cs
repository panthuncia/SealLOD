using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using BasicRenderer.Scripting.Interop;
using BasicRenderer.Scripting.Runtime;

namespace BasicRenderer.Scripting;

/// <summary>
/// Everything a running scenario can do: wait for frames, time or conditions; reach the scene, camera, settings and
/// control commands; animate; log; record checks. One context per scenario run.
/// </summary>
public sealed class ScenarioContext
{
    private readonly FrameScheduler _scheduler;
    private readonly List<CheckRecord> _checks = new();
    private Camera? _camera;

    internal ScenarioContext(FrameScheduler scheduler, string scenarioName, string source, CancellationToken cancellation)
    {
        _scheduler = scheduler;
        ScenarioName = scenarioName;
        Cancellation = cancellation;
        ScenarioDirectory = Directory.Exists(source) ? Path.GetFullPath(source)
            : Path.GetDirectoryName(Path.GetFullPath(string.IsNullOrEmpty(source) ? "." : source)) ?? Environment.CurrentDirectory;
        Animator = new Animator();
        Scene = new SceneApi(this);
        Settings = new SettingsApi();
        Commands = new CommandsApi(this);
        Capture = new CaptureApi(this);
    }

    public string ScenarioName { get; }

    /// <summary>The folder holding the scenario's source; relative model paths are looked up here first.</summary>
    public string ScenarioDirectory { get; }

    /// <summary>Frames since the scenario started; 0 during its first frame.</summary>
    public long Frame => _scheduler.Frame;

    /// <summary>Scenario time in seconds: the sum of the host's frame times since the first frame.</summary>
    public double Time => _scheduler.Time;

    /// <summary>This frame's time step in seconds (0 during the first frame).</summary>
    public double DeltaTime => _scheduler.DeltaTime;

    /// <summary>Cancelled when the scenario is stopped, reloaded or times out.</summary>
    public CancellationToken Cancellation { get; }

    public SceneApi Scene { get; }

    /// <summary>The primary camera. Moving it takes it from the fly camera until the scenario ends.</summary>
    public Camera Camera => _camera ??= new Camera(this, Native.Api.GetCamera());

    public SettingsApi Settings { get; }

    public CommandsApi Commands { get; }

    public CaptureApi Capture { get; }

    internal Animator Animator { get; }

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

    /// <summary>
    /// Resumes once the renderer has settled: nothing streaming, uploading or compiling, no model still loading, and
    /// the workload (resident geometry, full-resolution textures, draw records) unchanged for <paramref name="frames"/>
    /// frames in a row. The demo's sampling harness waits for the same thing. Throws TimeoutException after
    /// <paramref name="timeout"/> (default 120 s of scenario time).
    /// </summary>
    public Task WaitUntilStable(int frames = 30, TimeSpan? timeout = null)
    {
        int stableFrames = 0;
        ulong? signature = null;
        return Until(() =>
        {
            var status = Scene.Status;
            if (status.NothingPending && status.WorkloadSignature == signature) stableFrames++;
            else stableFrames = 0;
            signature = status.WorkloadSignature;
            return stableFrames >= frames;
        }, timeout ?? TimeSpan.FromSeconds(120));
    }

    // --- Animation -----------------------------------------------------------------------------------------------

    /// <summary>Animates a number from <paramref name="from"/> to <paramref name="to"/>, handing each frame's value to <paramref name="apply"/>.</summary>
    public Tween Tween(double from, double to, double seconds, Action<double> apply, Easing? ease = null) => new(this, from, to, seconds, apply, ease);

    /// <summary>Animates an entity (same as <c>entity.Animate()</c>).</summary>
    public AnimationBuilder Animate(Entity entity) => entity.Animate();

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
            Native.Log(NativeLogLevel.Error, Prefix($"check failed: {message} ({Path.GetFileName(file)}:{line})"));
        }
        return condition;
    }

    /// <summary>Checks that two vectors agree within <paramref name="tolerance"/> on every component.</summary>
    public bool CheckNear(Vec3 actual, Vec3 expected, float tolerance, string message,
        [CallerFilePath] string file = "", [CallerLineNumber] int line = 0) =>
        Check(actual.ApproximatelyEquals(expected, tolerance), $"{message}: {actual} vs {expected} (±{tolerance})", file, line);

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

    // --- Internals -----------------------------------------------------------------------------------------------

    /// <summary>A relative path beside the scenario's source if it exists there, else from the working directory.</summary>
    internal string ResolvePath(string path)
    {
        if (Path.IsPathRooted(path)) return path;
        var beside = Path.GetFullPath(Path.Combine(ScenarioDirectory, path));
        if (File.Exists(beside) || Directory.Exists(beside)) return beside;
        var working = Path.GetFullPath(path);
        return File.Exists(working) || Directory.Exists(working) ? working : beside;
    }

    /// <summary>The scenario has ended: stop its animations and put back what it changed.</summary>
    internal void Finish()
    {
        Animator.CancelAll(Cancellation);
        Settings.Restore();
        try {
            _camera?.ReleaseControl();
        } catch (ScriptApiException) {
            // The host restores the fly camera itself.
        }
    }
}

internal sealed record CheckRecord(string Message, bool Passed, long Frame, double Time, string File, int Line);
