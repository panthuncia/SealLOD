// Strand hair simulation LOD (docs/hair.md): the number of simulated guides follows the view. A drooping procedural
// groom of 4,096 guides in four levels sits on a sphere that sways gently from side to side throughout. The scenario
// reads the simulation's metrics of every frame (hair.sim) and
//   (a) sweeps a forced simulation level slowly from every guide down to none and back, and very slowly across a small
//       range, and checks that guides are promoted and demoted a few at a time and that the frames in which a guide
//       changes state move it no further than other frames move the others;
//   (b) lets the level follow the camera as it dollies away until nothing is simulated, and back;
//   (c) sweeps the forced level fast, turning blends round before they end and the instance off and on;
//   (d) swings two such grooms side by side, one fully simulated and one at a reduced level, and measures how far the
//       reduced one's guides are from the full one's.
// For (a) and (c) it also dumps the guide state of consecutive frames and compares each guide's motion in the frame it
// changes state with its motion in the frames either side. It captures the sweeps (about 60 files of 16 MB,
// hair_sim_lod_*.bmp in HAIR_CAPTURE_DIR, or in hair_captures in the temporary directory); the dumps go to a folder
// beside them, up to 400 MB at a time, and are deleted when read.
//   BasicRenderer.exe --script scenarios/hair/hair_sim_lod.cs --script-exit --script-fixed-dt 0.0166667
// Needs a host with the hair plugin (the demo), in raster mode, and a fixed 1/60 s frame.

[Scenario("hair-sim-lod", TimeoutSeconds = 1500, Description = "Simulation LOD: forced and automatic sweeps of the simulation level, promotion and demotion without jumps, fidelity at reduced levels.")]
public sealed class HairSimLod : Scenario
{
    const int Guides = 4096;
    static readonly uint[] LevelGuides = { 64, 192, 768, 3072 };
    const double Levels = 4.0;
    // The most a guide segment may be off its rest length while guides are promoted and demoted.
    const double StretchLimit = 0.10;
    // A frame in which guides change state may move one of them this many times as far as the same phase's steady
    // guides move at most, plus this many metres. A blend carries a guide from one state to another in a quarter of a
    // second, so a guide in one moves faster than its neighbours by the gap between the two; the fast sweeps, where
    // every guide is in a blend at once and blends turn round, get the wider factor. Without blends a slow sweep's
    // factor is about 2.
    const double TransitionFactor = 1.5;
    const double FastTransitionFactor = 2.5;
    const double TransitionSlack = 0.0005;
    // HairGuidePoint: 80 bytes; position at 0, rigid at 48, guideState at 76 (row 0 of a guide).
    const int RowBytes = 80;
    const uint StateSimulated = 1, StateBlending = 2 | 4;

    static readonly Vec3 HeadPosition = (0, 1.5, 0);
    static readonly Vec3 SecondHeadPosition = (2.6, 1.5, 0);

    sealed record Sample(ulong Frame, uint Instance, double Level, double Target, double Coverage, uint Simulated, uint[] PerLevel, uint Points,
        uint Evaluated, uint Blending, uint Promoted, uint Demoted, double Transition, double Steady, double StretchMax, double SpeedMax,
        double Deviation, double ColliderDistance, uint StrandRoots)
    {
        public override string ToString() =>
            $"frame={Frame} sFrac={Level:F3} target={Target:F3} coverage={Coverage:F4} simulated={Simulated} [{string.Join(",", PerLevel)}] points={Points} " +
            $"evaluated={Evaluated} blending={Blending} promoted={Promoted} demoted={Demoted} transition={Transition * 1000:F3}mm steady={Steady * 1000:F3}mm";
    }

    static string CaptureFolder()
    {
        string folder = Environment.GetEnvironmentVariable("HAIR_CAPTURE_DIR") ?? System.IO.Path.Combine(System.IO.Path.GetTempPath(), "hair_captures");
        System.IO.Directory.CreateDirectory(folder);
        return folder.Replace('\\', '/');
    }

    readonly SortedDictionary<ulong, Sample> _samples = new();   // the first instance's
    readonly List<string> _captures = new();
    Entity? _head, _secondHead;
    int _swayFrame;
    bool _sway = true;

    // Rows of metrics read with the groom in view and out of it: the simulation follows the view, and this scenario is
    // about the solver, so its grooms have to be in view throughout.
    int _inViewRows, _outOfViewRows;
    // Guide rows are lent to a groom while it is simulated: guides that went on from rows that were not theirs of the
    // frame before, and rows of metrics read with the groom in view but without guide rows.
    long _staleGuides, _starvedRows;

