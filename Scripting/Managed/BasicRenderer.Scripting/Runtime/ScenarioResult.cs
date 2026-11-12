using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace BasicRenderer.Scripting.Runtime;

/// <summary>The document a scenario run produces; hosts write it to --script-result. snake_case like the pipe commands.</summary>
internal sealed class ScenarioResult
{
    public string Scenario { get; set; } = "";
    public string Source { get; set; } = "";
    /// <summary>passed, failed or compile_error.</summary>
    public string Status { get; set; } = "";
    public long Frames { get; set; }
    public double ScenarioSeconds { get; set; }
    public double WallSeconds { get; set; }
    public List<CheckResult> Checks { get; set; } = new();
    public ErrorResult? Error { get; set; }
    public List<CompileDiagnostic> Diagnostics { get; set; } = new();

    private static readonly JsonSerializerOptions s_options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        WriteIndented = true,
    };

    public string ToJson() => JsonSerializer.Serialize(this, s_options);

    public static string StatusName(ScenarioState state) => state switch
    {
        ScenarioState.Passed => "passed",
        ScenarioState.Failed => "failed",
        ScenarioState.CompileError => "compile_error",
        _ => state.ToString().ToLowerInvariant(),
    };
}

internal sealed record CheckResult(string Message, bool Passed, long Frame, double Time, string File, int Line);

internal sealed record CompileDiagnostic(string File, int Line, int Column, string Id, string Severity, string Message)
{
    public override string ToString() => $"{File}({Line},{Column}): {Severity} {Id}: {Message}";
}

internal sealed class ErrorResult
{
    public string Type { get; set; } = "";
    public string Message { get; set; } = "";
    public string? File { get; set; }
    public int? Line { get; set; }
    public string? Stack { get; set; }

    public static ErrorResult FromMessage(string message) => new() { Type = "message", Message = message };

    /// <summary>Points at the innermost frame inside the scenario's own code when there is one.</summary>
    public static ErrorResult FromException(Exception exception, Assembly? scenarioAssembly)
    {
        exception = Unwrap(exception);
        var result = new ErrorResult
        {
            Type = exception.GetType().FullName ?? exception.GetType().Name,
            Message = exception.Message,
            Stack = exception.ToString(),
        };
        var frames = new StackTrace(exception, fNeedFileInfo: true).GetFrames();
        var located = frames.FirstOrDefault(f => f.GetFileName() != null && f.GetMethod()?.DeclaringType?.Assembly == scenarioAssembly)
            ?? frames.FirstOrDefault(f => f.GetFileName() != null);
        if (located != null) {
            result.File = located.GetFileName();
            result.Line = located.GetFileLineNumber();
        }
        return result;
    }

    private static Exception Unwrap(Exception exception)
    {
        while (exception is AggregateException { InnerExceptions.Count: 1 } aggregate) exception = aggregate.InnerExceptions[0];
        while (exception is TargetInvocationException { InnerException: { } inner }) exception = inner;
        return exception;
    }
}
