// Strand hair (docs/hair.md): two procedural grooms, one on a moving sphere (it follows the entity's transform) and one
// on the swinging tip joint of a skinned bar (it follows the skeleton), drawn lit. The scenario checks that the grooms
// reach the GPU, that the root-binding pass is dispatched over the drawn strands in every frame, that the root frames it
// writes match the same arithmetic on the CPU, and that the resolve writes hair pixels; it captures the frame
// (hair_single.bmp in HAIR_CAPTURE_DIR, or in hair_captures in the temporary directory).
//   BasicRenderer.exe --script scenarios/hair/hair_single.cs --script-exit --script-fixed-dt 0.0166667
// Needs a host with the hair plugin (the demo). It is in a folder of its own because the scripting tests run every
// scenario beside the examples on a fake host, which has no hair commands.

[Scenario("hair-single", TimeoutSeconds = 120, Description = "Procedural grooms on a rigid and a skinned entity; checks upload, dispatch and root frames.")]
public sealed class HairSingle : Scenario
{
    // Root frames are compared in world space, a few metres from the origin, in single precision.
    const float Tolerance = 1e-4f;

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

    public override async Task Run(ScenarioContext ctx)
    {
        // Hair is a frame graph extension, and the path-traced pipeline (the default where the device supports it)
        // has none. The setting is restored when the scenario ends.
        ctx.Settings["enablePathTracing"] = false;

        var scene = ctx.Scene;
        scene.DirectionalLight(direction: (-0.4, -1.0, -0.3), intensity: 6f);
        var head = scene.Instantiate("../assets/sphere.glb", position: (-1.0, 1.0, 0.0), name: "hair-head");
        var bar = scene.Instantiate("../assets/skinned_bar.glb", position: (1.0, 0.0, 0.0), name: "hair-bar");
        await Task.WhenAll(head.WhenReady(), bar.WhenReady());
        ctx.Camera.Fov(60).LookAt(eye: (0, 1.2, 4), target: (0, 0.8, 0));

        var commands = ctx.Commands;
        commands.Invoke("hair.clear");
        if (Environment.GetEnvironmentVariable("HAIR_COVERAGE") is string coverage) commands.Invoke("hair.lod", new { coverage }); // off: the opaque form
        uint scalp = commands.Invoke("hair.load", new {
            procedural = new { style = "wavy", strands = 4000, seed = 7, radius = 0.5, length = 0.7, droop = 5.0, root_width = 0.0005, tip_width = 0.0003 },
        }).GetProperty("asset").GetUInt32();
        uint tuft = commands.Invoke("hair.load", new {
            procedural = new { style = "straight", strands = 600, seed = 3, radius = 0.08, center = new[] { 0.0, 1.0, 0.0 }, cap_angle_degrees = 60.0, length = 0.2 },
        }).GetProperty("asset").GetUInt32();
        commands.Invoke("hair.attach", new { asset = scalp, entity = "hair-head" });
        commands.Invoke("hair.attach", new { asset = tuft, entity = "hair-bar", bone = "tip", color = new[] { 0.45, 0.12, 0.05 }, roughness = 0.5 });

        await ctx.WaitUntilStable();
        head.Animate().Spin(Vec3.UnitY, 90);
        head.Animate().MoveBy((0, 0.3, 0), 1.0, Ease.InOutSine).PingPong();
        await ctx.Frames(30);

        var loaded = commands.Invoke("hair.stats");
        foreach (var instance in loaded.GetProperty("instance_list").EnumerateArray()) {
            ctx.Log($"instance {instance.GetProperty("instance")}: {instance.GetProperty("state")} to {instance.GetProperty("entity")} ({instance.GetProperty("mode")}) {instance.GetProperty("error")}");
        }
        ctx.Check(loaded.GetProperty("assets").GetUInt32() == 2, "two grooms are loaded");
        ctx.Check(loaded.GetProperty("strands").GetUInt32() == 4600, "the grooms have 4000 and 600 strands");
        ctx.Check(loaded.GetProperty("attached_instances").GetUInt32() == 2, "both instances found their entities");
        ctx.Check(loaded.GetProperty("active_instances").GetUInt32() == 2, "both instances are dispatched");
        ctx.Check(loaded.GetProperty("pending_upload_assets").GetUInt64() == 0, "every groom is uploaded");
        ctx.Check(loaded.GetProperty("asset_bytes_uploaded").GetUInt64() > 0, "groom bytes were uploaded");

        // The GPU counts its own evaluate dispatches. A stats call returns the last readback of that counter and asks
        // for the next one. Two readbacks some frames apart must differ by what the CPU asked for in between: one
        // dispatch a frame, over the drawn strands of every instance.
        await ctx.Frames(5);
        var first = commands.Invoke("hair.stats").GetProperty("gpu_counter");
        await ctx.Frames(60);
        commands.Invoke("hair.stats");
        await ctx.Frames(5);
        var second = commands.Invoke("hair.stats").GetProperty("gpu_counter");
        ctx.Require(first.GetProperty("valid").GetBoolean() && second.GetProperty("valid").GetBoolean(), "the dispatch counter was read back");
        ulong frames = second.GetProperty("frame").GetUInt64() - first.GetProperty("frame").GetUInt64();
        ulong asked = second.GetProperty("cpu_evaluate_dispatches").GetUInt64() - first.GetProperty("cpu_evaluate_dispatches").GetUInt64();
        uint ran = unchecked(second.GetProperty("evaluate_dispatches").GetUInt32() - first.GetProperty("evaluate_dispatches").GetUInt32());
        ctx.Log($"evaluation over {frames} frames: {asked} dispatches asked for, {ran} ran on the GPU");
        ctx.Check(frames >= 30, "the two readbacks are at least 30 frames apart");
        ctx.Check(asked == frames, "one dispatch was asked for in every frame");
        ctx.Check(ran == asked, "every dispatch asked for ran on the GPU");
        var drawnFrame = commands.Invoke("hair.stats").GetProperty("gpu_frame");
        ctx.Check(drawnFrame.GetProperty("strands_selected").GetUInt32() > 0, "strands are drawn");
        ctx.Check(drawnFrame.GetProperty("stale_guides_drawn").GetUInt32() == 0 && drawnFrame.GetProperty("unseen_strands_drawn").GetUInt32() == 0,
            "no drawn strand read guide rows it was not lent, and none is of an instance left out for being out of view");
        // No root frame is stored: every pass works out the ones it needs, and an instance holds no guide rows unless
        // it is simulated.
        var guideState = commands.Invoke("hair.stats").GetProperty("memory").GetProperty("guide_state");
        ctx.Check(guideState.GetProperty("rows_in_use").GetUInt32() == 0 && guideState.GetProperty("holders").GetUInt32() == 0,
            $"with the simulation off no instance holds guide rows ({guideState.GetProperty("rows_in_use")} rows, {guideState.GetProperty("holders")} holders)");

        // The self-check asks the GPU for the root frames of roots sampled across each groom, drawn or not, as every
        // pass works them out, and compares them with the CPU's.
        commands.Invoke("hair.selfcheck", new { enabled = true, roots = 16 });
        await ctx.Frames(150);
        var stats = commands.Invoke("hair.stats");
        var check = stats.GetProperty("self_check");
        commands.Invoke("hair.selfcheck", new { enabled = false });
        float position = check.GetProperty("worst_position_error").GetSingle();
        float direction = check.GetProperty("worst_direction_error").GetSingle();
        ctx.Log($"self-check: {check.GetProperty("completed_checks")} checks, worst position difference {position:e3} m, worst direction difference {direction:e3}, " +
            $"moved up to {check.GetProperty("largest_motion").GetSingle():e3} m (rigid) and {check.GetProperty("largest_skinned_motion").GetSingle():e3} m (skinned) between frames");
        ctx.Check(check.GetProperty("completed_checks").GetUInt32() >= 10, "at least ten self-checks completed");
        ctx.Check(check.GetProperty("total_stale_instances").GetUInt32() == 0, "every checked instance was written in the checked frame");
        ctx.Check(check.GetProperty("instances").GetUInt32() == 2 && check.GetProperty("skinned_instances").GetUInt32() == 1, "the last check compared the rigid and the skinned instance");
        ctx.Check(check.GetProperty("roots").GetUInt32() == 32 && check.GetProperty("previous_roots").GetUInt32() == 32, "the last check compared 16 roots of each, current and previous");
        ctx.Check(check.GetProperty("largest_motion").GetSingle() > 1e-3f, "the rigid instance moved between frames");
        ctx.Check(check.GetProperty("largest_skinned_motion").GetSingle() > 1e-3f, "the skinned instance moved between frames");
        ctx.Check(position < Tolerance, $"GPU root positions match the CPU within {Tolerance} m");
        ctx.Check(direction < Tolerance, $"GPU root normals and tangents match the CPU within {Tolerance}");

        // The resolve: hair pixels reach the surface targets, no more of them than the rasterizer wrote.
        await ctx.Frames(5);
        var drawn = commands.Invoke("hair.stats").GetProperty("gpu_frame");
        uint resolved = drawn.GetProperty("pixels_resolved").GetUInt32();
        ctx.Log($"resolve: {resolved} hair pixels resolved, {drawn.GetProperty("pixels_written")} visibility writes, {drawn.GetProperty("strands_selected")} strands drawn");
        ctx.Check(resolved > 0 && resolved <= drawn.GetProperty("pixels_written").GetUInt32(), "the resolve wrote hair pixels, at most one for each visibility write");
        await Capture(ctx, "hair_single");

        ctx.Log($"hair: {stats.GetProperty("assets")} assets, {stats.GetProperty("instances")} instances, {stats.GetProperty("strands")} strands, " +
            $"{stats.GetProperty("bytes_uploaded")} bytes uploaded, {stats.GetProperty("evaluate_dispatches")} evaluate dispatches in {stats.GetProperty("frames")} frames");
    }
}
