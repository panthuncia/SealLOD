// Strand hair (docs/hair.md): LOD selection, strand evaluation, the compute rasterizer and the resolve. A procedural
// groom of 24,000 strands on a sphere, drawn lit; the camera dollies straight back from it. The scenario reads the GPU
// counters of every frame (hair.stats) and checks that the drawn strand count falls smoothly with distance, that hair
// reaches the hair visibility target and the surface targets while in view and not when outside the frustum or behind
// an occluder, that a frozen LOD keeps its selection, and that the strand pool does not overflow. It captures the lit
// groom at three distances and one debug view (hair_lod_sweep_*.bmp in HAIR_CAPTURE_DIR, or in hair_captures in the
// temporary directory).
//   BasicRenderer.exe --script scenarios/hair/hair_lod_sweep.cs --script-exit --script-fixed-dt 0.0166667
// With HAIR_SWEEP_SIMULATE=1 in the environment the groom is simulated (hair.sim) while the same checks run.
// Needs a host with the hair plugin (the demo), in raster mode. Every sampled frame is recorded in the result document
// as a check named "sample ...", since the document has no other place for data.

[Scenario("hair-lod-sweep", TimeoutSeconds = 240, Description = "Camera dolly over a 24k-strand groom; checks LOD selection, raster counters, culling and occlusion.")]
public sealed class HairLodSweep : Scenario
{
    const int SweepFrames = 420;
    const double NearDistance = 2.0;
    const double FarDistance = 80.0;
    // The most strands one cluster may gain or lose between two adjacent frames.
    const double StrandsPerClusterPerFrame = 3.0;

    // The groom is the upper hemisphere of a sphere of radius 0.5 at Center, with 0.3 m strands.
    static readonly Vec3 Center = (0, 1.0, 0);
    static readonly Vec3 Target = (0, 1.25, 0);

    sealed record Sample(ulong Frame, double Distance, bool Selected, uint ClustersTested, uint ClustersVisible, uint Strands,
        uint Segments, uint PoolVertices, uint Overflow, uint SegmentsDrawn, uint PixelsWritten, uint PixelsOccluded, uint Capped,
        uint PixelsResolved)
    {
        public override string ToString() =>
            $"frame={Frame} distance={Distance:F3} clusters_visible={ClustersVisible}/{ClustersTested} strands={Strands} segments={Segments} " +
            $"pool_vertices={PoolVertices} overflow={Overflow} segments_drawn={SegmentsDrawn} pixels_written={PixelsWritten} " +
            $"pixels_occluded={PixelsOccluded} capped={Capped} pixels_resolved={PixelsResolved}";
    }

    // Where hair.capture writes: HAIR_CAPTURE_DIR, or a folder in the system's temporary directory.
    static string CaptureFolder()
    {
        string folder = Environment.GetEnvironmentVariable("HAIR_CAPTURE_DIR") ?? System.IO.Path.Combine(System.IO.Path.GetTempPath(), "hair_captures");
        System.IO.Directory.CreateDirectory(folder);
        return folder.Replace('\\', '/');
    }

    // Writes the presented frame to <CaptureFolder>/<name>.bmp and checks that the file is there.
    static async Task Capture(ScenarioContext ctx, string name)
    {
        string path = CaptureFolder() + "/" + name + ".bmp";
        ctx.Commands.Invoke("hair.capture", new { path });
        string state = "pending";
        for (int frame = 0; frame < 600 && state == "pending"; ++frame) {
            await ctx.NextFrame();
            state = ctx.Commands.Invoke("hair.stats").GetProperty("capture").GetProperty("state").GetString() ?? "";
        }
        ctx.Check(state == "written" && System.IO.File.Exists(path), $"captured {path} ({state})");
    }

    static Vec3 Eye(double distance) => (0, 1.25 + 0.15 * distance, distance);

    static Sample? Parse(System.Text.Json.JsonElement frame, out Vec3 camera)
    {
        var position = frame.GetProperty("camera_position");
        camera = (position[0].GetSingle(), position[1].GetSingle(), position[2].GetSingle());
        if (!frame.GetProperty("valid").GetBoolean()) return null;
        double dx = camera.X - Center.X, dy = camera.Y - Center.Y, dz = camera.Z - Center.Z;
        return new Sample(frame.GetProperty("frame").GetUInt64(), Math.Sqrt(dx * dx + dy * dy + dz * dz),
            frame.GetProperty("selected").GetBoolean(), frame.GetProperty("clusters_tested").GetUInt32(),
            frame.GetProperty("clusters_visible").GetUInt32(), frame.GetProperty("strands_selected").GetUInt32(),
            frame.GetProperty("segments_selected").GetUInt32(), frame.GetProperty("pool_vertices").GetProperty("used").GetUInt32(),
            frame.GetProperty("overflow_clusters").GetUInt32(), frame.GetProperty("segments_drawn").GetUInt32(),
            frame.GetProperty("pixels_written").GetUInt32(), frame.GetProperty("pixels_occluded").GetUInt32(),
            frame.GetProperty("segments_capped").GetUInt32(), frame.GetProperty("pixels_resolved").GetUInt32());
    }

