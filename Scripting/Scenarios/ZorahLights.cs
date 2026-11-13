// The Zorah scene cache with moving point lights: some circle points across the scene, others wander along looping
// curves through it. Runs until stopped (script.stop, or closing the demo); the fly camera stays yours once the
// opening view is set.
//   BasicRenderer.exe --script scenarios/ZorahLights.cs
// ZORAH_SCENE overrides where the scene cache is (a *.brscene.json, or the folder holding exactly one).
// ZORAH_FREEZE_LIGHTS=1 leaves every light at its starting point (the animations are paused before their first frame),
// for repeatable measurements. ZORAH_KEEP_CAMERA=1 keeps the remembered camera pose instead of the opening view.
//
// Each of the model's ~9,500 parts becomes its own entity, so placing it takes a moment after the load.

[Scenario("zorah-lights", TimeoutSeconds = 0, Description = "Zorah with orbiting and wandering point lights.")]
public sealed class ZorahLights : Scenario
{
    const string DefaultScene = @"D:\Caches\zorah\zorah_textured_public.v1.brscene.json";

    // Light counts and look. Every point light casts shadows unless Shadows is false.
    const int Orbiters = 10;
    const int Wanderers = 6;
    const float Intensity = 250f;
    const float Range = 18f;
    const bool Shadows = true;

    // The region the lights move in: the bulk of Zorah's parts, from their placements in the manifest.
    static readonly Vec3 RegionMin = (-35, 0, -25);
    static readonly Vec3 RegionMax = (60, 0, 25);

    static readonly Vec3[] Palette =
    {
        (1.0, 0.55, 0.25), (0.35, 0.6, 1.0), (1.0, 0.3, 0.45), (0.45, 1.0, 0.55),
        (1.0, 0.85, 0.4), (0.7, 0.45, 1.0), (0.3, 0.95, 0.95), (1.0, 1.0, 1.0),
    };

    public override async Task Run(ScenarioContext ctx)
    {
        var path = System.Environment.GetEnvironmentVariable("ZORAH_SCENE") is { Length: > 0 } custom ? custom : DefaultScene;
        ctx.Log($"loading {path}");
        var zorah = await ctx.Scene.LoadModel(path, name: "zorah", timeout: TimeSpan.FromMinutes(10));
        ctx.Log("zorah placed");

        bool freeze = System.Environment.GetEnvironmentVariable("ZORAH_FREEZE_LIGHTS") is "1" or "true";
        var random = new Random(7);
        float Lerp(float a, float b, double t) => a + (b - a) * (float)t;
        Vec3 RandomPoint(double minHeight, double maxHeight) => new(
            Lerp(RegionMin.X, RegionMax.X, random.NextDouble()),
            Lerp((float)minHeight, (float)maxHeight, random.NextDouble()),
            Lerp(RegionMin.Z, RegionMax.Z, random.NextDouble()));

        // Orbiters: spread over a grid of centres, alternating direction, each at its own speed and height.
        int columns = (int)Math.Ceiling(Math.Sqrt(Orbiters * 2.0));
        int rows = (int)Math.Ceiling(Orbiters / (double)columns);
        for (int i = 0; i < Orbiters; i++)
        {
            int column = i % columns, row = i / columns;
            var center = new Vec3(
                Lerp(RegionMin.X, RegionMax.X, (column + 0.5) / columns),
                0,
                Lerp(RegionMin.Z, RegionMax.Z, (row + 0.5) / rows));
            double radius = 4 + random.NextDouble() * 5;
            double height = 1.5 + random.NextDouble() * 3;
            double seconds = 8 + random.NextDouble() * 10;
            var light = ctx.Scene.PointLight(center + new Vec3((float)radius, (float)height, 0), Intensity, Palette[i % Palette.Length],
                Range, shadows: Shadows, name: $"orbiter {i}");
            var orbit = light.Animate().Orbit(center, radius, height, seconds, turns: i % 2 == 0 ? 1 : -1, lookAtCenter: false).Loop();
            if (freeze) orbit.Pause();
        }

        // Wanderers: a closed Catmull-Rom loop through random points (the last point is the start, so loops are seamless).
        for (int i = 0; i < Wanderers; i++)
        {
            var start = RandomPoint(1.5, 6);
            var points = Enumerable.Range(0, 5).Select(_ => RandomPoint(1.5, 6)).Append(start).ToArray();
            var light = ctx.Scene.PointLight(start, Intensity, Palette[(i + 3) % Palette.Length], Range, shadows: Shadows,
                name: $"wanderer {i}");
            var wander = light.Animate().Path(points, 20 + random.NextDouble() * 15).Loop();
            if (freeze) wander.Pause();
        }

        // An opening overview, then the fly camera is handed back. ZORAH_KEEP_CAMERA=1 keeps the demo's remembered pose
        // instead (logs/demo_camera_state.json), for measuring from a saved view.
        if (System.Environment.GetEnvironmentVariable("ZORAH_KEEP_CAMERA") is not ("1" or "true"))
        {
            ctx.Camera.Fov(60).LookAt(eye: (-45, 22, 38), target: (15, 2, 0));
            await ctx.Frames(2);
            ctx.Camera.ReleaseControl();
        }

        ctx.Log($"{Orbiters} orbiting and {Wanderers} wandering point lights{(freeze ? " (frozen)" : "")}; running until stopped");
        await ctx.Until(() => false, timeout: null);
    }
}
