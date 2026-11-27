// Strand hair simulation (docs/hair.md): guides simulated on the GPU, collision with analytic colliders, and render
// strands interpolated from their guides. A drooping procedural groom sits on a sphere with a matching sphere collider.
// The scenario reads the simulation's metrics of every frame (hair.sim) and checks that
//   (a) with the entity at rest the hair settles without entering the collider;
//   (b) while the entity swings from side to side the hair leaves its settled shape, keeps moving after the entity
//       stops, and settles again into the same shape;
//   (c) wind moves it and it settles again when the wind stops;
//   (d) with the simulation off again the frames are the rigid ones from before, by the rasterizer's counters;
//   (e) Cem Yuksel's wStraight.hair settles on its head sphere, when HAIR_SIM_WSTRAIGHT names the file (skipped
//       otherwise: the models are not in the repository).
// It captures the interesting moments (hair_sim_*.bmp in HAIR_CAPTURE_DIR, or in hair_captures in the temporary
// directory).
//   BasicRenderer.exe --script scenarios/hair/hair_sim.cs --script-exit --script-fixed-dt 0.0166667
// Needs a host with the hair plugin (the demo), in raster mode. With a fixed time step the run is deterministic: two
// runs log the same numbers.

[Scenario("hair-sim", TimeoutSeconds = 900, Description = "Guide simulation, collision and interpolation: settle, swing, wind, and back to rigid.")]
public sealed class HairSim : Scenario
{
    // Settled: the guides' mean speed stays under this for SettledSteps simulation steps.
    const double SettleSpeed = 0.003;
    const ulong SettledSteps = 30;
    // The most a segment may be off its rest length, as a share of it, at any time.
    const double StretchLimit = 0.02;
    // And while the entity moves, which it does in jumps (CheckPhase).
    const double MovingStretchLimit = 0.10;
    // Root positions are compared in world space, a few metres from the origin, in single precision.
    const double RootTolerance = 1e-4;

    static readonly Vec3 HeadPosition = (0, 1.5, 0);

    sealed record Sample(ulong Frame, ulong Steps, uint Guides, double StretchMax, double StretchMean, uint Stretched, double SpeedMean, double SpeedMax,
        double Deviation, double ColliderDistance, uint GuideRoots, double GuideRootError, uint StrandRoots, double StrandRootError,
        uint ResetGuides, double RootShift)
    {
        public override string ToString() =>
            $"frame={Frame} steps={Steps} stretch_max={StretchMax:E3} stretch_mean={StretchMean:E3} speed_mean={SpeedMean:F5} speed_max={SpeedMax:F4} " +
            $"deviation={Deviation:F5} collider_distance={ColliderDistance:F6} guide_root_error={GuideRootError:E2} strand_root_error={StrandRootError:E2}";
    }

    // Where hair.capture writes: HAIR_CAPTURE_DIR, or a folder in the system's temporary directory.
    static string CaptureFolder()
    {
        string folder = Environment.GetEnvironmentVariable("HAIR_CAPTURE_DIR") ?? System.IO.Path.Combine(System.IO.Path.GetTempPath(), "hair_captures");
        System.IO.Directory.CreateDirectory(folder);
        return folder.Replace('\\', '/');
    }

    readonly SortedDictionary<ulong, Sample> _samples = new();
    readonly List<string> _captures = new();