    // The newest frame whose counters were read back.
    static Sample? Read(ScenarioContext ctx, out Vec3 camera) =>
        Parse(ctx.Commands.Invoke("hair.stats").GetProperty("gpu_frame"), out camera);

    // Adds every frame of the plugin's counter history to `samples`. A frame's counters arrive a few frames after it,
    // one frame's worth per frame, so polling once a frame collects every frame.
    static void Collect(ScenarioContext ctx, SortedDictionary<ulong, Sample> samples)
    {
        foreach (var frame in ctx.Commands.Invoke("hair.stats").GetProperty("gpu_history").EnumerateArray()) {
            if (Parse(frame, out _) is { } sample) samples[sample.Frame] = sample;
        }
    }

    // Puts the camera at `eye` and returns the counters of a frame rendered from there, a few frames after the first
    // such frame so that whatever else changed with the move has landed too.
    static async Task<Sample> SampleAt(ScenarioContext ctx, Vec3 eye, Vec3 target, int settleFrames = 6)
    {
        ctx.Camera.LookAt(eye, target);
        int settled = 0;
        for (int frame = 0; frame < 240; ++frame) {
            await ctx.NextFrame();
            var sample = Read(ctx, out var camera);
            double dx = camera.X - eye.X, dy = camera.Y - eye.Y, dz = camera.Z - eye.Z;
            if (sample is null || dx * dx + dy * dy + dz * dz > 1e-6) continue;
            if (++settled >= settleFrames) return sample;
        }
        ctx.Fail($"no hair counters were read back for a frame with the camera at {eye}");
        throw new InvalidOperationException();
    }