    void Collect(ScenarioContext ctx)
    {
        foreach (var row in ctx.Commands.Invoke("hair.sim").GetProperty("metrics").EnumerateArray()) {
            if (row.GetProperty("in_view").GetBoolean()) ++_inViewRows; else ++_outOfViewRows;
            _staleGuides += row.GetProperty("stale_guides").GetUInt32();
            if (row.GetProperty("starved").GetBoolean()) ++_starvedRows;
            if (row.GetProperty("instance").GetUInt32() != 0) continue;
            double distance = row.TryGetProperty("collider_distance", out var value) ? value.GetDouble() : double.PositiveInfinity;
            var sample = new Sample(row.GetProperty("frame").GetUInt64(), 0, row.GetProperty("sim_level").GetDouble(),
                row.GetProperty("target_level").GetDouble(), row.GetProperty("coverage").GetDouble(), row.GetProperty("guides").GetUInt32(),
                row.GetProperty("level_guides").EnumerateArray().Select(count => count.GetUInt32()).ToArray(),
                row.GetProperty("simulated_points").GetUInt32(), row.GetProperty("evaluated_guides").GetUInt32(),
                row.GetProperty("blending_guides").GetUInt32(), row.GetProperty("promoted_guides").GetUInt32(),
                row.GetProperty("demoted_guides").GetUInt32(), row.GetProperty("transition_motion").GetDouble(),
                row.GetProperty("steady_motion").GetDouble(), row.GetProperty("stretch_max").GetDouble(), row.GetProperty("speed_max").GetDouble(),
                row.GetProperty("deviation_mean").GetDouble(), distance, row.GetProperty("strand_roots").GetUInt32());
            _samples[sample.Frame] = sample;
        }
    }

    // One frame. The head sways 5 cm to each side every two seconds, so that the hair is never quite still.
    async Task Step(ScenarioContext ctx, int frames = 1)
    {
        for (int frame = 0; frame < frames; ++frame) {
            if (_sway) {
                ++_swayFrame;
                _head!.Position = (HeadPosition.X + 0.05 * Math.Sin(2 * Math.PI * _swayFrame / 120.0), HeadPosition.Y, HeadPosition.Z);
            }
            await ctx.NextFrame();
            Collect(ctx);
        }
    }

    ulong NewestFrame => _samples.Count == 0 ? 0 : _samples.Keys.Max();

    List<Sample> Since(ulong frame) => _samples.Values.Where(sample => sample.Frame > frame).ToList();

    static void Force(ScenarioContext ctx, double level) => ctx.Commands.Invoke("hair.sim", new { lod_force_level = level });

    static bool CapturePending(ScenarioContext ctx) =>
        ctx.Commands.Invoke("hair.stats").GetProperty("capture").GetProperty("state").GetString() == "pending";

    // Asks for a capture when none is pending. Returns whether it did.
    bool TryCapture(ScenarioContext ctx, string name)
    {
        if (CapturePending(ctx)) return false;
        string path = CaptureFolder() + "/" + name + ".bmp";
        ctx.Commands.Invoke("hair.capture", new { path });
        _captures.Add(path);
        return true;
    }

    async Task Capture(ScenarioContext ctx, string name)
    {
        for (int frame = 0; frame < 600 && CapturePending(ctx); ++frame) await Step(ctx);
        TryCapture(ctx, name);
    }

    async Task Captured(ScenarioContext ctx)
    {
        for (int frame = 0; frame < 600 && CapturePending(ctx); ++frame) await Step(ctx);
        int written = _captures.Count(System.IO.File.Exists);
        ctx.Check(written == _captures.Count, $"{written} of {_captures.Count} captures were written, the last {(_captures.Count > 0 ? _captures[^1] : "")}");
        _captures.Clear();
    }

    // Moves the forced level from `from` to `to` over `frames` frames. With `captures`, a capture every 60 frames, and
    // over frames 144 to 192 as often as captures complete (every fourth frame or so).
    async Task Sweep(ScenarioContext ctx, double from, double to, int frames, string? captures = null)
    {
        for (int frame = 1; frame <= frames; ++frame) {
            Force(ctx, from + (to - from) * frame / frames);
            await Step(ctx);
            if (captures != null && (frame % 60 == 0 || (frame >= 144 && frame < 192))) TryCapture(ctx, $"{captures}_{frame:D4}");
        }
    }

    // What every phase checks over its frames (after `since`, up to `until`), and the discontinuity measure: the largest
    // motion of a guide that changed state or is in a blend against the largest motion of any other guide.
    void CheckPhase(ScenarioContext ctx, string what, ulong since, ulong until = ulong.MaxValue, double factor = TransitionFactor)
    {
        var samples = Since(since).Where(sample => sample.Frame <= until).ToList();
        ctx.Require(samples.Count > 0, $"{what}: metrics were read back");
        var simulated = samples.Where(sample => sample.Simulated > 0).ToList();
        var changing = simulated.Where(sample => sample.Promoted + sample.Demoted + sample.Blending > 0).ToList();
        double steady = simulated.Count > 0 ? simulated.Max(sample => sample.Steady) : 0;
        double steadyMean = simulated.Count > 0 ? simulated.Average(sample => sample.Steady) : 0;
        double transition = changing.Count > 0 ? changing.Max(sample => sample.Transition) : 0;
        double transitionMean = changing.Count > 0 ? changing.Average(sample => sample.Transition) : 0;
        // Frame by frame: the transition guides' largest motion over the steady guides' in the same frame.
        var ratios = changing.Where(sample => sample.Steady > 1e-5).Select(sample => sample.Transition / sample.Steady).OrderBy(ratio => ratio).ToList();
        ctx.Log($"{what}: {samples.Count} frames, {simulated.Count} simulated, {changing.Count} with guides changing state or blending; " +
            $"promotions {samples.Sum(sample => sample.Promoted)}, demotions {samples.Sum(sample => sample.Demoted)}, most guides blending {samples.Max(sample => sample.Blending)}");
        ctx.Log($"{what}: largest guide motion in a frame, relative to its root: changing guides {transition * 1000:F3} mm (mean of the frames' maxima {transitionMean * 1000:F3}), " +
            $"other guides {steady * 1000:F3} mm (mean {steadyMean * 1000:F3}); changing over others in the same frame: " +
            (ratios.Count > 0 ? $"median {ratios[ratios.Count / 2]:F2}, 95th percentile {ratios[ratios.Count * 95 / 100]:F2}, largest {ratios[^1]:F2}" : "no frames"));
        ctx.Check(transition <= factor * steady + TransitionSlack,
            $"{what}: no frame moves a guide that changes state further than {factor} times the furthest another guide moves in the phase, plus {TransitionSlack * 1000} mm " +
            $"({transition * 1000:F3} mm against {steady * 1000:F3} mm)");
        if (simulated.Count > 0) {
            double stretch = simulated.Max(sample => sample.StretchMax);
            ctx.Check(stretch < StretchLimit, $"{what}: no simulated guide segment is more than {StretchLimit:P0} off its rest length ({stretch:P3})");
            ctx.Check(simulated.Min(sample => sample.ColliderDistance) >= 0.0, $"{what}: no simulated guide point is inside the collider");
            ctx.Check(simulated.All(sample => !double.IsNaN(sample.Deviation) && sample.SpeedMax < 100.0), $"{what}: no guide point is lost (speeds stay finite)");
            ctx.Check(simulated.All(sample => sample.Simulated + sample.Evaluated == Guides),
                $"{what}: every guide is simulated or evaluated in every simulated frame");
        }
        ctx.Check(samples.All(sample => sample.Simulated == sample.PerLevel.Sum(count => count)), $"{what}: the levels' counts add up");
    }