    // Adds the newest metrics read back to the samples. A frame's metrics arrive a few frames after it, one frame's
    // worth per frame, so calling this once a frame collects every frame.
    void Collect(ScenarioContext ctx)
    {
        foreach (var row in ctx.Commands.Invoke("hair.sim").GetProperty("metrics").EnumerateArray()) {
            double distance = row.TryGetProperty("collider_distance", out var value) ? value.GetDouble() : double.PositiveInfinity;
            var sample = new Sample(row.GetProperty("frame").GetUInt64(), row.GetProperty("steps").GetUInt64(), row.GetProperty("guides").GetUInt32(),
                row.GetProperty("stretch_max").GetDouble(), row.GetProperty("stretch_mean").GetDouble(),
                row.GetProperty("stretched_guides").GetUInt32(), row.GetProperty("speed_mean").GetDouble(),
                row.GetProperty("speed_max").GetDouble(), row.GetProperty("deviation_mean").GetDouble(), distance,
                row.GetProperty("guide_roots").GetUInt32(), row.GetProperty("guide_root_error").GetDouble(),
                row.GetProperty("strand_roots").GetUInt32(), row.GetProperty("strand_root_error").GetDouble(),
                row.GetProperty("reset_guides").GetUInt32(), row.GetProperty("root_shift").GetDouble());
            _samples[sample.Frame] = sample;
        }
    }

    async Task Step(ScenarioContext ctx, int frames = 1)
    {
        for (int frame = 0; frame < frames; ++frame) {
            await ctx.NextFrame();
            Collect(ctx);
        }
    }

    ulong NewestFrame => _samples.Count == 0 ? 0 : _samples.Keys.Max();

    List<Sample> Since(ulong frame) => _samples.Values.Where(sample => sample.Frame > frame).ToList();

    // Asks for a capture of the presented frame; Captured checks later that every file was written. A capture takes a
    // few frames, and the scenario does not hold the scene still for it.
    async Task Capture(ScenarioContext ctx, string name)
    {
        string path = CaptureFolder() + "/" + name + ".bmp";
        // One capture at a time.
        for (int frame = 0; frame < 600 && ctx.Commands.Invoke("hair.stats").GetProperty("capture").GetProperty("state").GetString() == "pending"; ++frame) {
            await Step(ctx);
        }
        ctx.Commands.Invoke("hair.capture", new { path });
        _captures.Add(path);
    }

    async Task Captured(ScenarioContext ctx)
    {
        for (int frame = 0; frame < 600 && ctx.Commands.Invoke("hair.stats").GetProperty("capture").GetProperty("state").GetString() == "pending"; ++frame) {
            await Step(ctx);
        }
        foreach (string path in _captures) ctx.Check(System.IO.File.Exists(path), $"captured {path}");
        _captures.Clear();
    }

    // Runs until the hair has settled, and returns the last sample: until the frames read back end with a run under
    // SettleSpeed that covers SettledSteps simulation steps. The settle time is from the first sample after `since` to
    // the first frame of that run, in simulation steps. A frame takes one step with a fixed 1/60 s frame.
    async Task<Sample> Settle(ScenarioContext ctx, string what, ulong since, double limitSeconds)
    {
        int limitFrames = (int)(limitSeconds * 60.0);
        for (int frame = 0; frame < limitFrames; ++frame) {
            await Step(ctx);
            var samples = Since(since);
            int run = 0;
            for (int i = samples.Count - 1; i >= 0 && samples[i].SpeedMean < SettleSpeed; --i) ++run;
            if (run < 4 || samples[^1].Steps < samples[samples.Count - run].Steps + SettledSteps - 1) continue;
            // The first frame of the settled run that the samples end with.
            var first = samples[samples.Count - run];
            var last = samples[^1];
            // A reset starts the step count again, a frame or two into the phase.
            ulong steps = first.Steps - samples.TakeWhile(sample => sample.Frame <= first.Frame).Min(sample => sample.Steps) + 1;
            ctx.Check(true, $"{what}: settled after {steps} steps ({steps / 60.0:F2} simulated seconds): {last}");
            return last;
        }
        var newest = Since(since).LastOrDefault();
        ctx.Fail($"{what}: the hair did not settle in {limitFrames} frames: {newest}");
        throw new InvalidOperationException();
    }

