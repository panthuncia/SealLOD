// Strand hair (docs/hair.md): many grooms. An N x N grid of heads, each with a groom, sharing four groom assets. The
// camera stands in front of the grid, flies down an aisle through it, and looks back at it from far behind. The scenario
// records every frame's counters (instances and clusters in view, drawn strands and segments, strand pool use, pixels
// written and resolved, simulated guides and guide points, and how the instances' simulation levels are spread) as
// checks named "sample ...", and captures the crowd from near, from the middle and from far (hair_crowd_*.bmp in
// HAIR_CAPTURE_DIR, or in hair_captures in the temporary directory).
//   BasicRenderer.exe --script scenarios/hair/hair_crowd.cs --script-exit --script-fixed-dt 0.0166667
// Environment: HAIR_CROWD_N (grid side, default 10), HAIR_CROWD_SPACING (metres between heads, default 2.5),
// HAIR_CROWD_SIMULATE (default 1: the grooms are simulated at the level their size on screen gives them).
// Needs a host with the hair plugin (the demo), in raster mode.

[Scenario("hair-crowd", TimeoutSeconds = 1500, Description = "An N x N grid of grooms sharing four assets; counters along a fly-through.")]
public sealed class HairCrowd : Scenario
{
    const int FlyFrames = 360;
    const double HeadHeight = 1.6;
    const float HeadScale = 0.22f; // sphere.glb has radius 0.5: an 11 cm head

    sealed record Sample(ulong Frame, uint Instances, uint ClustersTested, uint Clusters, uint Strands, uint Segments, uint PoolVertices,
        uint Overflow, uint SegmentsDrawn, uint PixelsWritten, uint PixelsResolved, uint Capped, uint StaleGuides, uint UnseenStrands)
    {
        public override string ToString() =>
            $"frame={Frame} instances_visible={Instances} clusters_visible={Clusters}/{ClustersTested} strands={Strands} segments={Segments} " +
            $"pool_vertices={PoolVertices} overflow={Overflow} segments_drawn={SegmentsDrawn} pixels_written={PixelsWritten} " +
            $"pixels_resolved={PixelsResolved} capped={Capped}";
    }

    // The simulation of one frame, over every instance: how many are simulated, their guides and guide points, and the
    // instances by simulation level (off, up to 1, up to 2, ..., and every guide).
    sealed record SimSample(ulong Frame, uint Simulated, uint Guides, uint Points, uint Evaluated, uint StaleGuides, uint Starved, int[] Levels)
    {
        public override string ToString() =>
            $"sim_instances={Simulated} sim_guides={Guides} sim_points={Points} evaluated_guides={Evaluated} sfrac_off|<=1|<=2|..|full=[{string.Join(",", Levels)}]";
    }

    readonly SortedDictionary<ulong, Sample> _samples = new();
    readonly SortedDictionary<ulong, SimSample> _simSamples = new();
    readonly List<Entity> _heads = new();
    readonly List<Vec3> _headPositions = new();
    bool _simulate;
    int _swayFrame;

    static string CaptureFolder()
    {
        string folder = Environment.GetEnvironmentVariable("HAIR_CAPTURE_DIR") ?? System.IO.Path.Combine(System.IO.Path.GetTempPath(), "hair_captures");
        System.IO.Directory.CreateDirectory(folder);
        return folder.Replace('\\', '/');
    }

    static double EnvNumber(string name, double fallback) =>
        double.TryParse(Environment.GetEnvironmentVariable(name), System.Globalization.NumberStyles.Float,
            System.Globalization.CultureInfo.InvariantCulture, out double value) ? value : fallback;

