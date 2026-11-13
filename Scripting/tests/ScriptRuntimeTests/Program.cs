using System.Text.RegularExpressions;
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
        Environment.SetEnvironmentVariable("BASICRENDERER_SCRIPT_CACHE", "off");

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
            ("an unawaited Task is a compile error, an unawaited animation is not", UnawaitedTaskIsError),
            ("ambiguous sources need a scenario name", AmbiguousNeedsName),
            ("compile cache returns the same assembly for unchanged sources", CompileCacheHits),
            ("look rotation matches the renderer's look-at convention", LookRotationConvention),
            ("move reaches its target exactly on time", MoveReachesTarget),
            ("animations of different channels combine", ChannelsCombine),
            ("ping-pong, loops and Then callbacks", PingPongLoopsAndCallbacks),
            ("orbit returns to its start after a full turn", OrbitFullTurn),
            ("tracks sample keys with easing", TrackSampling),
            ("fixed time steps give identical runs", Deterministic),
            ("entities: placement, parenting, world matrices, destroy", EntityApi),
            ("camera control and projection", CameraApi),
            ("lights: direction, colour, fades", LightApi),
            ("settings are restored when a scenario ends", SettingsRestored),
            ("commands and jobs", CommandsAndJobs),
            ("waiting for the scene to settle", WaitUntilStable),
            ("a model that fails to load fails WhenReady", ModelLoadFailure),
            ("example scenarios compile", ExamplesCompile),
            ("example scenarios pass on the fake host", ExamplesPassOnFakeHost),
        };
        int failed = 0;
        foreach (var (name, body) in tests) {
            try {
                Native.Host = null;
                body();
                Console.WriteLine($"PASS  {name}");
            } catch (Exception e) {
                failed++;
                Console.WriteLine($"FAIL  {name}\n      {e.Message}");
            } finally {
                Native.Host = null;
            }
        }
        Console.WriteLine(failed == 0 ? $"all {tests.Length} tests passed" : $"{failed} of {tests.Length} tests failed");
        return failed;
    }

    // --- Harness ---------------------------------------------------------------------------------------------------

    private sealed class Probe(Func<ScenarioContext, Task> body) : Scenario
    {
        public override Task Run(ScenarioContext ctx) => body(ctx);
    }

    private static ScenarioRunner Runner(Func<ScenarioContext, Task> body, double timeout = 0) =>
        new(new Probe(body), "probe", Path.Combine(s_scenarios, "probe.cs"), timeout);

    private static ScenarioRunner TickUntilFinished(ScenarioRunner runner, double dt = 1.0 / 60, int maxFrames = 20_000, FakeHost? host = null)
    {
        for (int i = 0; i < maxFrames && !runner.IsFinished; i++) {
            host?.Frame();
            runner.Tick(dt);
        }
        Expect(runner.IsFinished, $"scenario did not finish within {maxFrames} frames");
        return runner;
    }

    private static void ExpectPassed(ScenarioRunner runner)
    {
        if (runner.State == ScenarioState.Passed) return;
        var failedChecks = string.Join("; ", runner.Result.Checks.Where(c => !c.Passed).Select(c => c.Message));
        throw new Exception($"{runner.Result.Error?.Message} {failedChecks}".Trim());
    }

    private static FakeHost AttachFake()
    {
        var host = new FakeHost();
        Native.Host = host;
        return host;
    }

    /// <summary>Runs a scenario body on the fake host and returns the host.</summary>
    private static FakeHost RunOnFake(Func<ScenarioContext, Task> body, double dt = 1.0 / 60, Action<FakeHost>? setup = null)
    {
        var host = AttachFake();
        setup?.Invoke(host);
        ExpectPassed(TickUntilFinished(Runner(body), dt, host: host));
        return host;
    }

    // --- Scheduler -------------------------------------------------------------------------------------------------

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
            await Task.WhenAll(Worker("a", 2), Worker("b", 1), Worker("c", 2));
        });
        ExpectPassed(TickUntilFinished(runner));
        Equal("b@1,a@2,c@2", string.Join(",", order));
    }

    private static void SecondsWaitForTime()
    {
        long resumedFrame = -1, zeroWaitFrame = -1;
        double resumedTime = -1;
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
        Equal(5L, resumedFrame);
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
            await Task.Run(() => Thread.Sleep(5));
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

    private static JsonElement RunSession(string source, string? scenario = null, string? options = null, Action<ScriptSession>? midRun = null,
        ulong stopAfterFrames = 0)
    {
        var session = new ScriptSession();
        Equal(AbiConstants.Ok, session.Run(source, scenario, options));
        var clock = Stopwatch.StartNew();
        ulong frame = 0;
        while (session.IsBusy) {
            Expect(clock.Elapsed < TimeSpan.FromSeconds(60), "session did not finish within 60 s");
            (Native.Host as FakeHost)?.Frame();
            session.Tick(frame++, 1.0 / 60);
            if (session.State == ScenarioState.Running && midRun != null) {
                midRun(session);
                midRun = null;
            }
            if (stopAfterFrames != 0 && frame >= stopAfterFrames && session.State == ScenarioState.Running) {
                session.Stop("frame limit reached");   // ExamplesPassOnFakeHost expects this reason
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

    private static void UnawaitedTaskIsError()
    {
        var result = RunSession(Fixture("UnawaitedTask.cs"));
        Equal("compile_error", result.GetProperty("status").GetString());
        var errors = result.GetProperty("diagnostics").EnumerateArray().Where(d => d.GetProperty("severity").GetString() == "error").ToList();
        Equal(1, errors.Count);   // the Task; the animation on the line before is fine
        Equal("CS4014", errors[0].GetProperty("id").GetString());
        Equal(9, errors[0].GetProperty("line").GetInt32());
    }

    private static void AmbiguousNeedsName()
    {
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

    private static void CompileCacheHits()
    {
        var directory = Path.Combine(Path.GetTempPath(), $"br-script-cache-{Environment.ProcessId}");
        Environment.SetEnvironmentVariable("BASICRENDERER_SCRIPT_CACHE", directory);
        var logs = new List<string>();
        Native.DetachedLogSink = (_, message) => logs.Add(message);
        try {
            // A source nothing else compiled, so neither cache layer has it.
            Directory.CreateDirectory(directory);
            var unique = Path.Combine(directory, "Unique.cs");
            File.WriteAllText(unique, File.ReadAllText(Path.Combine(s_scenarios, "Hello.cs")) + $"\n// {Guid.NewGuid()}\n");
            var sources = ScenarioCompiler.CollectSources(unique);
            var key = CompileCache.Key(sources);
            Expect(!CompileCache.TryGet(key, out _), "a fresh cache already had the entry");
            var output = ScenarioCompiler.Compile(sources, "CacheProbe");
            CompileCache.Store(key, output);
            Expect(File.Exists(Path.Combine(directory, key + ".dll")), "the assembly was not written to the cache: " + string.Join("; ", logs));
            Expect(CompileCache.TryGet(key, out var cached) && cached.Assembly!.SequenceEqual(output.Assembly!), "the cache returned something else");
            var copy = Path.Combine(directory, "Hello.cs");
            File.WriteAllText(copy, File.ReadAllText(sources[0]) + "\n// edited\n");
            Expect(CompileCache.Key([copy]) != key, "an edited source kept the same key");
        } finally {
            Native.DetachedLogSink = (_, _) => { };
            Environment.SetEnvironmentVariable("BASICRENDERER_SCRIPT_CACHE", "off");
            if (Directory.Exists(directory)) Directory.Delete(directory, recursive: true);
        }
    }

    // --- Maths and animation ---------------------------------------------------------------------------------------

    private static void LookRotationConvention()
    {
        foreach (var direction in new Vec3[] { (0, 0, -1), (1, 0, 0), (0.3, -0.8, 0.5), (0, -1, 0.001) }) {
            var q = Quat.LookRotation(direction);
            Expect(q.Rotate(Vec3.Forward).ApproximatelyEquals(direction.Normalized, 1e-4f), $"camera -Z for {direction}: {q.Rotate(Vec3.Forward)}");
            Expect(Quat.PointZ(direction).Rotate(Vec3.UnitZ).ApproximatelyEquals(direction.Normalized, 1e-4f), $"light +Z for {direction}");
        }
        // XMMatrixLookAtRH's up: the camera's local +Y stays in the plane of up and the view direction.
        var level = Quat.LookRotation((1, 0, -1));
        Expect(level.Rotate(Vec3.UnitY).ApproximatelyEquals(Vec3.UnitY, 1e-4f), "a level camera keeps +Y up");
    }

    private static void MoveReachesTarget()
    {
        var host = RunOnFake(async ctx =>
        {
            var cube = ctx.Scene.Instantiate("cube.glb", (0, 0, 0));
            var move = cube.Animate().MoveTo((4, 0, 0), 1.0);
            await ctx.Seconds(0.5);
            ctx.CheckNear(cube.Position, (2, 0, 0), 1e-3f, "halfway at 0.5 s (linear)");
            await move;
            ctx.CheckNear(cube.Position, (4, 0, 0), 1e-5f, "at the target when the move ends");
            Equal(60L, ctx.Frame);
        }, dt: 1.0 / 60);
        Equal(1, host.EntityCount);
    }

    private static void ChannelsCombine()
    {
        RunOnFake(async ctx =>
        {
            var cube = ctx.Scene.Instantiate("cube.glb");
            cube.Animate().Spin(Vec3.UnitY, 90);
            await cube.Animate().MoveBy((0, 3, 0), 1.0);
            ctx.CheckNear(cube.Position, (0, 3, 0), 1e-4f, "the move happened");
            ctx.Check(Math.Abs(Quat.AngleBetween(cube.Rotation, Quat.AxisAngle(Vec3.UnitY, 90))) < 0.1, $"the spin happened alongside: {cube.Rotation}");
        }, dt: 0.1);
    }

    private static void PingPongLoopsAndCallbacks()
    {
        RunOnFake(async ctx =>
        {
            var cube = ctx.Scene.Instantiate("cube.glb");
            int fired = 0;
            var bob = cube.Animate().MoveTo((0, 1, 0), 1.0).Then(() => fired++).PingPong(loops: 3);
            await ctx.Seconds(1.5);
            ctx.CheckNear(cube.Position, (0, 0.5f, 0), 1e-3f, "on the way back at 1.5 s");
            await bob;
            ctx.CheckNear(cube.Position, (0, 1, 0), 1e-5f, "three loops end at the far end");
            ctx.Check(fired == 2, $"Then fired once per forward pass: cycles 0 and 2 ({fired})");
            var loop = cube.Animate().MoveBy((1, 0, 0), 0.5).Loop(2);
            await loop;
            ctx.CheckNear(cube.Position, (1, 1, 0), 1e-5f, "a loop restarts from the start (it does not accumulate)");
        }, dt: 0.05);
    }

    private static void OrbitFullTurn()
    {
        RunOnFake(async ctx =>
        {
            var camera = ctx.Camera;
            camera.LookAt((0, 2, 10), (0, 0, 0));
            var orbit = camera.Orbit((0, 0, 0), radius: 10, height: 2, seconds: 4, turns: 1);
            await ctx.Seconds(1);
            // Positive turns go counter-clockwise seen from above: from +Z towards +X.
            ctx.CheckNear(camera.Position, (10, 2, 0), 1e-3f, "a quarter turn");
            ctx.CheckNear(camera.Forward, (Vec3.Zero - camera.Position).Normalized, 1e-3f, "facing the centre");
            await orbit;
            ctx.CheckNear(camera.Position, (0, 2, 10), 1e-3f, "back at the start");
            ctx.Check(!((FakeHost)Native.Host!).CameraInputEnabled, "orbiting took the camera from the fly camera");
        }, dt: 0.05);
    }

    private static void TrackSampling()
    {
        var track = Track.Vec3().Key(0, (0, 0, 0)).Key(1, (2, 0, 0)).Key(3, (2, 4, 0), Ease.InQuad);
        Equal(3.0, track.Duration);
        Expect(track.Sample(-1).ApproximatelyEquals((0, 0, 0)), "held before the first key");
        Expect(track.Sample(0.5).ApproximatelyEquals((1, 0, 0)), "linear between the first keys");
        Expect(track.Sample(2).ApproximatelyEquals((2, 1, 0)), $"eased (InQuad at half: a quarter): {track.Sample(2)}");
        Expect(track.Sample(10).ApproximatelyEquals((2, 4, 0)), "held after the last key");
        RunOnFake(async ctx =>
        {
            var cube = ctx.Scene.Instantiate("cube.glb");
            await cube.Animate().Follow(track);
            ctx.CheckNear(cube.Position, (2, 4, 0), 1e-5f, "followed to the last key");
        }, dt: 0.1);
    }

    private static void Deterministic()
    {
        List<string> Record()
        {
            var samples = new List<string>();
            RunOnFake(async ctx =>
            {
                var cube = ctx.Scene.Instantiate("cube.glb");
                cube.Animate().Spin(Vec3.UnitY, 33.3);
                cube.Animate().Path([(1, 2, 3), (-2, 0, 1), (0, 0, 0)], 2.0, Ease.InOutCubic, face: true);
                for (int i = 0; i < 150; i++) {
                    await ctx.NextFrame();
                    samples.Add(cube.Transform.ToString());
                }
            }, dt: 1.0 / 60);
            return samples;
        }
        var first = Record();
        var second = Record();
        Expect(first.SequenceEqual(second), "two runs with the same time step differed");
    }

    // --- API wrappers ----------------------------------------------------------------------------------------------

    private static void EntityApi()
    {
        var host = RunOnFake(async ctx =>
        {
            var parent = ctx.Scene.Instantiate("cube.glb", (1, 0, 0)).Scaled(2);
            var child = ctx.Scene.Instantiate("cube.glb").At(0, 1, 0).Rotated(0, 90);
            child.Parent = parent;
            await ctx.NextFrame();
            ctx.CheckNear(child.WorldPosition!.Value, (1, 2, 0), 1e-4f, "the child's world position includes the parent's scale and offset");
            parent.Destroy();
            ctx.Check(!parent.IsAlive && child.IsAlive, "destroying the parent leaves the child");
            ctx.Check(child.Parent == parent, "the script's view of the parent is unchanged until reassigned");
            try {
                parent.Position = (0, 0, 0);
                ctx.Fail("writing to a destroyed entity should throw");
            } catch (ScriptApiException e) {
                ctx.Check(e.Status == AbiConstants.StaleHandle, "a stale handle reports STALE_HANDLE");
            }
        });
        Equal(1, host.EntityCount);
    }

    private static void CameraApi()
    {
        var host = RunOnFake(async ctx =>
        {
            var camera = ctx.Camera.Fov(40);
            ctx.Check(Math.Abs(camera.FieldOfView - 40) < 1e-3, "field of view in degrees");
            ctx.Check(((FakeHost)Native.Host!).CameraInputEnabled, "setting the lens alone keeps the fly camera");
            camera.LookAt((0, 5, 5), (0, 0, 0));
            ctx.Check(!((FakeHost)Native.Host!).CameraInputEnabled, "moving the camera takes it from the fly camera");
            ctx.CheckNear(camera.Forward, ((Vec3)(0, -1, -1)).Normalized, 1e-4f, "looking at the target");
            await camera.FlyTo((5, 1, 0), (0, 1, 0), 0.5);
            ctx.CheckNear(camera.Position, (5, 1, 0), 1e-4f, "flew to the eye");
            ctx.CheckNear(camera.Forward, (-1, 0, 0), 1e-4f, "and turned to the target");
        });
        Expect(host.CameraInputEnabled, "the fly camera is given back when the scenario ends");
    }

    private static void LightApi()
    {
        RunOnFake(async ctx =>
        {
            var spot = ctx.Scene.SpotLight((0, 5, 0), (0, -1, 0), intensity: 100, innerConeDegrees: 10, outerConeDegrees: 25, color: (1, 0.5, 0.25));
            ctx.Check(spot.Kind == LightKind.Spot, "kind");
            ctx.CheckNear(spot.Direction, (0, -1, 0), 1e-4f, "direction from creation");
            ctx.Check(Math.Abs(spot.OuterConeDegrees - 25) < 1e-3, "cone in degrees");
            spot.Direction = (1, 0, 0);
            ctx.CheckNear(spot.Direction, (1, 0, 0), 1e-4f, "direction set");
            await spot.FadeTo(10, 1.0);
            ctx.Check(Math.Abs(spot.Intensity - 10) < 1e-4, $"faded to 10 ({spot.Intensity})");
            ctx.CheckNear(spot.Color, (1, 0.5f, 0.25f), 1e-6f, "colour kept through the fade");
        }, dt: 0.1);
    }

    private static void SettingsRestored()
    {
        var host = RunOnFake(async ctx =>
        {
            ctx.Settings["enableBloom"] = true;
            ctx.Settings.Set("exposure", 2.5);
            ctx.Settings.Set("maxShadowCascades", 2);
            await ctx.NextFrame();
            ctx.Check(ctx.Settings.GetBool("enableBloom"), "bool set");
            ctx.Check(Math.Abs(ctx.Settings.GetFloat("exposure") - 2.5f) < 1e-6, "float set");
            ctx.Check(ctx.Settings.GetUInt("maxShadowCascades") == 2, "an int value finds the uint setting");
            try {
                ctx.Settings.Set("enableBloom", 3.0f);
                ctx.Fail("a float for a bool setting should throw");
            } catch (ScriptApiException e) {
                ctx.Check(e.Status == AbiConstants.TypeMismatch, "type mismatch reported");
            }
        });
        Equal(false, (bool)host.Settings["enableBloom"].Value);
        Equal(1.0f, (float)host.Settings["exposure"].Value);
        Equal(4u, (uint)host.Settings["maxShadowCascades"].Value);
    }

    private static void CommandsAndJobs()
    {
        RunOnFake(async ctx =>
        {
            var echo = ctx.Commands.Invoke("echo", new { value = 3, name = "x" });
            ctx.Check(echo.GetProperty("echo").GetProperty("value").GetInt32() == 3, "arguments reach the command");
            ctx.Check(!ctx.Commands.TryInvoke("no.such.command", null, out var reply) && reply.GetProperty("error").GetString()!.Contains("unknown"),
                "an unknown command fails");
            long before = ctx.Frame;
            var job = await ctx.Commands.RunJob("fake.job");
            ctx.Check(job.GetProperty("state").GetString() == "published", "the job finished");
            ctx.Check(ctx.Frame - before == 3, $"polled once a frame ({ctx.Frame - before} frames)");
        });
    }

    private static void WaitUntilStable()
    {
        RunOnFake(async ctx =>
        {
            var host = (FakeHost)Native.Host!;
            host.PendingWork = 5;
            var stable = ctx.WaitUntilStable(frames: 10);
            await ctx.Frames(20);
            ctx.Check(!stable.IsCompleted, "not stable while work is pending");
            host.PendingWork = 0;
            await ctx.Frames(5);
            host.Signature++;   // the workload changed: start counting again
            long settledFrom = ctx.Frame;
            await stable;
            ctx.Check(ctx.Frame - settledFrom >= 10, $"waited for 10 unchanged frames ({ctx.Frame - settledFrom})");
        });
    }

    private static void ModelLoadFailure()
    {
        RunOnFake(async ctx =>
        {
            var model = ctx.Scene.Instantiate("missing.glb");
            try {
                await model.WhenReady();
                ctx.Fail("WhenReady should throw for a failed model");
            } catch (ScenarioFailedException e) {
                ctx.Check(e.Message.Contains("could not load"), $"the failure says why: {e.Message}");
            }
            ctx.Check(!model.IsAlive, "a failed model is not alive");
        }, setup: host => host.MissingFiles.Add("missing.glb"));
    }

    // --- Examples --------------------------------------------------------------------------------------------------

    private static void ExamplesCompile()
    {
        var sources = ScenarioCompiler.CollectSources(s_scenarios);
        Expect(sources.Count > 0, "no example scenarios found");
        var output = ScenarioCompiler.Compile(sources, "ExampleScenarios");
        Expect(output.Succeeded, string.Join("\n", output.Diagnostics));
        Expect(output.Diagnostics.Count == 0, "warnings: " + string.Join("\n", output.Diagnostics));
    }

    private static void ExamplesPassOnFakeHost()
    {
        foreach (var file in Directory.EnumerateFiles(s_scenarios, "*.cs").Order()) {
            var host = AttachFake();
            host.ModelLoadFrames = 3;
            // An example with no time limit runs until stopped: run it for ten seconds, then stop it.
            // A stopped scenario ends "failed" with the stop reason as its error; that is the expected end here.
            bool unlimited = Regex.IsMatch(File.ReadAllText(file), @"TimeoutSeconds\s*=\s*0\b");
            var result = RunSession(file, stopAfterFrames: unlimited ? 600u : 0u);
            var status = result.GetProperty("status").GetString();
            var error = result.TryGetProperty("error", out var e) ? e.GetProperty("message").GetString() : "";
            bool expectedEnd = unlimited ? status == "failed" && error == "frame limit reached" : status == "passed";
            if (!expectedEnd || result.GetProperty("checks").EnumerateArray().Any(c => !c.GetProperty("passed").GetBoolean())) {
                var failed = result.GetProperty("checks").EnumerateArray().Where(c => !c.GetProperty("passed").GetBoolean())
                    .Select(c => c.GetProperty("message").GetString());
                throw new Exception($"{Path.GetFileName(file)}: {status}: {error} {string.Join("; ", failed)}");
            }
        }
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