    // The checks every simulated phase shares, over its frames.
    // `entityMoves`: the entity moved in this phase. A frame draws the scene's last published version, which can be the
    // one the frame before drew, or skip one, so the entity (the roots and the colliders) then moves in jumps of two or
    // three frames' travel, differently in every run. A collider that runs into hair that fast can stretch it for a
    // frame, so while the entity moves the limit on the worst segment is wider, and the rest is on how many guides and
    // frames it happens to.
    void CheckPhase(ScenarioContext ctx, string what, ulong since, bool entityMoves = false)
    {
        var samples = Since(since);
        ctx.Require(samples.Count > 0, $"{what}: metrics were read back");
        double stretch = samples.Max(sample => sample.StretchMax);
        double stretchMean = samples.Max(sample => sample.StretchMean);
        double distance = samples.Min(sample => sample.ColliderDistance);
        double guideRoot = samples.Max(sample => sample.GuideRootError);
        double strandRoot = samples.Max(sample => sample.StrandRootError);
        ctx.Log($"{what}: {samples.Count} frames; largest stretch error {stretch:E3} (mean {stretchMean:E3}); smallest collider distance {distance:F6} m; " +
            $"largest root error before the snap: guides {guideRoot:E2} m, interpolated strands {strandRoot:E2} m; " +
            $"fastest point {samples.Max(sample => sample.SpeedMax):F3} m/s");
        var over = samples.Where(sample => sample.Stretched > 0).ToList();
        ctx.Log($"{what}: {over.Count} frames with a guide over {StretchLimit:P0}; most such guides in a frame {samples.Max(sample => sample.Stretched)} of {samples[0].Guides}; " +
            "worst frames (frames into the phase: stretch, guides over): " +
            string.Join(", ", over.OrderByDescending(sample => sample.StretchMax).Take(8).Select(sample => $"{sample.Frame - since}: {sample.StretchMax:P1}, {sample.Stretched}")));
        var shifted = samples.Where(sample => sample.RootShift > 1e-4).ToList();
        var shiftedFrames = shifted.Select(sample => sample.Frame).ToHashSet();
        int explained = over.Count(sample => Enumerable.Range(0, 4).Any(back => shiftedFrames.Contains(sample.Frame - (ulong)back)));
        if (entityMoves) {
            uint most = samples.Max(sample => sample.Stretched);
            ctx.Log($"{what}: {explained} of the {over.Count} frames with a guide over {StretchLimit:P0} began with the entity in a jump, or came within 3 frames of one that did");
            ctx.Check(stretch < MovingStretchLimit, $"{what}: no guide segment is more than {MovingStretchLimit:P0} off its rest length ({stretch:P3})");
            ctx.Check(over.Count * 10 <= samples.Count && most * 100 <= samples[0].Guides,
                $"{what}: at most a hundredth of the guides have a segment more than {StretchLimit:P0} off its rest length, in at most a tenth of the frames " +
                $"({most} of {samples[0].Guides} guides at worst, in {over.Count} of {samples.Count} frames)");
            ctx.Check(stretchMean < StretchLimit / 2, $"{what}: the guides' mean stretch error stays under {StretchLimit / 2:P0} ({stretchMean:P3})");
        } else {
            ctx.Check(shifted.Count == 0, $"{what}: the roots moved from frame to frame without a jump");
            ctx.Check(stretch < StretchLimit, $"{what}: no guide segment is more than {StretchLimit:P0} off its rest length ({stretch:P3})");
        }
        ctx.Log($"{what}: {shifted.Count} frames began with a root off its previous root frame (by up to {samples.Max(sample => sample.RootShift) * 1000:F2} mm): " +
            string.Join(", ", shifted.Take(12).Select(sample => $"{sample.Frame - since}: {sample.RootShift * 1000:F1} mm")));
        ctx.Check(distance >= 0.0, $"{what}: no simulated guide point is inside a collider (nearest {distance * 1000:F3} mm)");
        ctx.Check(samples.All(sample => sample.GuideRoots > 0 && sample.StrandRoots > 0), $"{what}: guides and interpolated strands are drawn in every frame");
        ctx.Check(guideRoot < RootTolerance && strandRoot < RootTolerance, $"{what}: evaluated roots are within {RootTolerance} m of their root frames");
        ctx.Check(samples.All(sample => !double.IsNaN(sample.SpeedMean) && !double.IsNaN(sample.Deviation) && sample.SpeedMax < 100.0),
            $"{what}: no guide point is lost (speeds stay finite)");
    }