    // ---- Guide state dumps.

    sealed class GuideDump
    {
        public ulong Frame;
        public float[] Position = Array.Empty<float>(); // xyz per row
        public float[] Rigid = Array.Empty<float>();
        public uint[] State = Array.Empty<uint>();      // per row; a guide's is its row 0's
    }

    static GuideDump ReadDump(string path, int rows)
    {
        byte[] bytes = System.IO.File.ReadAllBytes(path);
        if (bytes.Length < 8 + rows * RowBytes) throw new InvalidOperationException($"{path} holds {bytes.Length} bytes, fewer than {rows} rows");
        var dump = new GuideDump { Frame = BitConverter.ToUInt64(bytes, 0), Position = new float[rows * 3], Rigid = new float[rows * 3], State = new uint[rows] };
        for (int row = 0; row < rows; ++row) {
            int at = 8 + row * RowBytes;
            for (int axis = 0; axis < 3; ++axis) {
                dump.Position[row * 3 + axis] = BitConverter.ToSingle(bytes, at + axis * 4);
                dump.Rigid[row * 3 + axis] = BitConverter.ToSingle(bytes, at + 48 + axis * 4);
            }
            dump.State[row] = BitConverter.ToUInt32(bytes, at + 76);
        }
        System.IO.File.Delete(path);
        return dump;
    }

    static string DumpFolder()
    {
        string folder = CaptureFolder() + "/dumps";
        System.IO.Directory.CreateDirectory(folder);
        return folder;
    }

    async Task WaitDumps(ScenarioContext ctx)
    {
        for (int frame = 0; frame < 600 && ctx.Commands.Invoke("hair.stats").GetProperty("dumps").GetProperty("pending").GetUInt32() != 0; ++frame) await Step(ctx);
        ctx.Require(ctx.Commands.Invoke("hair.stats").GetProperty("dumps").GetProperty("failed").GetUInt32() == 0, "no dump failed");
    }

    // The same guide across frames: each guide's motion in a frame (the furthest any of its points moved relative to its
    // root, between two consecutive dumps) against its own motion in the frame before and the frame after. `excess` is
    // how much further it moved than in the larger of those two. For the guides that became or stopped being simulated
    // in the frame, for those in a blend, and for the rest.
    void CompareNeighbouringFrames(ScenarioContext ctx, string what, List<GuideDump> dumps, int pointsPerGuide, double factor = TransitionFactor)
    {
        ctx.Require(dumps.Zip(dumps.Skip(1)).All(pair => pair.Second.Frame == pair.First.Frame + 1), $"{what}: the dumps are of consecutive frames");
        int frames = dumps.Count;
        var motion = new double[frames][];
        for (int frame = 1; frame < frames; ++frame) {
            motion[frame] = new double[Guides];
            float[] now = dumps[frame].Position, before = dumps[frame - 1].Position;
            for (int guide = 0; guide < Guides; ++guide) {
                int root = guide * pointsPerGuide * 3;
                double rx = now[root] - before[root], ry = now[root + 1] - before[root + 1], rz = now[root + 2] - before[root + 2];
                double furthest = 0;
                for (int index = 2; index < pointsPerGuide; ++index) {
                    int at = root + index * 3;
                    double dx = now[at] - before[at] - rx, dy = now[at + 1] - before[at + 1] - ry, dz = now[at + 2] - before[at + 2] - rz;
                    furthest = Math.Max(furthest, Math.Sqrt(dx * dx + dy * dy + dz * dz));
                }
                motion[frame][guide] = furthest;
            }
        }
        // Group 0: changed state in the frame; 1: in a blend; 2: the rest.
        var count = new int[3];
        var largest = new double[3];
        var excess = new double[3];
        var excessSum = new double[3];
        for (int frame = 2; frame + 1 < frames; ++frame) {
            for (int guide = 0; guide < Guides; ++guide) {
                uint state = dumps[frame].State[guide * pointsPerGuide], stateBefore = dumps[frame - 1].State[guide * pointsPerGuide];
                int group = ((state ^ stateBefore) & StateSimulated) != 0 ? 0 : (state & StateBlending) != 0 ? 1 : 2;
                double over = motion[frame][guide] - Math.Max(motion[frame - 1][guide], motion[frame + 1][guide]);
                ++count[group];
                largest[group] = Math.Max(largest[group], motion[frame][guide]);
                excess[group] = Math.Max(excess[group], over);
                excessSum[group] += Math.Max(over, 0);
            }
        }
        string[] names = { "guides in the frame they become or stop being simulated", "guides in a blend", "other guides" };
        for (int group = 0; group < 3; ++group) {
            ctx.Log($"{what}: {names[group]}: {count[group]} guide-frames; furthest motion {largest[group] * 1000:F3} mm; " +
                $"furthest beyond their own motion in the frame before and after {excess[group] * 1000:F3} mm (mean {(count[group] > 0 ? excessSum[group] / count[group] * 1000 : 0):F4} mm)");
        }
        ctx.Check(count[0] > 0 && count[2] > 0, $"{what}: the dumped frames hold guides that change state ({count[0]}) and guides that do not ({count[2]})");
        ctx.Check(excess[0] <= factor * excess[2] + TransitionSlack && excess[1] <= factor * excess[2] + TransitionSlack,
            $"{what}: a guide moves no further in the frame it changes state, or in a blend, beyond its motion in the frames either side, than {factor} times what other guides do plus {TransitionSlack * 1000} mm " +
            $"(changing {excess[0] * 1000:F3} mm, blending {excess[1] * 1000:F3} mm, others {excess[2] * 1000:F3} mm)");
    }

