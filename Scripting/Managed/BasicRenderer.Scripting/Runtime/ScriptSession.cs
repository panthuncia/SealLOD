using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Threading.Tasks;
using BasicRenderer.Scripting.Interop;

namespace BasicRenderer.Scripting.Runtime;

/// <summary>
/// One scenario at a time: compile (off the main thread), load into a collectible context, run, report, unload.
/// Every public method is called on the host's main thread and never throws.
/// </summary>
internal sealed class ScriptSession
{
    private Task<CompileOutput>? _compile;
    private PendingRun? _pending;
    private ScenarioRunner? _runner;
    private ScenarioLoadContext? _loadContext;
    private int _runCount;

    public ScenarioState State { get; private set; } = ScenarioState.Idle;

    /// <summary>The result document of the last finished scenario.</summary>
    public string? LastResultJson { get; private set; }

    public bool IsBusy => State is ScenarioState.Compiling or ScenarioState.Running;

    /// <summary>Starts compiling. Returns AbiConstants.Busy while a scenario is compiling or running.</summary>
    public int Run(string sourcePath, string? scenarioName, string? optionsJson)
    {
        if (IsBusy) return AbiConstants.Busy;
        IReadOnlyList<string> sources;
        RunOptions options;
        try {
            options = RunOptions.Parse(optionsJson);
            sources = ScenarioCompiler.CollectSources(sourcePath);
        } catch (Exception e) {
            FinishWithoutRunning(sourcePath, scenarioName, e.Message);
            return AbiConstants.Ok;
        }
        if (sources.Count == 0) {
            FinishWithoutRunning(sourcePath, scenarioName, $"no .cs files under {sourcePath}");
            return AbiConstants.Ok;
        }
        _runCount++;
        _pending = new PendingRun(sourcePath, string.IsNullOrWhiteSpace(scenarioName) ? null : scenarioName, options);
        var assemblyName = $"Scenario{_runCount}_{Path.GetFileNameWithoutExtension(sourcePath)}";
        Native.Log(NativeLogLevel.Info, $"compiling {sources.Count} scenario file(s) from {sourcePath}");
        _compile = Task.Run(() => ScenarioCompiler.Compile(sources, assemblyName));
        SetState(ScenarioState.Compiling, null);
        return AbiConstants.Ok;
    }

    public void Tick(ulong hostFrameIndex, double deltaSeconds)
    {
        try {
            if (State == ScenarioState.Compiling && _compile is { IsCompleted: true }) {
                StartCompiled();
            }
            if (State == ScenarioState.Running && _runner != null) {
                _runner.Tick(deltaSeconds);
                if (_runner.IsFinished) FinishRunner();
            }
        } catch (Exception e) {
            // A failure of the runtime itself, not of scenario code (that is caught by the runner).
            Native.Log(NativeLogLevel.Error, $"scripting runtime error: {e}");
            if (_runner != null) {
                _runner.Stop($"scripting runtime error: {e.Message}");
                FinishRunner();
            } else if (IsBusy) {
                FinishWithoutRunning(_pending?.Source ?? "", _pending?.ScenarioName, $"scripting runtime error: {e.Message}");
            }
        }
    }

    public void Stop(string reason)
    {
        try {
            if (State == ScenarioState.Compiling) {
                // The compile finishes in the background and is discarded.
                FinishWithoutRunning(_pending?.Source ?? "", _pending?.ScenarioName, reason, ScenarioState.Failed);
            } else if (_runner != null) {
                _runner.Stop(reason);
                FinishRunner();
            }
        } catch (Exception e) {
            Native.Log(NativeLogLevel.Error, $"scripting runtime error while stopping: {e}");
        }
    }