    // The rasterizer's counters of the last frames read back (the plugin keeps 32), after enough frames that all of them
    // are new, sorted. The camera is jittered for the upscaler, so a still scene's counters repeat with the jitter
    // sequence and not from frame to frame: sorted, two runs of 32 consecutive frames are equal when the sequence's
    // length divides 32 (DLSS: 16).
    static async Task<List<(uint Strands, uint Segments, uint Drawn, uint Written, uint Resolved)>> FrameCounters(ScenarioContext ctx)
    {
        await ctx.Frames(48);
        var frames = ctx.Commands.Invoke("hair.stats").GetProperty("gpu_history").EnumerateArray().ToList();
        ctx.Check(frames.Count == 32 && frames[^1].GetProperty("frame").GetUInt64() - frames[0].GetProperty("frame").GetUInt64() == 31,
            $"the counters of 32 consecutive frames were read back ({frames.Count})");
        return frames.Select(frame => (frame.GetProperty("strands_selected").GetUInt32(), frame.GetProperty("segments_selected").GetUInt32(),
            frame.GetProperty("segments_drawn").GetUInt32(), frame.GetProperty("pixels_written").GetUInt32(),
            frame.GetProperty("pixels_resolved").GetUInt32())).OrderBy(counters => counters).ToList();
    }

