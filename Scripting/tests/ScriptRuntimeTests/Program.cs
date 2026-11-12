// Managed runtime tests. No test framework: each test is a method, a failed expectation throws, and the exit code is
// the number of failed tests. Usage: ScriptRuntimeTests <fixtures dir> <scenarios dir>
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using BasicRenderer.Scripting;
using BasicRenderer.Scripting.Interop;
using BasicRenderer.Scripting.Runtime;

internal static class Program
{
    private static string s_fixtures = "";
    private static string s_scenarios = "";

    private static int Main(string[] args)
    {
        if (args.Length < 2) {
            Console.Error.WriteLine("usage: ScriptRuntimeTests <fixtures dir> <scenarios dir>");
            return 100;
        }
        s_fixtures = Path.GetFullPath(args[0]);
        s_scenarios = Path.GetFullPath(args[1]);
        Native.DetachedLogSink = (_, _) => { };

        var tests = new (string Name, Action Body)[]
        {
            ("frames resume in registration order", FramesResumeInOrder),
            ("seconds wait for scenario time, always a later frame", SecondsWaitForTime),
            ("until polls once per frame and times out", UntilPollsAndTimesOut),
            ("continuations stay on the ticking thread", ContinuationsStayOnThread),
            ("passing scenario reports passed", PassingScenario),
            ("failed check fails but runs to the end", FailedCheckRunsToEnd),
            ("exception fails with its location", ExceptionLocated),
            ("timeout cancels pending waits", TimeoutCancels),
            ("stop ends a running scenario", StopEndsScenario),
            ("compile errors carry file and line", CompileErrorLocated),
            ("ambiguous sources need a scenario name", AmbiguousNeedsName),
            ("example scenarios compile", ExamplesCompile),
        };
        int failed = 0;
        foreach (var (name, body) in tests) {
            try {
                body();
                Console.WriteLine($"PASS  {name}");
            } catch (Exception e) {
                failed++;
                Console.WriteLine($"FAIL  {name}\n      {e.Message}");
            }
        }
        Console.WriteLine(failed == 0 ? $"all {tests.Length} tests passed" : $"{failed} of {tests.Length} tests failed");
        return failed;
    }

    // --- Scheduler -------------------------------------------------------------------------------------------------

    private sealed class Probe(Func<ScenarioContext, Task> body) : Scenario
    {
        public override Task Run(ScenarioContext ctx) => body(ctx);
    }

    private static ScenarioRunner Runner(Func<ScenarioContext, Task> body, double timeout = 0) =>
        new(new Probe(body), "probe", "<test>", timeout);

    private static void TickUntilFinished(ScenarioRunner runner, double dt = 1.0 / 60, int maxFrames = 10_000)
    {
        for (int i = 0; i < maxFrames && !runner.IsFinished; i++) runner.Tick(dt);
        Expect(runner.IsFinished, $"scenario did not finish within {maxFrames} frames");
    }

    private static void FramesResumeInOrder()
    {
        var order = new List<string>();
        var runner = Runner(async ctx =>
        {
            async Task Worker(string name, int frames)
            {
                await ctx.Frames(frames);
                order.Add($"{name}@{ctx.Frame}");
            }
            var a = Worker("a", 2);
            var b = Worker("b", 1);
            var c = Worker("c", 2);
            await Task.WhenAll(a, b, c);
        });
        TickUntilFinished(runner);
        Expect(runner.State == ScenarioState.Passed, runner.Result.Error?.Message ?? "");
        Equal("b@1,a@2,c@2", string.Join(",", order));
    }

    private static void SecondsWaitForTime()
    {
        long resumedFrame = -1;
        double resumedTime = -1;
        long zeroWaitFrame = -1;
        var runner = Runner(async ctx =>
        {
            await ctx.Seconds(0);
            zeroWaitFrame = ctx.Frame;
            await ctx.Seconds(1);
            resumedFrame = ctx.Frame;
            resumedTime = ctx.Time;
        });
        TickUntilFinished(runner, dt: 0.25);
        Equal(1L, zeroWaitFrame);
        Equal(5L, resumedFrame);   // 1 + four 0.25 s frames
        Expect(Math.Abs(resumedTime - 1.25) < 1e-9, $"time {resumedTime}");
    }

    private static void UntilPollsAndTimesOut()
    {
        int polls = 0;
        bool timedOut = false;
        var runner = Runner(async ctx =>
        {
            await ctx.Until(() => ++polls == 3);
            Equal(3L, ctx.Frame);
            try {
                await ctx.Until(() => false, TimeSpan.FromSeconds(0.1));
            } catch (TimeoutException) {
                timedOut = true;
            }
        });
        TickUntilFinished(runner, dt: 0.05);
        Equal(3, polls);
        Expect(timedOut, "Until with a timeout did not throw TimeoutException");
    }

    private static void ContinuationsStayOnThread()
    {
        int thread = Environment.CurrentManagedThreadId;
        var seen = new List<int>();
        var runner = Runner(async ctx =>
        {
            seen.Add(Environment.CurrentManagedThreadId);
            await Task.Run(() => Thread.Sleep(5));   // completes on the pool; must resume on the ticking thread
            seen.Add(Environment.CurrentManagedThreadId);
            await ctx.NextFrame();
            seen.Add(Environment.CurrentManagedThreadId);
        });
        for (int i = 0; i < 1000 && !runner.IsFinished; i++) {
            runner.Tick(1.0 / 60);
            Thread.Sleep(1);
        }
        Expect(runner.IsFinished, "did not finish");
        Expect(seen.Count == 3 && seen.All(t => t == thread), $"continuation threads {string.Join(",", seen)}, ticking thread {thread}");
    }