    public override async Task Run(ScenarioContext ctx)
    {
        // Hair is a frame graph extension, and the path-traced pipeline (the default where the device supports it)
        // has none. The setting is restored when the scenario ends.
        ctx.Settings["enablePathTracing"] = false;

        var scene = ctx.Scene;
        scene.DirectionalLight(direction: (-0.4, -1.0, -0.3), intensity: 6f);
        var head = scene.Instantiate("../assets/sphere.glb", position: Center, name: "hair-sweep-head");
        await head.WhenReady();
        ctx.Camera.Fov(60).LookAt(Eye(NearDistance), Target);

        var commands = ctx.Commands;
        commands.Invoke("hair.clear");
        commands.Invoke("hair.lod", new { error_pixels = 1.0, max_width_scale = 8.0, min_detail = 0.02, force_detail = 0.0, segment_pixels = 4.0, freeze = false });
        if (Environment.GetEnvironmentVariable("HAIR_COVERAGE") is string coverage) commands.Invoke("hair.lod", new { coverage }); // off: the opaque form
        uint asset = commands.Invoke("hair.load", new {
            procedural = new { style = "wavy", strands = 24000, seed = 11, radius = 0.5, length = 0.3 },
        }).GetProperty("asset").GetUInt32();
        commands.Invoke("hair.attach", new { asset, entity = "hair-sweep-head" });
        commands.Invoke("hair.debug", new { view = "off" });
        // HAIR_SWEEP_SIMULATE=1 runs the same sweep with the guides simulated and the strands interpolated from them.
        bool simulate = Environment.GetEnvironmentVariable("HAIR_SWEEP_SIMULATE") == "1";
        commands.Invoke("hair.sim", new { enabled = simulate });
        ctx.Log($"simulation: {(simulate ? "on" : "off")}");
        commands.Invoke("hair.stats", new { continuous = true });
        await ctx.WaitUntilStable();
        await ctx.Frames(20);

        var loaded = commands.Invoke("hair.stats");
        uint totalStrands = loaded.GetProperty("strands").GetUInt32();
        uint totalClusters = loaded.GetProperty("rows").GetProperty("clusters").GetProperty("used").GetUInt32();
        uint poolCapacity = loaded.GetProperty("gpu_frame").GetProperty("pool_vertices").GetProperty("capacity").GetUInt32();
        ctx.Log($"groom: {totalStrands} strands in {totalClusters} clusters, {loaded.GetProperty("points")} points; " +
            $"pool capacity {poolCapacity} vertices; hair visibility target {loaded.GetProperty("visibility_target")}");
        ctx.Require(totalStrands >= 20000, "the groom has at least 20,000 strands");
        ctx.Require(loaded.GetProperty("active_instances").GetUInt32() == 1, "the groom is attached and dispatched");

        // The dolly: the distance grows by the same ratio every frame, so the detail level changes at an even rate.
        var collected = new SortedDictionary<ulong, Sample>();
        Collect(ctx, collected);
        ulong firstDollyFrame = collected.Count == 0 ? 0 : collected.Keys.Max() + 1;
        for (int step = 0; step <= SweepFrames; ++step) {
            double distance = NearDistance * Math.Pow(FarDistance / NearDistance, (double)step / SweepFrames);
            ctx.Camera.LookAt(Eye(distance), Target);
            await ctx.NextFrame();
            Collect(ctx, collected);
        }
        // The last frames' counters are still on their way.
        for (int frame = 0; frame < 12; ++frame) {
            await ctx.NextFrame();
            Collect(ctx, collected);
        }
        var samples = collected.Values.Where(sample => sample.Frame >= firstDollyFrame).ToList();
        foreach (var sample in samples) {
            ctx.Check(sample.Overflow == 0 && sample.PoolVertices < poolCapacity, $"sample {sample}");
        }
        ctx.Require(samples.Count >= SweepFrames, $"the counters of the dolly's frames were read back ({samples.Count} of {SweepFrames + 1})");
        int gaps = 0;
        for (int i = 1; i < samples.Count; ++i) gaps += samples[i].Frame == samples[i - 1].Frame + 1 ? 0 : 1;
        ctx.Check(gaps == 0, $"the sampled frames are consecutive ({gaps} gaps)");

        var first = samples[0];
        var last = samples[^1];
        ctx.Log($"dolly: {samples.Count} frames sampled; near {first}; far {last}");
        ctx.Check(samples.All(sample => sample.Selected), "every dolly frame ran the select pass");
        ctx.Check(samples.All(sample => sample.ClustersTested == totalClusters), "every cluster was tested in every frame");
        ctx.Check(first.Strands == totalStrands, $"every strand is drawn at {first.Distance:F1} m ({first.Strands} of {totalStrands})");
        ctx.Check(last.Strands * 4 < first.Strands, $"under a quarter of the strands are drawn at {last.Distance:F1} m ({last.Strands} of {first.Strands})");
        ctx.Check(last.Segments * 8 < first.Segments, $"under an eighth of the segments are drawn at {last.Distance:F1} m ({last.Segments} of {first.Segments})");
        ctx.Check(samples.All(sample => sample.PixelsWritten > 0 && sample.SegmentsDrawn > 0), "hair is rasterized in every dolly frame");
        ctx.Check(samples.All(sample => sample.PixelsResolved > 0 && sample.PixelsResolved <= sample.PixelsWritten),
            $"hair is resolved into the surface targets in every dolly frame ({samples.Min(sample => sample.PixelsResolved)} to {samples.Max(sample => sample.PixelsResolved)} pixels)");
        ctx.Check(samples.All(sample => sample.Capped == 0), $"no segment hit the per-segment pixel limit (most in a frame: {samples.Max(sample => sample.Capped)})");

        // Monotonic in distance. The frustum can change the count the other way (a cluster entering the view adds its
        // strands), so the strand count is compared only between frames that saw the same number of clusters.
        int compared = 0, rises = 0, frustumChanges = 0;
        double largestStep = 0;
        for (int i = 1; i < samples.Count; ++i) {
            var a = samples[i - 1];
            var b = samples[i];
            if (a.ClustersVisible != b.ClustersVisible) {
                ++frustumChanges;
                continue;
            }
            ++compared;
            if (b.Strands > a.Strands) ++rises;
            double perClusterPerFrame = Math.Abs((double)b.Strands - a.Strands) / Math.Max(1u, b.ClustersVisible) / (b.Frame - a.Frame);
            largestStep = Math.Max(largestStep, perClusterPerFrame);
        }
        ctx.Log($"dolly: {compared} adjacent pairs compared, {frustumChanges} skipped for a change in visible clusters; " +
            $"clusters visible {samples.Min(sample => sample.ClustersVisible)} to {samples.Max(sample => sample.ClustersVisible)} of {totalClusters}; " +
            $"largest change {largestStep:F3} strands per cluster per frame");
        ctx.Check(compared >= samples.Count / 2, "most adjacent frames saw the same clusters");
        ctx.Check(rises == 0, $"the drawn strand count never rises as the camera moves away ({rises} rises in {compared} pairs)");
        ctx.Check(largestStep <= StrandsPerClusterPerFrame, $"adjacent frames differ by at most {StrandsPerClusterPerFrame} strands per cluster ({largestStep:F3})");

        // The lit groom at three distances, and a debug view over it.
        foreach (double distance in new[] { NearDistance, 12.0, FarDistance }) {
            await SampleAt(ctx, Eye(distance), Target, settleFrames: 30);
            await Capture(ctx, $"hair_lod_sweep_{distance:F0}m");
        }
        commands.Invoke("hair.debug", new { view = "detail" });
        await SampleAt(ctx, Eye(12.0), Target, settleFrames: 10);
        await Capture(ctx, "hair_lod_sweep_12m_detail");
        commands.Invoke("hair.debug", new { view = "off" });

        // Frozen LOD: the selection made far away is kept when the camera comes close.
        var far = await SampleAt(ctx, Eye(40), Target);
        commands.Invoke("hair.lod", new { freeze = true });
        await ctx.Frames(4);
        var frozen = await SampleAt(ctx, Eye(3), Target);
        commands.Invoke("hair.lod", new { freeze = false });
        var thawed = await SampleAt(ctx, Eye(3), Target);
        ctx.Log($"freeze: far {far}; frozen near {frozen}; thawed near {thawed}");
        ctx.Check(!frozen.Selected && frozen.Strands == far.Strands && frozen.ClustersVisible == far.ClustersVisible, "a frozen LOD keeps the far selection at the near camera");
        ctx.Check(frozen.PixelsWritten > far.PixelsWritten, "the frozen selection is still evaluated and rasterized from the near camera");
        ctx.Check(thawed.Selected && thawed.Strands > frozen.Strands, "selection resumes when the LOD is thawed");

        // Forced detail: every cluster draws the forced share, whatever the distance.
        commands.Invoke("hair.lod", new { force_detail = 0.25 });
        var forced = await SampleAt(ctx, Eye(3), Target);
        commands.Invoke("hair.lod", new { force_detail = 0.0 });
        ctx.Log($"forced detail 0.25: {forced}");
        ctx.Check(forced.Strands > totalStrands / 4 && forced.Strands < totalStrands / 4 + 2 * totalClusters, $"forcing detail 0.25 draws a quarter of each cluster ({forced.Strands} of {totalStrands})");

        // Outside the frustum: the camera turns its back on the groom.
        var away = await SampleAt(ctx, Eye(5), (0, 1.25 + 0.15 * 10, 10));
        ctx.Log($"outside the frustum: {away}");
        ctx.Check(away.ClustersVisible == 0 && away.Strands == 0 && away.PoolVertices == 0, "no cluster is selected with the groom behind the camera");
        ctx.Check(away.PixelsWritten == 0 && away.SegmentsDrawn == 0 && away.PixelsResolved == 0, "nothing is rasterized or resolved with the groom behind the camera");

        // Behind an occluder: a 4 m cube between the camera and the groom. The groom is still in the frustum, so it is
        // selected and evaluated, and every covered pixel fails the opaque depth test.
        var open = await SampleAt(ctx, Eye(6), Target);
        var wall = scene.Instantiate("../assets/cube.glb", position: (0, 1.7, 3), name: "hair-sweep-wall").Scaled(4f);
        await wall.WhenReady();
        await ctx.WaitUntilStable();
        await ctx.Frames(30);
        var occluded = await SampleAt(ctx, Eye(6), Target, settleFrames: 10);
        ctx.Log($"open: {open}; behind the wall: {occluded}");
        ctx.Check(open.PixelsWritten > 0, "hair is rasterized before the wall is there");
        ctx.Check(occluded.ClustersVisible == open.ClustersVisible && occluded.Strands == open.Strands, "the occluded groom is still selected");
        ctx.Check(occluded.PixelsWritten == 0 && occluded.SegmentsDrawn == 0 && occluded.PixelsResolved == 0, $"no hair pixel is written or resolved behind the wall ({occluded.PixelsWritten})");
        ctx.Check(occluded.PixelsOccluded > 0, "the covered pixels were rejected by the opaque depth");

        commands.Invoke("hair.stats", new { continuous = false });
        commands.Invoke("hair.debug", new { view = "off" });
    }
}