    public override async Task Run(ScenarioContext ctx)
    {
        // Hair is a frame graph extension, and the path-traced pipeline (the default where the device supports it)
        // has none. The setting is restored when the scenario ends.
        ctx.Settings["enablePathTracing"] = false;

        var scene = ctx.Scene;
        scene.DirectionalLight(direction: (0.5, -0.7, -0.8), intensity: 4f);
        var head = scene.Instantiate("../assets/sphere.glb", position: HeadPosition, name: "hair-sim-head");
        var floor = scene.Instantiate("../assets/plane.glb", position: (0, 0, 0), name: "hair-sim-floor");
        await Task.WhenAll(head.WhenReady(), floor.WhenReady());
        ctx.Camera.Fov(50).LookAt(eye: (0, 1.7, 4.2), target: (0, 1.2, 0));

        var commands = ctx.Commands;
        commands.Invoke("hair.clear");
        commands.Invoke("hair.lod", new { error_pixels = 1.0, max_width_scale = 8.0, min_detail = 0.02, force_detail = 0.0, segment_pixels = 4.0, freeze = false });
        commands.Invoke("hair.debug", new { view = "off" });
        // Every control, so an earlier session's values do not carry over. The simulation level is forced to every guide:
        // this scenario is about the solver, and hair_sim_lod.cs about the level.
        var defaults = commands.Invoke("hair.sim", new {
            enabled = false, gravity = new[] { 0.0, -9.81, 0.0 }, damping = 3.0, stretch_compliance = 0.0, bend_compliance = 1e-5, shape_compliance = 0.01,
            step_seconds = 1.0 / 60.0, substeps = 4, max_steps = 4, wind = new[] { 0.0, 0.0, 0.0 }, gust_amplitude = 0.5, gust_frequency = 0.7,
            friction = 0.3, collision_margin = 0.002, teleport_distance = 1.0,
            lod_full_coverage = 1.0, lod_level0_coverage = 0.125, lod_off_coverage = 0.03, lod_rate = 2.0, lod_force_level = 8.0, lod_point_budget = 0,
            promote_seconds = 0.25, demote_seconds = 0.25,
        });
        uint asset = commands.Invoke("hair.load", new {
            procedural = new { style = "wavy", strands = 20000, seed = 5, radius = 0.5, length = 0.9, droop = 6.0, wave_amplitude = 0.012, wave_count = 5.0,
                root_width = 0.0004, tip_width = 0.0002, points = 33 },
        }).GetProperty("asset").GetUInt32();
        uint instance = commands.Invoke("hair.attach", new { asset, entity = "hair-sim-head" }).GetProperty("instance").GetUInt32();
        // The sphere the groom grows on, in the entity's mesh space.
        commands.Invoke("hair.colliders", new { instance, colliders = new[] { new { center = new[] { 0.0, 0.0, 0.0 }, radius = 0.5 } } });
        commands.Invoke("hair.stats", new { continuous = true });
        await ctx.WaitUntilStable();
        await ctx.Frames(30);

        var loaded = commands.Invoke("hair.stats");
        ctx.Require(loaded.GetProperty("active_instances").GetUInt32() == 1, "the groom is attached and dispatched");
        ctx.Log($"groom: {loaded.GetProperty("strands")} strands, {loaded.GetProperty("rows").GetProperty("guides").GetProperty("used")} guides with " +
            $"{loaded.GetProperty("rows").GetProperty("guide_points").GetProperty("used")} points; simulation defaults {defaults}");
        var rigid = await FrameCounters(ctx);
        ctx.Log($"rigid: {rigid[0]} to {rigid[^1]}");
        ctx.Check(rigid.All(frame => frame.Written > 0 && frame.Resolved > 0), "the rigid groom is drawn");
        await Capture(ctx, "hair_sim_0_rigid");
        await Captured(ctx);

        // (a) At rest.
        commands.Invoke("hair.sim", new { enabled = true });
        ulong start = NewestFrame;
        await Step(ctx, 8);
        var firstFrames = Since(start);
        ctx.Require(firstFrames.Count > 0, "the simulation's metrics are read back");
        ctx.Check(firstFrames[0].ResetGuides == firstFrames[0].Guides && firstFrames[0].Guides > 0,
            $"every guide starts on its rest pose ({firstFrames[0].ResetGuides} of {firstFrames[0].Guides})");
        ctx.Check(firstFrames.Skip(1).All(sample => sample.ResetGuides == 0), "and is not reset again");
        var settled = await Settle(ctx, "at rest", start, limitSeconds: 12);
        CheckPhase(ctx, "at rest", start);
        ctx.Check(settled.Deviation > 0.002, $"gravity moved the hair off its rigid pose ({settled.Deviation * 1000:F2} mm on average)");
        bool fixedStep = Math.Abs(ctx.DeltaTime - 1.0 / 60.0) < 1e-5;
        if (fixedStep) {
            var atRest = Since(start);
            ctx.Check(atRest.Zip(atRest.Skip(1)).All(pair => pair.Second.Steps - pair.First.Steps == pair.Second.Frame - pair.First.Frame),
                "with a fixed 1/60 s frame the simulation takes one step a frame");
        }
        await Capture(ctx, "hair_sim_1_settled");
        await Captured(ctx);

        // (b) The entity swings from side to side, twice, and stops where it started.
        const double Amplitude = 0.35, Period = 1.2;
        ulong swingStart = NewestFrame;
        double swingBegan = ctx.Time;
        bool swingCaptured = false;
        for (int frame = 1; ; ++frame) {
            // By frame count with a fixed frame, so that the positions are the same in every run.
            double time = fixedStep ? frame / 60.0 : ctx.Time - swingBegan;
            bool last = time >= 2 * Period - 1e-9;
            head.Position = last ? HeadPosition : (HeadPosition.X + Amplitude * Math.Sin(2 * Math.PI * time / Period), HeadPosition.Y, HeadPosition.Z);
            await Step(ctx);
            // Moving fastest, through the middle, the second time.
            if (!swingCaptured && time >= 1.5 * Period - 1e-9) {
                swingCaptured = true;
                await Capture(ctx, "hair_sim_2_swing");
            }
            if (last) break;
        }
        await Step(ctx, 8);
        var swing = Since(swingStart);
        double swingDeviation = swing.Max(sample => Math.Abs(sample.Deviation - settled.Deviation));
        ctx.Log($"swing: deviation from the rigid pose {swing.Min(sample => sample.Deviation):F4} to {swing.Max(sample => sample.Deviation):F4} m (settled {settled.Deviation:F4})");
        ctx.Check(swingDeviation > 0.005, $"the hair lags the moving entity ({swingDeviation * 1000:F1} mm off its settled shape, on average over the guides)");
        ulong stopped = NewestFrame;
        await Step(ctx, 10);
        var afterStop = Since(stopped);
        ctx.Check(afterStop.Max(sample => sample.SpeedMean) > SettleSpeed, $"the hair keeps swinging after the entity stops ({afterStop.Max(sample => sample.SpeedMean):F3} m/s)");
        var settledAgain = await Settle(ctx, "after the swing", stopped, limitSeconds: 12);
        CheckPhase(ctx, "swing", swingStart, entityMoves: true);
        ctx.Check(settledAgain.StretchMax < StretchLimit, $"settled after the swing, no guide segment is more than {StretchLimit:P0} off its rest length ({settledAgain.StretchMax:P4})");
        ctx.Check(Math.Abs(settledAgain.Deviation - settled.Deviation) < 0.003,
            $"the hair settles back into the same shape ({settledAgain.Deviation * 1000:F2} mm against {settled.Deviation * 1000:F2} mm off the rigid pose)");
        await Capture(ctx, "hair_sim_3_settled_again");
        await Captured(ctx);

        // (c) Wind on, then off. From a reset, so that this part does not depend on how the swing went: the entity's
        // jumps differ from run to run, and the hair with them.
        commands.Invoke("hair.sim", new { reset = true });
        ulong resetFrame = NewestFrame;
        await Step(ctx, 8);
        ctx.Check(Since(resetFrame).Any(sample => sample.ResetGuides == sample.Guides), "a reset puts every guide back on its rest pose");
        var settledForWind = await Settle(ctx, "after a reset", resetFrame, limitSeconds: 12);
        ctx.Check(Math.Abs(settledForWind.Deviation - settled.Deviation) < 1e-4,
            $"and it settles as it did at first ({settledForWind.Deviation * 1000:F3} mm against {settled.Deviation * 1000:F3} mm off the rigid pose)");
        commands.Invoke("hair.sim", new { wind = new[] { 5.0, 0.0, 1.5 } });
        ulong windStart = NewestFrame;
        await Step(ctx, 180);
        await Capture(ctx, "hair_sim_4_wind");
        await Step(ctx, 30);
        var wind = Since(windStart);
        double windDeviation = wind.Max(sample => Math.Abs(sample.Deviation - settled.Deviation));
        ctx.Check(windDeviation > 0.005, $"the wind moves the hair ({windDeviation * 1000:F1} mm off its settled shape)");
        ctx.Check(wind.TakeLast(30).Max(sample => sample.SpeedMean) > SettleSpeed, "gusts keep it moving");
        commands.Invoke("hair.sim", new { wind = new[] { 0.0, 0.0, 0.0 } });
        ulong windStopped = NewestFrame;
        var afterWind = await Settle(ctx, "after the wind", windStopped, limitSeconds: 12);
        CheckPhase(ctx, "wind", windStart);
        ctx.Check(Math.Abs(afterWind.Deviation - settled.Deviation) < 0.003,
            $"the hair settles back into the same shape after the wind ({afterWind.Deviation * 1000:F2} mm against {settled.Deviation * 1000:F2} mm)");
        await Captured(ctx);

        // (d) Off again: the rigid frame from before.
        var status = commands.Invoke("hair.sim", new { enabled = false });
        ctx.Log($"simulation: {status.GetProperty("steps")} steps, {status.GetProperty("simulated_seconds").GetDouble():F2} s simulated, " +
            $"{status.GetProperty("dropped_seconds").GetDouble():F3} s dropped in {status.GetProperty("capped_frames")} capped frames");
        if (fixedStep) ctx.Check(status.GetProperty("capped_frames").GetUInt64() == 0, "no frame hit the step cap");
        var rigidAgain = await FrameCounters(ctx);
        // The same strands and segments exactly, and the same pixels to one part in 10,000: the entity has moved and come
        // back since `rigid` was read, and a rigid groom whose entity is moved and put back draws a few pixels in five
        // million differently from then on, with no simulation at all (the entity's row is not bit for bit what it was).
        static bool Close(uint a, uint b) => Math.Abs((double)a - b) <= 1e-4 * Math.Max(a, b);
        ctx.Check(rigidAgain.Count == rigid.Count && rigidAgain.Zip(rigid).All(pair => pair.First.Strands == pair.Second.Strands &&
                pair.First.Segments == pair.Second.Segments && Close(pair.First.Drawn, pair.Second.Drawn) &&
                Close(pair.First.Written, pair.Second.Written) && Close(pair.First.Resolved, pair.Second.Resolved)),
            $"with the simulation off the rasterizer's counters over 32 frames are the rigid ones again ({rigidAgain[0]} to {rigidAgain[^1]})");
        await Capture(ctx, "hair_sim_5_rigid_again");
        await Captured(ctx);

        // (e) A reference model on its head sphere.
        string? model = Environment.GetEnvironmentVariable("HAIR_SIM_WSTRAIGHT");
        if (string.IsNullOrEmpty(model) || !System.IO.File.Exists(model)) {
            ctx.Log("wStraight: skipped (HAIR_SIM_WSTRAIGHT does not name a file)");
            return;
        }
        commands.Invoke("hair.clear");
        head.Destroy();
        // The file is Z-up, about 0.003 m a unit; its roots lie on a sphere of radius 30 units around (-11.3, 0, 29.5),
        // the face along +x. The head is a sphere of 26 units, so the roots are outside it, with a neck below.
        const double Unit = 0.003, HeadRadius = 26.0;
        float entityScale = (float)(HeadRadius * Unit / 0.5);
        double restScale = Unit / entityScale;
        var skull = scene.Instantiate("../assets/sphere.glb", position: HeadPosition, name: "hair-sim-skull").Scaled(entityScale);
        await skull.WhenReady();
        uint straight = commands.Invoke("hair.load", new {
            path = model.Replace('\\', '/'), unit_scale = restScale, offset = new[] { 11.3 * restScale, -29.5 * restScale, 0.0 },
        }).GetProperty("asset").GetUInt32();
        uint straightInstance = commands.Invoke("hair.attach", new { asset = straight, entity = "hair-sim-skull", color = new[] { 0.30, 0.22, 0.12 } })
            .GetProperty("instance").GetUInt32();
        commands.Invoke("hair.colliders", new {
            instance = straightInstance,
            colliders = new object[] {
                new { center = new[] { 0.0, 0.0, 0.0 }, radius = 0.5 },
                new { a = new[] { 0.0, -0.45, 0.0 }, b = new[] { 0.0, -1.6, 0.0 }, radius = 0.28 },
            },
        });
        ctx.Camera.LookAt(eye: (0.42, 1.52, 0.42), target: (0, 1.43, 0));
        await ctx.WaitUntilStable();
        await ctx.Frames(30);
        await Capture(ctx, "hair_sim_6_wstraight_rigid");
        await Captured(ctx);
        commands.Invoke("hair.sim", new { enabled = true });
        ulong straightStart = NewestFrame;
        var straightSettled = await Settle(ctx, "wStraight", straightStart, limitSeconds: 15);
        CheckPhase(ctx, "wStraight", straightStart);
        await Capture(ctx, "hair_sim_7_wstraight_settled");
        ctx.Camera.LookAt(eye: (-0.45, 1.55, -0.35), target: (0, 1.43, 0));
        await Step(ctx, 45);
        await Capture(ctx, "hair_sim_8_wstraight_settled_back");
        await Captured(ctx);
        commands.Invoke("hair.sim", new { enabled = false, lod_force_level = -1.0 });
        commands.Invoke("hair.stats", new { continuous = false });
    }
}
