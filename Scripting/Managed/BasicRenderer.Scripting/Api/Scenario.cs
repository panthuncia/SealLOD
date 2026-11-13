using System;
using System.Threading.Tasks;

namespace BasicRenderer.Scripting;

/// <summary>
/// A scripted scenario. Derive from this, optionally mark the class with <see cref="ScenarioAttribute"/>, and
/// implement <see cref="Run"/>. The scenario passes when Run returns without a failed check, and fails when it throws,
/// a check fails, or it runs past its timeout.
/// </summary>
/// <remarks>
/// Run executes on the host's main thread, one slice per frame: every <c>await</c> on the context (NextFrame, Frames,
/// Seconds, Until) resumes on a later frame, on the same thread. Scenario code needs no locks. Do not block (Thread.Sleep,
/// Task.Wait, .Result): the frame cannot advance while Run holds the thread.
/// </remarks>
public abstract class Scenario
{
    public abstract Task Run(ScenarioContext ctx);
}

/// <summary>Names a scenario and sets its limits. Without it, the scenario's name is its class name.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class ScenarioAttribute(string name) : Attribute
{
    public string Name { get; } = name;

    /// <summary>Fails the scenario after this many seconds of scenario time. 0 means no limit.</summary>
    public double TimeoutSeconds { get; init; } = 120;

    /// <summary>A one-line description, shown by listings and in results.</summary>
    public string? Description { get; init; }
}

/// <summary>Scenario lifecycle as the host sees it (BR_SCRIPT_STATE_*).</summary>
public enum ScenarioState
{
    Idle = 0,
    Compiling = 1,
    Running = 2,
    Passed = 3,
    Failed = 4,
    CompileError = 5,
}

/// <summary>Thrown by <see cref="ScenarioContext.Fail"/>: ends the scenario as failed with this message.</summary>
public sealed class ScenarioFailedException(string message) : Exception(message);

/// <summary>
/// A host API call failed: a stale entity, a missing setting or file, a type mismatch. <see cref="Status"/> is the
/// ABI's BR_SCRIPT_E_* code; the message is the host's explanation.
/// </summary>
public sealed class ScriptApiException(int status, string operation, string detail)
    : Exception(string.IsNullOrEmpty(detail) ? $"{operation} failed ({status})" : $"{operation} failed: {detail}")
{
    public int Status { get; } = status;

    /// <summary>The entity was destroyed, or belongs to an earlier run.</summary>
    public bool IsStaleHandle => Status == -2;
    public bool IsNotFound => Status == -3;
    public bool IsTypeMismatch => Status == -4;
    public bool IsUnsupported => Status == -5;
}