    // An instance's first row of guide state: rows are lent while it is simulated, and -1 means it holds none.
    static int GuideRowBase(ScenarioContext ctx, int instance) =>
        (int)ctx.Commands.Invoke("hair.stats").GetProperty("instance_list")[instance].GetProperty("guide_row_base").GetInt64();

    // Dumps the guide state of `frames` consecutive frames while `drive` sets each frame's level. The groom is the only
    // one simulated, so its rows are the buffer's first.
    async Task<List<GuideDump>> DumpFrames(ScenarioContext ctx, string name, int frames, int rows, Action<int> drive)
    {
        ctx.Require(GuideRowBase(ctx, 0) == 0, $"{name}: the groom's guide rows are the first of the buffer ({GuideRowBase(ctx, 0)})");
        var paths = new List<string>();
        for (int frame = 0; frame < frames; ++frame) {
            drive(frame);
            paths.Add($"{DumpFolder()}/{name}_{frame:D2}.bin");
            ctx.Commands.Invoke("hair.dump", new { buffer = "guide_points", path = paths[^1], rows });
            await Step(ctx);
        }
        await WaitDumps(ctx);
        return paths.Select(path => ReadDump(path, rows)).ToList();
    }

    public override async Task Run(ScenarioContext ctx)
    {
        ctx.Settings["enablePathTracing"] = false;

        var scene = ctx.Scene;
        scene.DirectionalLight(direction: (0.5, -0.7, -0.8), intensity: 4f);
        _head = scene.Instantiate("../assets/sphere.glb", position: HeadPosition, name: "hair-lod-head");
        _secondHead = scene.Instantiate("../assets/sphere.glb", position: SecondHeadPosition, name: "hair-lod-head-b");
        var floor = scene.Instantiate("../assets/plane.glb", position: (0, 0, 0), name: "hair-lod-floor");
        await Task.WhenAll(_head.WhenReady(), _secondHead.WhenReady(), floor.WhenReady());
        Vec3 eye = (0, 1.7, 4.2), target = (0, 1.2, 0);
        ctx.Camera.Fov(50).LookAt(eye: eye, target: target);

        var commands = ctx.Commands;
        commands.Invoke("hair.clear");
        commands.Invoke("hair.lod", new { error_pixels = 1.0, max_width_scale = 8.0, min_detail = 0.02, force_detail = 0.0, segment_pixels = 4.0, freeze = false });
        if (Environment.GetEnvironmentVariable("HAIR_COVERAGE") is string coverage) commands.Invoke("hair.lod", new { coverage }); // off: the opaque form
        if (Environment.GetEnvironmentVariable("HAIR_PATH") is string path) commands.Invoke("hair.render", new { path }); // layered: the composited path
        commands.Invoke("hair.debug", new { view = "off" });
        // Every control, so an earlier session's values do not carry over. The level is forced to every guide to begin with.
        var controls = commands.Invoke("hair.sim", new {
            enabled = false, gravity = new[] { 0.0, -9.81, 0.0 }, damping = 3.0, stretch_compliance = 0.0, bend_compliance = 1e-5, shape_compliance = 0.01,
            step_seconds = 1.0 / 60.0, substeps = 4, max_steps = 4, wind = new[] { 0.0, 0.0, 0.0 }, gust_amplitude = 0.5, gust_frequency = 0.7,
            friction = 0.3, collision_margin = 0.002, teleport_distance = 1.0,
            lod_full_coverage = 1.0, lod_level0_coverage = 0.125, lod_off_coverage = 0.03, lod_rate = 2.0, lod_force_level = Levels, lod_point_budget = 0,
            promote_seconds = 0.25, demote_seconds = 0.25,
        });
        double blendSeconds = 0.25;
        int blendFrames = (int)Math.Ceiling(blendSeconds * 60.0) + 4; // a blend, and the frames an instance takes to stop after it
        uint asset = commands.Invoke("hair.load", new {
            procedural = new { style = "wavy", strands = 20000, seed = 5, radius = 0.5, length = 0.9, droop = 6.0, wave_amplitude = 0.012, wave_count = 5.0,
                root_width = 0.0004, tip_width = 0.0002, points = 33 },
        }).GetProperty("asset").GetUInt32();
        uint instance = commands.Invoke("hair.attach", new { asset, entity = "hair-lod-head" }).GetProperty("instance").GetUInt32();
        var sphere = new[] { new { center = new[] { 0.0, 0.0, 0.0 }, radius = 0.5 } };
        commands.Invoke("hair.colliders", new { instance, colliders = sphere });
        commands.Invoke("hair.stats", new { continuous = true });
        await ctx.WaitUntilStable();
        await ctx.Frames(30);

        ctx.Require(Math.Abs(ctx.DeltaTime - 1.0 / 60.0) < 1e-5, "the scenario runs with --script-fixed-dt 0.0166667");
        var loaded = commands.Invoke("hair.stats");
        ctx.Require(loaded.GetProperty("active_instances").GetUInt32() == 1, "the groom is attached and dispatched");
        int guideRows = (int)loaded.GetProperty("instance_list")[0].GetProperty("guide_rows").GetUInt32();
        ctx.Require(loaded.GetProperty("rows").GetProperty("guides").GetProperty("used").GetUInt32() == Guides && guideRows % Guides == 0,
            $"the groom has {Guides} guides of one point count ({guideRows} guide points)");
        int pointsPerGuide = guideRows / Guides;
        ctx.Log($"groom: {loaded.GetProperty("strands")} strands, {Guides} guides of {pointsPerGuide} points; controls {controls}");

        // Every guide simulated, settled under the sway.
        commands.Invoke("hair.sim", new { enabled = true });
        ulong start = NewestFrame;
        await Step(ctx, 300);
        var full = Since(start);
        ctx.Require(full.Count > 0, "the simulation's metrics are read back");
        ctx.Check(full[^1].Simulated == Guides && full[^1].PerLevel.SequenceEqual(LevelGuides) && full[^1].Evaluated == 0 && full[^1].Points == guideRows,
            $"at the highest level every guide is simulated: {full[^1]}");
        ctx.Check(full.Skip(1).All(sample => sample.Promoted + sample.Demoted + sample.Blending == 0), "and none changes state");
        ctx.Log($"full simulation under the sway: largest guide motion in a frame {full.Skip(60).Max(sample => sample.Steady) * 1000:F3} mm relative to its root");
        await Capture(ctx, "hair_sim_lod_cost_0_full");
        await Captured(ctx);

        // (a) A slow sweep down to nothing and back.
        const int SweepFrames = 600;
        ulong sweepDownStart = NewestFrame;
        await Sweep(ctx, Levels, 0.0, SweepFrames, "hair_sim_lod_a_down");
        await Step(ctx, blendFrames + 8);
        ulong bottom = NewestFrame;
        await Step(ctx, 30);
        var off = Since(bottom);
        ctx.Check(off.All(sample => sample.Simulated == 0 && sample.Evaluated == 0 && sample.Level == 0.0),
            $"at level 0 nothing is simulated and the strands follow their roots: {off[^1]}");
        await Capture(ctx, "hair_sim_lod_a_off");
        var down = Since(sweepDownStart).Where(sample => sample.Frame <= bottom).ToList();
        ulong sweepUpStart = NewestFrame;
        await Sweep(ctx, 0.0, Levels, SweepFrames);
        await Step(ctx, blendFrames + 8);
        var up = Since(sweepUpStart);
        ctx.Check(up[^1].Simulated == Guides && up[^1].Blending == 0, $"back at the highest level every guide is simulated again: {up[^1]}");
        foreach (var (name, sweep, sign) in new[] { ("down", down, -1), ("up", up, 1) }) {
            var live = sweep.Where(sample => sample.Level > 0).ToList();
            var steps = live.Zip(live.Skip(1)).Where(pair => pair.Second.Frame == pair.First.Frame + 1)
                .Select(pair => ((int)pair.Second.Simulated - (int)pair.First.Simulated) * sign).ToList();
            ctx.Check(steps.All(step => step >= 0), $"slow sweep {name}: the simulated guide count never moves the other way");
            // The level moves 1/150 a frame: about 20 of level 3's guides.
            ctx.Check(steps.Max() <= 48, $"slow sweep {name}: guides change a few at a time, never a level at once ({steps.Max()} at most in a frame, {steps.Where(step => step > 0).Average():F1} on average)");
            foreach (var sample in sweep.Where((_, index) => index % 50 == 0)) ctx.Log($"slow sweep {name}: {sample}");
            var levelOne = live.Where(sample => sample.Level <= 1.0 && sample.Blending == 0).ToList();
            ctx.Check(levelOne.Count > 0 && levelOne.All(sample => sample.Simulated == LevelGuides[0]),
                $"slow sweep {name}: between level 0 and 1 only the {LevelGuides[0]} level 0 guides are simulated ({levelOne.Count} frames)");
        }
        CheckPhase(ctx, "slow sweep down", sweepDownStart, sweepUpStart);
        CheckPhase(ctx, "slow sweep up", sweepUpStart);
        await Captured(ctx);

        // A very slow sweep over a twentieth of level 3: its guides go one at a time.
        Force(ctx, 3.5);
        await Step(ctx, blendFrames + 8);
        ulong fineStart = NewestFrame;
        await Sweep(ctx, 3.5, 3.45, 300);
        await Step(ctx, blendFrames + 8);
        var fine = Since(fineStart);
        var fineSteps = fine.Zip(fine.Skip(1)).Where(pair => pair.Second.Frame == pair.First.Frame + 1)
            .Select(pair => (int)pair.First.Simulated - (int)pair.Second.Simulated).ToList();
        ctx.Check(fineSteps.All(step => step >= 0 && step <= 3) && fineSteps.Count(step => step == 1) >= 40,
            $"very slow sweep: {fine[0].Simulated} to {fine[^1].Simulated} simulated guides over 300 frames, one guide at a time " +
            $"({fineSteps.Count(step => step == 1)} frames demote one, {fineSteps.Count(step => step == 2)} two, {fineSteps.Count(step => step > 2)} more; the most {fineSteps.Max()})");
        CheckPhase(ctx, "very slow sweep", fineStart);

        // The same guide in neighbouring frames, while the level rises for 10 frames and then falls for 26 at the slow
        // sweep's rate: guides are promoted, and demotions that begin in the first of the falling frames end in the last.
        Force(ctx, 2.5);
        await Step(ctx, blendFrames + 8);
        var slowDumps = await DumpFrames(ctx, "slow", 36, guideRows, frame => Force(ctx, 2.5 + (frame < 10 ? frame + 1 : 19 - frame) / 150.0));
        CompareNeighbouringFrames(ctx, "slow sweep, neighbouring frames", slowDumps, pointsPerGuide);

        // Cost readings (the pass timing panel in the captures): an intermediate level, then level 0 alone.
        Force(ctx, 2.5);
        await Step(ctx, 300);
        ctx.Log($"intermediate level: {Since(0)[^1]}");
        await Capture(ctx, "hair_sim_lod_cost_1_level_2_5");
        await Captured(ctx);
        Force(ctx, 1.0);
        await Step(ctx, 300);
        ctx.Log($"level 0 only: {Since(0)[^1]}");
        await Capture(ctx, "hair_sim_lod_cost_2_level_1");
        await Captured(ctx);

        // (b) The level follows the camera. The groom's bounds fill the view at 2 m and are a fiftieth of it at 120 m.
        static Vec3 Eye(double distance) => (0, 1.2 + 0.12 * distance, distance);
        const double NearDistance = 2.0, FarDistance = 120.0;
        const int DollyFrames = 480;
        Force(ctx, Levels);
        await Step(ctx, blendFrames + 8);
        ctx.Camera.LookAt(eye: Eye(NearDistance), target: target);
        await Step(ctx, 8);
        Force(ctx, -1.0);
        await Step(ctx, 8);
        ulong autoStart = NewestFrame;
        await Step(ctx, 90);
        var near = Since(autoStart);
        ctx.Check(near.All(sample => sample.Level == Levels && sample.Simulated == Guides) && near[^1].Coverage >= 1.0,
            $"with the groom filling the view every guide is simulated: {near[^1]}");
        await Capture(ctx, "hair_sim_lod_b_near");
        ulong dollyOutStart = NewestFrame;
        for (int frame = 1; frame <= DollyFrames; ++frame) {
            ctx.Camera.LookAt(eye: Eye(NearDistance * Math.Pow(FarDistance / NearDistance, frame / (double)DollyFrames)), target: target);
            await Step(ctx);
            if (frame == DollyFrames / 2) {
                // Half way, the camera stands still for two seconds: the level comes to rest and stays there.
                await Step(ctx, 60);
                ulong still = NewestFrame;
                await Step(ctx, 60);
                var stillFrames = Since(still);
                ctx.Check(stillFrames.All(sample => sample.Level == stillFrames[0].Level && sample.Promoted + sample.Demoted == 0),
                    $"with the camera still the level does not move and no guide changes state: {stillFrames[^1]}");
                await Capture(ctx, "hair_sim_lod_b_middle");
            }
        }
        await Step(ctx, 120);
        ulong far = NewestFrame;
        await Step(ctx, 300);
        var farFrames = Since(far);
        ctx.Check(farFrames.All(sample => sample.Simulated == 0 && sample.Level == 0.0 && sample.Coverage > 0 && sample.Coverage < 0.03),
            $"far away nothing is simulated: {farFrames[^1]}");
        await Capture(ctx, "hair_sim_lod_cost_3_off_by_distance");
        var dollyOut = Since(dollyOutStart).Where(sample => sample.Frame <= far).ToList();
        var rise = dollyOut.Zip(dollyOut.Skip(1)).FirstOrDefault(pair => pair.Second.Level > pair.First.Level + 1e-6);
        ctx.Check(rise.First is null, "dollying away, the level never rises" + (rise.First is null ? "" : $" (from {rise.First} to {rise.Second})"));
        foreach (var sample in dollyOut.Where((_, index) => index % 40 == 0)) ctx.Log($"dolly away: {sample}");
        ulong dollyBackStart = NewestFrame;
        for (int frame = 1; frame <= DollyFrames; ++frame) {
            ctx.Camera.LookAt(eye: Eye(FarDistance * Math.Pow(NearDistance / FarDistance, frame / (double)DollyFrames)), target: target);
            await Step(ctx);
        }
        await Step(ctx, 180);
        var dollyBack = Since(dollyBackStart);
        ctx.Check(dollyBack.Zip(dollyBack.Skip(1)).All(pair => pair.Second.Level >= pair.First.Level - 1e-6), "dollying back, the level never falls");
        ctx.Check(dollyBack[^1].Level == Levels && dollyBack[^1].Simulated == Guides && dollyBack[^1].Blending == 0,
            $"back at the near end every guide is simulated again: {dollyBack[^1]}");
        foreach (var sample in dollyBack.Where((_, index) => index % 40 == 0)) ctx.Log($"dolly back: {sample}");
        var started = dollyBack.FirstOrDefault(sample => sample.Simulated > 0);
        ctx.Log($"dolly back: simulation starts again at {started}");
        CheckPhase(ctx, "dolly away", dollyOutStart, dollyBackStart);
        CheckPhase(ctx, "dolly back", dollyBackStart);
        await Captured(ctx);

        // (c) Fast sweeps of a forced level.
        ctx.Camera.LookAt(eye: eye, target: target);
        Force(ctx, Levels);
        await Step(ctx, 60);
        ulong fastStart = NewestFrame;
        // A triangle between 0.5 and every guide, 12 frames each way: shorter than a blend, so blends turn round.
        static double Triangle(int frame, int half, double low, double high) =>
            low + (high - low) * Math.Abs((frame % (2 * half)) - half) / half;
        for (int frame = 0; frame < 240; ++frame) {
            Force(ctx, Triangle(frame, 12, 0.5, Levels));
            await Step(ctx);
            if (frame >= 24 && frame < 72) TryCapture(ctx, $"hair_sim_lod_c_fast_{frame:D4}");
        }
        var fastDumps = await DumpFrames(ctx, "fast", 30, guideRows, frame => Force(ctx, Triangle(frame, 12, 0.5, Levels)));
        // Every guide and level 0 alone, six frames each.
        for (int frame = 0; frame < 120; ++frame) {
            Force(ctx, frame / 6 % 2 == 0 ? 1.0 : Levels);
            await Step(ctx);
        }
        // Through nothing: 20 frames each way, so level 0 starts blending back to the rigid pose and turns round.
        for (int frame = 0; frame < 160; ++frame) {
            Force(ctx, Triangle(frame, 20, 0.0, Levels) < 0.4 ? 0.0 : Triangle(frame, 20, 0.0, Levels));
            await Step(ctx);
        }
        // Off for half a second, on for half a second.
        for (int frame = 0; frame < 180; ++frame) {
            Force(ctx, frame / 30 % 2 == 0 ? 0.0 : Levels);
            await Step(ctx);
        }
        var offOn = Since(fastStart).TakeLast(170).ToList();
        ctx.Check(offOn.Any(sample => sample.Simulated == 0) && offOn.Any(sample => sample.Simulated == Guides),
            "fast sweep: the instance stops simulating and starts again");
        Force(ctx, Levels);
        await Step(ctx, 60);
        var afterFast = Since(fastStart);
        ctx.Check(afterFast[^1].Simulated == Guides && afterFast[^1].Blending == 0 && afterFast[^1].StretchMax < 0.02,
            $"after the fast sweeps every guide is simulated, none blending, none stretched: {afterFast[^1]}");
        CheckPhase(ctx, "fast sweeps", fastStart, factor: FastTransitionFactor);
        CompareNeighbouringFrames(ctx, "fast sweep, neighbouring frames", fastDumps, pointsPerGuide, FastTransitionFactor);
        await Captured(ctx);

        // The budget: at the highest level, at most 40,000 simulated guide points.
        commands.Invoke("hair.sim", new { lod_point_budget = 40000 });
        await Step(ctx, blendFrames + 30);
        var budgeted = Since(0)[^1];
        ctx.Check(budgeted.Points <= 40000 && budgeted.Points > 36000 && budgeted.Level < Levels,
            $"a budget of 40,000 simulated guide points scales the level down to fit: {budgeted}");
        commands.Invoke("hair.sim", new { lod_point_budget = 0 });
        await Step(ctx, blendFrames + 30);

        // (d) Fidelity: two grooms side by side on heads that swing together, the first with every guide simulated and
        // the second at a lower level. Both heads' transforms reach a frame together, so the two see the same motion.
        _sway = false;
        _head.Position = HeadPosition;
        uint second = commands.Invoke("hair.attach", new { asset, entity = "hair-lod-head-b" }).GetProperty("instance").GetUInt32();
        commands.Invoke("hair.colliders", new { instance = second, colliders = sphere });
        ctx.Camera.LookAt(eye: (1.3, 2.0, 7.0), target: (1.3, 2.3, 0)); // low in the frame, clear of the menu
        await Step(ctx, 30);
        const double Amplitude = 0.25, Period = 1.2;
        int swingFrames = (int)Math.Round(2 * Period * 60.0);
        int[] dumpFrames = { 0, 27, 54, 72, 90, 108, 126, 144, 164, 204 };
        var fidelity = new List<(double Level, double Mean, double Median, double High, double Motion)>();
        foreach (double level in new[] { Levels, 3.0, 2.0, 1.0 }) {
            commands.Invoke("hair.simlevel", new { instance = second, level });
            commands.Invoke("hair.sim", new { reset = true });
            await Step(ctx, 240);
            // Where each groom's guide rows are: both are simulated, so both hold rows, and keep them through the swing.
            int baseA = GuideRowBase(ctx, 0), baseB = GuideRowBase(ctx, (int)second);
            ctx.Require(baseA >= 0 && baseB >= 0 && baseA != baseB, $"both grooms hold guide rows (from rows {baseA} and {baseB})");
            int dumpRows = Math.Max(baseA, baseB) + guideRows;
            var paths = new List<string>();
            for (int frame = 0; frame <= dumpFrames[^1]; ++frame) {
                double time = frame / 60.0;
                double offset = frame >= swingFrames ? 0.0 : Amplitude * Math.Sin(2 * Math.PI * time / Period);
                _head.Position = (HeadPosition.X + offset, HeadPosition.Y, HeadPosition.Z);
                _secondHead.Position = (SecondHeadPosition.X + offset, SecondHeadPosition.Y, SecondHeadPosition.Z);
                if (dumpFrames.Contains(frame)) {
                    paths.Add($"{DumpFolder()}/fidelity_{level:F0}_{frame:D3}.bin");
                    commands.Invoke("hair.dump", new { buffer = "guide_points", path = paths[^1], rows = dumpRows });
                }
                if (frame == 90 && level == 1.0) TryCapture(ctx, "hair_sim_lod_d_full_left_level_1_right");
                await Step(ctx);
            }
            await WaitDumps(ctx);
            // A point's offset from its rigid pose, in the second groom against the first. The first dump is the settled
            // hair before the swing; the motion is how far the first groom's points are from where they were then.
            ctx.Require(GuideRowBase(ctx, 0) == baseA && GuideRowBase(ctx, (int)second) == baseB, "both grooms kept their guide rows through the swing");
            var dumps = paths.Select(path => ReadDump(path, dumpRows)).ToList();
            double settledError = 0, sum = 0, largest = 0, motionSum = 0;
            var errors = new List<float>();
            for (int index = 0; index < dumps.Count; ++index) {
                var dump = dumps[index];
                double frameSum = 0;
                for (int row = 0; row < guideRows; ++row) {
                    int a = (row + baseA) * 3, b = (row + baseB) * 3;
                    double ex = 0, mx = 0;
                    for (int axis = 0; axis < 3; ++axis) {
                        double first = dump.Position[a + axis] - dump.Rigid[a + axis];
                        double difference = dump.Position[b + axis] - dump.Rigid[b + axis] - first;
                        double moved = first - (dumps[0].Position[a + axis] - dumps[0].Rigid[a + axis]);
                        ex += difference * difference;
                        mx += moved * moved;
                    }
                    double error = Math.Sqrt(ex);
                    frameSum += error;
                    if (index == 0) continue;
                    errors.Add((float)error);
                    largest = Math.Max(largest, error);
                    motionSum += Math.Sqrt(mx);
                }
                if (index == 0) settledError = frameSum / guideRows; else sum += frameSum;
            }
            errors.Sort();
            double mean = sum / errors.Count, motion = motionSum / errors.Count;
            fidelity.Add((level, mean, errors[errors.Count / 2], errors[errors.Count * 95 / 100], motion));
            ctx.Log($"fidelity at level {level:F0} against every guide simulated, over {dumps.Count - 1} frames of a swing: guide points differ by {mean * 1000:F3} mm on average, " +
                $"{errors[errors.Count / 2] * 1000:F3} mm median, {errors[errors.Count * 95 / 100] * 1000:F3} mm at the 95th percentile, {largest * 1000:F2} mm at most; " +
                $"settled before the swing {settledError * 1000:F3} mm on average; the fully simulated points are {motion * 1000:F2} mm from their settled place on average");
        }
        // Not the mean: a few guides in contact with the collider part ways between two grooms a few metres apart, by
        // centimetres, and carry it.
        ctx.Check(fidelity[0].Median < 5e-5 && fidelity[0].High < 1e-3,
            $"two grooms at the same level move alike (guide points {fidelity[0].Median * 1000:F4} mm apart at the median, {fidelity[0].High * 1000:F3} mm at the 95th percentile)");
        ctx.Check(fidelity.Skip(1).All(row => row.Mean < row.Motion), "at every reduced level the guides are nearer the fully simulated ones than those are to their settled place");
        ctx.Check(fidelity[1].Mean <= fidelity[2].Mean && fidelity[2].Mean <= fidelity[3].Mean,
            $"the difference grows as the level falls ({string.Join(", ", fidelity.Skip(1).Select(row => $"level {row.Level:F0}: {row.Mean * 1000:F3} mm"))})");
        await Captured(ctx);

        ctx.Check(_outOfViewRows == 0 && _inViewRows > 0, $"the grooms were in view whenever their metrics were read ({_inViewRows} rows in view, {_outOfViewRows} out of it)");
        ctx.Check(_staleGuides == 0 && _starvedRows == 0, $"every guide went on from its own rows of the frame before, and no groom went without guide rows ({_staleGuides} stale guides, {_starvedRows} starved rows)");
        var status = commands.Invoke("hair.sim", new { enabled = false, lod_force_level = -1.0 });
        ctx.Check(status.GetProperty("capped_frames").GetUInt64() == 0, "no frame hit the step cap");
        commands.Invoke("hair.stats", new { continuous = false });
    }
}