    void Collect(ScenarioContext ctx)
    {
        foreach (var frame in ctx.Commands.Invoke("hair.stats").GetProperty("gpu_history").EnumerateArray()) {
            if (!frame.GetProperty("valid").GetBoolean()) continue;
            var sample = new Sample(frame.GetProperty("frame").GetUInt64(), frame.GetProperty("instances_visible").GetUInt32(),
                frame.GetProperty("clusters_tested").GetUInt32(), frame.GetProperty("clusters_visible").GetUInt32(),
                frame.GetProperty("strands_selected").GetUInt32(), frame.GetProperty("segments_selected").GetUInt32(),
                frame.GetProperty("pool_vertices").GetProperty("used").GetUInt32(), frame.GetProperty("overflow_clusters").GetUInt32(),
                frame.GetProperty("segments_drawn").GetUInt32(), frame.GetProperty("pixels_written").GetUInt32(),
                frame.GetProperty("pixels_resolved").GetUInt32(), frame.GetProperty("segments_capped").GetUInt32(),
                frame.GetProperty("stale_guides_drawn").GetUInt32(), frame.GetProperty("unseen_strands_drawn").GetUInt32());
            _samples[sample.Frame] = sample;
        }
        if (!_simulate) return;
        // Every instance's newest metrics; they are of one frame once every instance has been read back for it.
        var byFrame = new Dictionary<ulong, List<System.Text.Json.JsonElement>>();
        foreach (var row in ctx.Commands.Invoke("hair.sim").GetProperty("metrics").EnumerateArray()) {
            ulong frame = row.GetProperty("frame").GetUInt64();
            if (!byFrame.TryGetValue(frame, out var rows)) byFrame[frame] = rows = new();
            rows.Add(row);
        }
        foreach (var (frame, rows) in byFrame) {
            if (rows.Count != _heads.Count) continue;
            int levelCount = rows.Max(row => row.GetProperty("level_guides").GetArrayLength());
            var levels = new int[levelCount + 2];
            uint simulated = 0, guides = 0, points = 0, evaluated = 0, staleGuides = 0, starved = 0;
            foreach (var row in rows) {
                staleGuides += row.GetProperty("stale_guides").GetUInt32();
                starved += row.GetProperty("starved").GetBoolean() ? 1u : 0u;
                double level = row.GetProperty("sim_level").GetDouble();
                uint rowGuides = row.GetProperty("guides").GetUInt32();
                simulated += rowGuides > 0 ? 1u : 0u;
                guides += rowGuides;
                points += row.GetProperty("simulated_points").GetUInt32();
                evaluated += row.GetProperty("evaluated_guides").GetUInt32();
                int bin = level <= 0 ? 0 : level >= row.GetProperty("level_guides").GetArrayLength() ? levelCount + 1 : (int)Math.Ceiling(level);
                ++levels[Math.Min(bin, levelCount + 1)];
            }
            _simSamples[frame] = new SimSample(frame, simulated, guides, points, evaluated, staleGuides, starved, levels);
        }
    }

    // One frame. With the simulation on the heads sway 2 cm to each side every two seconds, so the hair is never still.
    async Task Step(ScenarioContext ctx, int frames = 1)
    {
        for (int frame = 0; frame < frames; ++frame) {
            if (_simulate) {
                ++_swayFrame;
                double sway = 0.02 * Math.Sin(2 * Math.PI * _swayFrame / 120.0);
                for (int head = 0; head < _heads.Count; ++head) {
                    var position = _headPositions[head];
                    _heads[head].Position = (position.X + sway, position.Y, position.Z);
                }
            }
            await ctx.NextFrame();
            Collect(ctx);
        }
    }

    ulong NewestFrame => _samples.Count == 0 ? 0 : _samples.Keys.Max();

    // Holds the camera for `frames` frames and returns the newest frame's counters, logged with the simulation's.
    async Task<Sample> Station(ScenarioContext ctx, string name, Vec3 eye, Vec3 target, int frames = 150)
    {
        ctx.Camera.LookAt(eye, target);
        await Step(ctx, frames);
        var sample = _samples[NewestFrame];
        string sim = _simSamples.Count > 0 ? " " + _simSamples.Values.Last() : "";
        ctx.Log($"station {name}: {sample}{sim}");
        return sample;
    }

    async Task Capture(ScenarioContext ctx, string name)
    {
        string path = CaptureFolder() + "/" + name + ".bmp";
        ctx.Commands.Invoke("hair.capture", new { path });
        for (int frame = 0; frame < 600; ++frame) {
            await Step(ctx);
            if (ctx.Commands.Invoke("hair.stats").GetProperty("capture").GetProperty("state").GetString() != "pending") break;
        }
        ctx.Check(System.IO.File.Exists(path), $"captured {path}");
    }