    // --- Lifecycle through the session (compile, load, run) -------------------------------------------------------

    private static JsonElement RunSession(string source, string? scenario = null, string? options = null, Action<ScriptSession>? midRun = null)
    {
        var session = new ScriptSession();
        Equal(AbiConstants.Ok, session.Run(source, scenario, options));
        var clock = Stopwatch.StartNew();
        ulong frame = 0;
        while (session.IsBusy) {
            Expect(clock.Elapsed < TimeSpan.FromSeconds(60), "session did not finish within 60 s");
            session.Tick(frame++, 1.0 / 60);
            if (session.State == ScenarioState.Running && midRun != null) {
                midRun(session);
                midRun = null;
            }
            if (session.State == ScenarioState.Compiling) Thread.Sleep(2);
        }
        Expect(session.LastResultJson != null, "no result document");
        return JsonDocument.Parse(session.LastResultJson!).RootElement.Clone();
    }

    private static string Fixture(string name) => Path.Combine(s_fixtures, name);

    private static void PassingScenario()
    {
        var result = RunSession(Path.Combine(s_scenarios, "Hello.cs"));
        Equal("passed", result.GetProperty("status").GetString());
        Equal("hello", result.GetProperty("scenario").GetString());
        Equal(2, result.GetProperty("checks").GetArrayLength());
    }

    private static void FailedCheckRunsToEnd()
    {
        var result = RunSession(Fixture("FailedCheck.cs"));
        Equal("failed", result.GetProperty("status").GetString());
        var checks = result.GetProperty("checks");
        Equal(2, checks.GetArrayLength());
        Expect(!checks[1].GetProperty("passed").GetBoolean(), "second check should have failed");
        Equal(9, checks[1].GetProperty("line").GetInt32());
    }

    private static void ExceptionLocated()
    {
        var result = RunSession(Fixture("Throws.cs"));
        Equal("failed", result.GetProperty("status").GetString());
        var error = result.GetProperty("error");
        Equal("System.InvalidOperationException", error.GetProperty("type").GetString());
        Expect(error.GetProperty("file").GetString()!.EndsWith("Throws.cs", StringComparison.OrdinalIgnoreCase), "error file");
        Equal(9, error.GetProperty("line").GetInt32());
    }

    private static void TimeoutCancels()
    {
        var logs = new List<string>();
        Native.DetachedLogSink = (_, message) => logs.Add(message);
        try {
            var result = RunSession(Fixture("Timeout.cs"));
            Equal("failed", result.GetProperty("status").GetString());
            Expect(result.GetProperty("error").GetProperty("message").GetString()!.Contains("timed out"), "timeout message");
            Expect(logs.Any(l => l.Contains("cleanup ran")), "finally block did not run on timeout");
        } finally {
            Native.DetachedLogSink = (_, _) => { };
        }
    }

    private static void StopEndsScenario()
    {
        var result = RunSession(Fixture("Timeout.cs"), options: "{\"timeout_seconds\": 0}", midRun: s => s.Stop("stopped by test"));
        Equal("failed", result.GetProperty("status").GetString());
        Equal("stopped by test", result.GetProperty("error").GetProperty("message").GetString());
    }

    private static void CompileErrorLocated()
    {
        var result = RunSession(Fixture("CompileError.cs"));
        Equal("compile_error", result.GetProperty("status").GetString());
        var first = result.GetProperty("diagnostics")[0];
        Equal("error", first.GetProperty("severity").GetString());
        Equal(7, first.GetProperty("line").GetInt32());
        Expect(first.GetProperty("file").GetString()!.EndsWith("CompileError.cs", StringComparison.OrdinalIgnoreCase), "diagnostic file");
    }

    private static void AmbiguousNeedsName()
    {
        // The fixtures directory holds several scenarios (and one that does not compile), so pick FailedCheck by name
        // from a directory of the compiling ones.
        var directory = Path.Combine(Path.GetTempPath(), $"br-script-tests-{Environment.ProcessId}");
        Directory.CreateDirectory(directory);
        try {
            File.Copy(Fixture("FailedCheck.cs"), Path.Combine(directory, "FailedCheck.cs"), overwrite: true);
            File.Copy(Fixture("Throws.cs"), Path.Combine(directory, "Throws.cs"), overwrite: true);
            var ambiguous = RunSession(directory);
            Equal("compile_error", ambiguous.GetProperty("status").GetString());
            Expect(ambiguous.GetProperty("error").GetProperty("message").GetString()!.Contains("choose one"), "ambiguity message");
            var chosen = RunSession(directory, scenario: "throws");
            Equal("throws", chosen.GetProperty("scenario").GetString());
        } finally {
            Directory.Delete(directory, recursive: true);
        }
    }

    private static void ExamplesCompile()
    {
        var sources = ScenarioCompiler.CollectSources(s_scenarios);
        Expect(sources.Count > 0, "no example scenarios found");
        var output = ScenarioCompiler.Compile(sources, "ExampleScenarios");
        Expect(output.Succeeded, string.Join("\n", output.Diagnostics));
    }

    // --- Expectations ----------------------------------------------------------------------------------------------

    private static void Expect(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }

    private static void Equal<T>(T expected, T actual)
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual)) throw new Exception($"expected {expected}, got {actual}");
    }
}