    private void StartCompiled()
    {
        var pending = _pending!;
        CompileOutput output;
        try {
            output = _compile!.GetAwaiter().GetResult();
        } catch (Exception e) {
            FinishWithoutRunning(pending.Source, pending.ScenarioName, $"compiler failure: {e.Message}");
            return;
        } finally {
            _compile = null;
        }
        foreach (var diagnostic in output.Diagnostics) {
            Native.Log(diagnostic.Severity == "error" ? NativeLogLevel.Error : NativeLogLevel.Warn, diagnostic.ToString());
        }
        if (!output.Succeeded) {
            int errors = output.Diagnostics.Count(d => d.Severity == "error");
            FinishWithoutRunning(pending.Source, pending.ScenarioName, $"{errors} compile error(s)", ScenarioState.CompileError, output.Diagnostics);
            return;
        }

        var loadContext = new ScenarioLoadContext();
        Assembly assembly;
        using (var pe = new MemoryStream(output.Assembly!))
        using (var pdb = new MemoryStream(output.Pdb!)) {
            assembly = loadContext.LoadFromStream(pe, pdb);
        }

        if (!TrySelectScenario(assembly, pending.ScenarioName, out var type, out var attribute, out var selectError)) {
            Unload(loadContext);
            FinishWithoutRunning(pending.Source, pending.ScenarioName, selectError, ScenarioState.CompileError, output.Diagnostics);
            return;
        }

        var name = attribute?.Name ?? type!.Name;
        Scenario instance;
        try {
            instance = (Scenario)Activator.CreateInstance(type!)!;
        } catch (Exception e) {
            Unload(loadContext);
            var failed = new ScenarioResult { Scenario = name, Source = pending.Source, Status = ScenarioResult.StatusName(ScenarioState.Failed) };
            failed.Error = ErrorResult.FromException(e, assembly);
            Report(ScenarioState.Failed, failed);
            return;
        }

        double timeout = pending.Options.TimeoutSeconds ?? attribute?.TimeoutSeconds ?? 120;
        _loadContext = loadContext;
        _runner = new ScenarioRunner(instance, name, pending.Source, timeout);
        _runner.Result.Diagnostics = output.Diagnostics;
        Native.Log(NativeLogLevel.Info, $"running scenario '{name}'" + (timeout > 0 ? $" (timeout {timeout:0.###} s)" : ""));
        SetState(ScenarioState.Running, null);
    }

    private static bool TrySelectScenario(Assembly assembly, string? requested, out Type? type, out ScenarioAttribute? attribute, out string error)
    {
        var candidates = assembly.GetTypes()
            .Where(t => !t.IsAbstract && typeof(Scenario).IsAssignableFrom(t))
            .Select(t => (Type: t, Attribute: t.GetCustomAttribute<ScenarioAttribute>()))
            .ToList();
        string NameOf((Type Type, ScenarioAttribute? Attribute) c) => c.Attribute?.Name ?? c.Type.Name;

        type = null;
        attribute = null;
        error = "";
        if (candidates.Count == 0) {
            error = "no scenario found: derive a class from BasicRenderer.Scripting.Scenario";
            return false;
        }
        var matches = requested == null
            ? candidates
            : candidates.Where(c => string.Equals(NameOf(c), requested, StringComparison.OrdinalIgnoreCase)).ToList();
        if (matches.Count != 1) {
            var names = string.Join(", ", candidates.Select(NameOf).Order());
            error = requested == null
                ? $"the sources define {candidates.Count} scenarios ({names}); choose one with --scenario"
                : $"no scenario named '{requested}' (available: {names})";
            return false;
        }
        (type, attribute) = matches[0];
        if (type.GetConstructor(Type.EmptyTypes) == null) {
            error = $"scenario {type.Name} needs a public parameterless constructor";
            return false;
        }
        return true;
    }

    private void FinishRunner()
    {
        var runner = _runner!;
        _runner = null;
        Report(runner.State, runner.Result);
        if (_loadContext != null) {
            Unload(_loadContext);
            _loadContext = null;
        }
    }

    private void FinishWithoutRunning(string source, string? scenarioName, string message, ScenarioState state = ScenarioState.CompileError,
        List<CompileDiagnostic>? diagnostics = null)
    {
        _pending = null;
        _compile = null;
        Native.Log(NativeLogLevel.Error, message);
        var result = new ScenarioResult
        {
            Scenario = scenarioName ?? "",
            Source = source,
            Status = ScenarioResult.StatusName(state),
            Error = ErrorResult.FromMessage(message),
            Diagnostics = diagnostics ?? new(),
        };
        Report(state, result);
    }

    private void Report(ScenarioState state, ScenarioResult result)
    {
        LastResultJson = result.ToJson();
        var summary = result.Error == null ? result.Status : $"{result.Status}: {result.Error.Message}";
        Native.Log(state == ScenarioState.Passed ? NativeLogLevel.Info : NativeLogLevel.Error,
            $"scenario '{result.Scenario}' {summary} ({result.Frames} frames, {result.ScenarioSeconds:0.###} s)");
        SetState(state, LastResultJson);
    }

    private void SetState(ScenarioState state, string? resultJson)
    {
        State = state;
        Native.ReportState(state, resultJson);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Unload(ScenarioLoadContext context) => context.Unload();

    private sealed record PendingRun(string Source, string? ScenarioName, RunOptions Options);

    internal sealed record RunOptions(double? TimeoutSeconds)
    {
        public static RunOptions Parse(string? json)
        {
            if (string.IsNullOrWhiteSpace(json)) return new RunOptions((double?)null);
            using var document = JsonDocument.Parse(json);
            double? timeout = document.RootElement.TryGetProperty("timeout_seconds", out var t) ? t.GetDouble() : null;
            return new RunOptions(timeout);
        }
    }
}