    public override async Task Run(ScenarioContext ctx)
    {
        int n = (int)EnvNumber("HAIR_CROWD_N", 10);
        double spacing = EnvNumber("HAIR_CROWD_SPACING", 2.5);
        _simulate = EnvNumber("HAIR_CROWD_SIMULATE", 1) != 0;
        ctx.Require(n >= 1 && n <= 24, $"HAIR_CROWD_N is between 1 and 24 ({n})");

        // Hair is a frame graph extension, and the path-traced pipeline (the default where the device supports it) has none.
        ctx.Settings["enablePathTracing"] = false;
        var scene = ctx.Scene;
        scene.DirectionalLight(direction: (-0.4, -1.0, -0.3), intensity: 6f);
        scene.Instantiate("../assets/plane.glb", position: (0, 0, -0.5 * (n - 1) * spacing), name: "hair-crowd-floor").Scaled((float)(n * spacing + 80));

        // Row 0 is nearest the first camera; rows go away from it along -z. With an odd side the grid is shifted half a
        // spacing, so that x = 0 is always an aisle.
        double firstX = -0.5 * (n - 1) * spacing + (n % 2 == 1 ? 0.5 * spacing : 0.0);
        for (int row = 0; row < n; ++row) {
            for (int column = 0; column < n; ++column) {
                Vec3 position = (firstX + column * spacing, HeadHeight, -row * spacing);
                _headPositions.Add(position);
                _heads.Add(scene.Instantiate("../assets/sphere.glb", position: position, name: $"hair-crowd-head-{row}-{column}").Scaled(HeadScale));
            }
        }
        await Task.WhenAll(_heads.Select(head => head.WhenReady()));
        double depth = (n - 1) * spacing;
        Vec3 nearEye = (0, HeadHeight + 0.15, 2.0);
        Vec3 nearTarget = (0, HeadHeight - 0.1, -0.5 * depth);
        ctx.Camera.Fov(50).LookAt(nearEye, nearTarget);

        // Four grooms, shared by every head. A groom's rest space is its head's mesh space, so its size and its strands'
        // width are scaled with the head (0.22): 13 to 20 cm of hair on an 11 cm head. 1,024 guides each: an instance
        // keeps state for every guide point, and the builder's default of 4,096 guides would need 1.1 GB for a hundred.
        var commands = ctx.Commands;
        commands.Invoke("hair.clear");
        commands.Invoke("hair.lod", new { error_pixels = 1.0, max_width_scale = 8.0, min_detail = 0.02, force_detail = 0.0, segment_pixels = 4.0, freeze = false });
        if (Environment.GetEnvironmentVariable("HAIR_COVERAGE") is string coverage) commands.Invoke("hair.lod", new { coverage }); // off: the opaque form
        commands.Invoke("hair.debug", new { view = "off" });
        var grooms = new object[] {
            new { style = "straight", strands = 12000, seed = 21, radius = 0.5, length = 0.9, droop = 6.0, root_width = 0.0004, tip_width = 0.0002, points = 17 },
            new { style = "wavy", strands = 10000, seed = 22, radius = 0.5, length = 0.9, droop = 6.0, wave_amplitude = 0.012, wave_count = 5.0, root_width = 0.0004, tip_width = 0.0002, points = 33 },
            new { style = "curly", strands = 8000, seed = 23, radius = 0.5, length = 0.6, droop = 4.0, root_width = 0.0004, tip_width = 0.0002, points = 33 },
            new { style = "wavy", strands = 10000, seed = 24, radius = 0.5, length = 0.4, droop = 3.0, wave_amplitude = 0.02, wave_count = 3.0, root_width = 0.0005, tip_width = 0.0002, points = 33 },
        };
        var assets = grooms.Select(procedural => commands.Invoke("hair.load", new { procedural, max_guides = 1024 }).GetProperty("asset").GetUInt32()).ToArray();
        var colors = new[] { new[] { 0.09, 0.05, 0.03 }, new[] { 0.30, 0.20, 0.09 }, new[] { 0.02, 0.02, 0.02 }, new[] { 0.35, 0.10, 0.04 }, new[] { 0.5, 0.42, 0.3 } };
        for (int head = 0; head < _heads.Count; ++head) {
            int row = head / n, column = head % n;
            uint asset = assets[(row * 3 + column * 5) % assets.Length];
            bool fits = commands.TryInvoke("hair.attach", new { asset, entity = $"hair-crowd-head-{row}-{column}", color = colors[(row + column * 2) % colors.Length], roughness = 0.6 }, out var attached);
            ctx.Require(fits, $"head {head + 1} of {_heads.Count} takes a groom ({(fits ? "" : attached.ToString())})");
            commands.Invoke("hair.colliders", new { instance = attached.GetProperty("instance").GetUInt32(), colliders = new[] { new { center = new[] { 0.0, 0.0, 0.0 }, radius = 0.5 } } });
        }
        commands.Invoke("hair.sim", new { enabled = _simulate, reset = true, lod_force_level = -1.0, lod_point_budget = 0 });
        commands.Invoke("hair.stats", new { continuous = true });
        await ctx.WaitUntilStable();
        await Step(ctx, 30);

        var loaded = commands.Invoke("hair.stats");
        var rows = loaded.GetProperty("rows");
        uint totalStrands = loaded.GetProperty("instance_strands").GetUInt32();
        ctx.Log($"crowd: {n} x {n} heads {spacing} m apart, simulation {(_simulate ? "on" : "off")}; {loaded.GetProperty("assets")} assets, " +
            $"{loaded.GetProperty("instances")} instances, {totalStrands} strands over the instances; rows {rows}; " +
            $"buffers {loaded.GetProperty("buffer_bytes").GetUInt64() / (1024.0 * 1024.0):F1} MB; visibility target {loaded.GetProperty("visibility_target")}");
        ctx.Require(loaded.GetProperty("active_instances").GetUInt32() == (uint)(n * n), $"all {n * n} grooms are attached and dispatched ({loaded.GetProperty("active_instances")})");

        // From the front: the grid runs from 2 m away to its far row.
        var near = await Station(ctx, "near", nearEye, nearTarget);
        await Capture(ctx, "hair_crowd_near");
        ctx.Check(near.Instances > 0 && near.PixelsResolved > 0, $"grooms are drawn from the front ({near.Instances} in view, {near.PixelsResolved} pixels)");
        ctx.Check(near.Strands < totalStrands, $"fewer strands are drawn than the scene has ({near.Strands} of {totalStrands})");

        // The fly-through: down the aisle at x = 0, from in front of the grid to 3 m behind its far row.
        ulong firstFlyFrame = NewestFrame + 1;
        for (int step = 0; step <= FlyFrames; ++step) {
            double z = 2.0 - (depth + 5.0) * step / FlyFrames;
            ctx.Camera.LookAt((0, HeadHeight + 0.1, z), (0, HeadHeight, z - 6.0));
            await Step(ctx);
            if (step == FlyFrames / 2) await Capture(ctx, "hair_crowd_middle");
        }
        await Step(ctx, 12);
        ulong lastFlyFrame = NewestFrame;

        // Behind the last row, facing away: nothing is in view.
        var away = await Station(ctx, "facing away", (0, HeadHeight, -depth - 3.0), (0, HeadHeight, -depth - 20.0));
        ctx.Check(away.Instances == 0 && away.Strands == 0 && away.PixelsWritten == 0, $"nothing is selected or drawn facing away from the grid ({away.Instances} instances, {away.Strands} strands)");
        // The simulation follows the view: with no groom in view none is simulated. The guide rows still lent out are
        // kept shapes: none belongs to a groom that is being simulated.
        if (_simulate && _simSamples.Count > 0) {
            var awaySim = _simSamples.Values.Last();
            ctx.Check(awaySim.Simulated == 0 && awaySim.Evaluated == 0, $"no groom is simulated facing away from the grid ({awaySim})");
        }
        var awayRows = commands.Invoke("hair.stats").GetProperty("memory").GetProperty("guide_state");
        ctx.Log($"guide rows facing away: {awayRows}");
        ctx.Check(awayRows.GetProperty("rows_active").GetUInt32() == 0, $"no guide rows are in active use facing away from the grid ({awayRows.GetProperty("rows_active")})");

        // From far in front, the whole grid small on screen.
        double farDistance = 30.0 + depth;
        var far = await Station(ctx, "far", (0, HeadHeight + 0.2 * farDistance, farDistance), (0, HeadHeight, -0.5 * depth), frames: 240);
        await Capture(ctx, "hair_crowd_far");
        ctx.Check(far.Instances == (uint)(n * n), $"every groom is in view from {farDistance:F0} m ({far.Instances} of {n * n})");
        ctx.Check(far.Strands * 2 < totalStrands, $"under half the strands are drawn from {farDistance:F0} m ({far.Strands} of {totalStrands})");

        foreach (var sample in _samples.Values.Where(sample => sample.Frame >= firstFlyFrame && sample.Frame <= lastFlyFrame)) {
            string sim = _simSamples.TryGetValue(sample.Frame, out var simSample) ? " " + simSample : "";
            ctx.Check(true, $"sample {sample}{sim}");
        }
        var all = _samples.Values.ToList();
        uint poolCapacity = loaded.GetProperty("gpu_frame").GetProperty("pool_vertices").GetProperty("capacity").GetUInt32();
        ctx.Log($"all {all.Count} frames: instances in view {all.Min(s => s.Instances)} to {all.Max(s => s.Instances)}, strands {all.Min(s => s.Strands)} to {all.Max(s => s.Strands)}, " +
            $"segments up to {all.Max(s => s.Segments)}, pool up to {all.Max(s => s.PoolVertices)} of {poolCapacity}, pixels written up to {all.Max(s => s.PixelsWritten)}, " +
            $"overflow frames {all.Count(s => s.Overflow != 0)}, capped segments in a frame up to {all.Max(s => s.Capped)}");
        ctx.Check(all.All(sample => sample.Overflow == 0), $"the strand pool and the visible-cluster list never overflow (pool use up to {all.Max(s => s.PoolVertices)} of {poolCapacity})");
        ctx.Check(all.All(sample => sample.ClustersTested > 0), "every frame ran the select pass over the crowd");
        ctx.Check(all.All(sample => sample.StaleGuides == 0),
            $"no drawn strand read guide rows that were not written in its frame for its groom (up to {all.Max(s => s.StaleGuides)} did)");
        ctx.Check(all.All(sample => sample.UnseenStrands == 0),
            $"no strand was drawn of a groom left unsimulated for being out of view (up to {all.Max(s => s.UnseenStrands)} in a frame)");
        // Guide state is lent on demand. From the far camera every groom is too small to simulate and none holds rows;
        // over the run every groom a view showed got the rows it asked for.
        var guideRows = commands.Invoke("hair.stats").GetProperty("memory").GetProperty("guide_state");
        ctx.Log($"guide rows at the end: {guideRows}");
        ctx.Check(guideRows.GetProperty("rows_in_use").GetUInt32() == 0, $"no groom holds guide rows from {farDistance:F0} m ({guideRows.GetProperty("rows_in_use")} rows)");
        ctx.Check(guideRows.GetProperty("allocation_failures").GetUInt64() == 0 && guideRows.GetProperty("starved_instances").GetUInt32() == 0,
            $"every groom in view got the guide rows it asked for ({guideRows.GetProperty("allocation_failures")} failures)");
        ctx.Check(guideRows.GetProperty("high_water_rows").GetUInt32() <= guideRows.GetProperty("capacity_rows").GetUInt32() &&
            (guideRows.GetProperty("allocations").GetUInt64() > 0) == _simulate,
            $"guide rows were lent only while simulating, within the buffer ({guideRows.GetProperty("allocations")} ranges, at most {guideRows.GetProperty("high_water_rows")} rows of {guideRows.GetProperty("capacity_rows")})");
        if (_simulate) {
            var sims = _simSamples.Values.ToList();
            ctx.Check(sims.Count > FlyFrames / 2, $"the simulation's metrics were read back for every instance in {sims.Count} frames");
            if (sims.Count > 0) {
                ctx.Log($"simulation over {sims.Count} frames: instances simulated {sims.Min(s => s.Simulated)} to {sims.Max(s => s.Simulated)}, " +
                    $"guides {sims.Min(s => s.Guides)} to {sims.Max(s => s.Guides)}, guide points {sims.Min(s => s.Points)} to {sims.Max(s => s.Points)}");
                ctx.Check(sims.All(s => s.StaleGuides == 0), $"every guide went on from its own rows of the frame before (up to {sims.Max(s => s.StaleGuides)} in a frame did not)");
                ctx.Check(sims.All(s => s.Starved == 0), $"no groom in view went without guide rows (up to {sims.Max(s => s.Starved)} in a frame)");
                var farSim = sims[^1];
                ctx.Check(farSim.Simulated == 0, $"no groom is simulated from {farDistance:F0} m ({farSim})");
            }
        }
        commands.Invoke("hair.sim", new { enabled = false });
        commands.Invoke("hair.stats", new { continuous = false });
    }
}
