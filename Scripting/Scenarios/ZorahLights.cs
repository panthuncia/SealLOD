// The Zorah scene cache with moving point lights: some circle points across the scene, others wander along looping
// curves through it. Runs until stopped (script.stop, or closing the demo); the fly camera stays yours once the
// opening view is set.
//   BasicRenderer.exe --script scenarios/ZorahLights.cs
// ZORAH_SCENE overrides where the scene cache is (a *.brscene.json, or the folder holding exactly one).
// ZORAH_FREEZE_LIGHTS=1 leaves every light at its starting point (the animations are paused before their first frame),
// for repeatable measurements. ZORAH_KEEP_CAMERA=1 keeps the remembered camera pose instead of the opening view.
// ZORAH_LIGHT_SCALE=n multiplies the number of orbiters and wanderers (stochastic lighting's scaling runs use 16 and 64;
// 0 leaves only the sun).
// ZORAH_LIGHT_RANGE=r gives every point light a range of r (default 18). Fewer metres means less overlap per cluster, to
// separate "many lights" from "many lights on every pixel" in scaling runs.
// ZORAH_LIGHT_INTENSITY=x multiplies every point light's intensity (default 1, 250 each). Many lights saturate the image
// (256 and more read nearly white through debug output 77), so high-count image checks scale it down, e.g. 1/64 at 1024.
// ZORAH_SOURCE_RADIUS=r gives every light a source radius of r (default 0: the light model's 1 cm floor, near-hard shadows).
// ZORAH_LIGHT_SPEED=s makes every light s times as fast (default 1), for stressing temporal filters.
// ZORAH_LIGHT_LAYOUT=sweep replaces the orbiters and wanderers with the trail test: ZORAH_SWEEP_LIGHTS (default 4) lights
// skimming the ground back and forth along X, nearly parallel to it, under an overhead camera held ZORAH_SWEEP_CAMERA_HEIGHT
// (5) m up over (ZORAH_SWEEP_X, ZORAH_SWEEP_Z) = (-7, 0). ZORAH_SWEEP_GROUND (0) is the ground's height there;
// ZORAH_SWEEP_HEIGHT (0: on the ground) and ZORAH_SWEEP_DIP (0) are the lights' height above it and their gentle swing, in metres;
// ZORAH_SWEEP_SECONDS (2) is one crossing, divided by ZORAH_LIGHT_SPEED.
// ZORAH_CAMERA_MOTION flies the camera on a loop around the scene, low enough that the cluster LOD cut, the ray-tracing
// BVH and the culled set keep changing:
//   profile  holds the loop's start pose until a profile (profile.run on the control pipe) starts settling, flies the
//            loop from its start while that profile settles and samples, and goes back to the start pose when it ends.
//            Readiness needs a still camera, and every profile measures the same stretch of the loop.
//   always   flies the loop from the start, for watching it.
// ZORAH_CAMERA_SECONDS=s is the length of one lap (default 12).
// ZORAH_CAMERA_T=t holds the camera still at a fraction t (0..1) of that loop.
// ZORAH_LOD_ERROR_PIXELS=p sets clodLodErrorPixels to p before the load, as timing runs do (3, with
// SARP_TEXTURE_FEEDBACK_FORCE_MIP=4) to keep the scene under the VRAM limit.
//
// Each of the model's ~9,500 parts becomes its own entity, so placing it takes a moment after the load.

[Scenario("zorah-lights", TimeoutSeconds = 0, Description = "Zorah with orbiting and wandering point lights.")]
public sealed class ZorahLights : Scenario
{
    const string DefaultScene = @"D:\Caches\zorah\zorah_textured_public.v1.brscene.json";

    // Light counts and look. Every point light casts shadows unless Shadows is false.
    const int BaseOrbiters = 10;
    const int BaseWanderers = 6;
    const float BaseIntensity = 250f;
    const float DefaultRange = 18f;
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
        if (float.TryParse(System.Environment.GetEnvironmentVariable("ZORAH_LOD_ERROR_PIXELS"),
                System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var lodErrorPixels)
            && lodErrorPixels > 0)
        {
            ctx.Settings["clodLodErrorPixels"] = lodErrorPixels;
            ctx.Log($"clodLodErrorPixels {lodErrorPixels}");
        }
        ctx.Log($"loading {path}");
        var zorah = await ctx.Scene.LoadModel(path, name: "zorah", timeout: TimeSpan.FromMinutes(10));
        ctx.Log("zorah placed");

        bool freeze = System.Environment.GetEnvironmentVariable("ZORAH_FREEZE_LIGHTS") is "1" or "true";
        int scale = int.TryParse(System.Environment.GetEnvironmentVariable("ZORAH_LIGHT_SCALE"), out var parsedScale) && parsedScale >= 0
            ? parsedScale : 1;
        float sourceRadius = float.TryParse(System.Environment.GetEnvironmentVariable("ZORAH_SOURCE_RADIUS"),
            System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var parsedRadius)
            && parsedRadius > 0 ? parsedRadius : 0f;
        float speed = float.TryParse(System.Environment.GetEnvironmentVariable("ZORAH_LIGHT_SPEED"),
            System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var parsedSpeed)
            && parsedSpeed > 0 ? parsedSpeed : 1f;
        float Range = float.TryParse(System.Environment.GetEnvironmentVariable("ZORAH_LIGHT_RANGE"),
            System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var parsedRange)
            && parsedRange > 0 ? parsedRange : DefaultRange;
        float intensityScale = float.TryParse(System.Environment.GetEnvironmentVariable("ZORAH_LIGHT_INTENSITY"),
            System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var parsedIntensity)
            && parsedIntensity > 0 ? parsedIntensity : 1f;
        float Intensity = BaseIntensity * intensityScale;
        int Orbiters = BaseOrbiters * scale;
        int Wanderers = BaseWanderers * scale;
        var random = new Random(7);
        float Lerp(float a, float b, double t) => a + (b - a) * (float)t;
        Vec3 RandomPoint(double minHeight, double maxHeight) => new(
            Lerp(RegionMin.X, RegionMax.X, random.NextDouble()),
            Lerp((float)minHeight, (float)maxHeight, random.NextDouble()),
            Lerp(RegionMin.Z, RegionMax.Z, random.NextDouble()));

        // ZORAH_LIGHT_LAYOUT=sweep: the trail test (docs/stochastic_lighting.md, "Moving-light trails"). Instead of orbiters
        // and wanderers, a few lights skim the ground in straight lines, nearly parallel to it and just above it, back and
        // forth across a held overhead view close to the ground: the case where the denoiser's history visibly sticks.
        if (System.Environment.GetEnvironmentVariable("ZORAH_LIGHT_LAYOUT")?.ToLowerInvariant() is "sweep")
        {
            float F(string name, float fallback) => float.TryParse(System.Environment.GetEnvironmentVariable(name),
                System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var v) ? v : fallback;
            float ground = F("ZORAH_SWEEP_GROUND", 0f);           // the ground's height under the view, metres
            float baseHeight = F("ZORAH_SWEEP_HEIGHT", 0.0f);     // the lanes' height above it
            float dip = F("ZORAH_SWEEP_DIP", 0.0f);              // a gentle swing above and below that, so they graze it
            float crossing = F("ZORAH_SWEEP_SECONDS", 2f) / speed; // seconds to cross the view one way
            float cameraHeight = F("ZORAH_SWEEP_CAMERA_HEIGHT", 5f);
            float cx = F("ZORAH_SWEEP_X", -7f);
            float cz = F("ZORAH_SWEEP_Z", 0f);
            int lanes = int.TryParse(System.Environment.GetEnvironmentVariable("ZORAH_SWEEP_LIGHTS"), out var parsedLanes) && parsedLanes > 0
                ? parsedLanes : 4;
            // Looking straight down with a 60 degree vertical FOV at 16:10, the view covers about +-0.58 h along Z (up the
            // screen) and +-0.92 h along X. Lanes run along X, spread over Z, and overshoot both edges a little.
            float halfZ = 0.58f * cameraHeight * 0.8f, halfX = 0.92f * cameraHeight * 1.2f;
            for (int i = 0; i < lanes; i++)
            {
                float z = cz + Lerp(-halfZ, halfZ, (i + 0.5) / lanes);
                bool reverse = i % 2 == 1;
                // Out and back along the lane, five points each way, with the height swinging gently: nearly parallel to the
                // ground, so the light's sphere slides along it.
                var points = new List<Vec3>();
                for (int k = 0; k < 10; k++)
                {
                    double along = k < 5 ? k / 4.0 : (9 - k) / 4.0;
                    if (reverse) along = 1 - along;
                    float h = ground + baseHeight + (k % 2 == 0 ? -dip : dip);
                    points.Add(new Vec3(cx + Lerp(-halfX, halfX, along), h, z));
                }
                int shift = (int)(random.NextDouble() * points.Count);
                var loop = points.Skip(shift).Concat(points.Take(shift)).ToList();
                loop.Add(loop[0]);
                var light = ctx.Scene.PointLight(loop[0], Intensity, Palette[i % Palette.Length], Range, sourceRadius, shadows: Shadows,
                    name: $"sweeper {i}");
                var sweep = light.Animate().Path(loop.ToArray(), 2 * crossing).Loop();
                if (freeze) sweep.Pause();
            }
            ctx.Camera.Fov(60).LookAt(eye: (cx, ground + cameraHeight, cz), target: (cx, ground, cz), up: (0, 0, -1));
            ctx.Log($"{lanes} sweeping point lights (range {Range}, source radius {sourceRadius}, {crossing:0.##} s a crossing, {baseHeight} +- {dip} m above y = {ground}, intensity {Intensity}){(freeze ? " (frozen)" : "")}; overhead camera held {cameraHeight} m up at ({cx}, {cz})");
            await ctx.Until(() => false, timeout: null);
            return;
        }

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
            double seconds = (8 + random.NextDouble() * 10) / speed;
            var light = ctx.Scene.PointLight(center + new Vec3((float)radius, (float)height, 0), Intensity, Palette[i % Palette.Length],
                Range, sourceRadius, shadows: Shadows, name: $"orbiter {i}");
            var orbit = light.Animate().Orbit(center, radius, height, seconds, turns: i % 2 == 0 ? 1 : -1, lookAtCenter: false).Loop();
            if (freeze) orbit.Pause();
        }

        // Wanderers: a closed Catmull-Rom loop through random points (the last point is the start, so loops are seamless).
        for (int i = 0; i < Wanderers; i++)
        {
            var start = RandomPoint(1.5, 6);
            var points = Enumerable.Range(0, 5).Select(_ => RandomPoint(1.5, 6)).Append(start).ToArray();
            var light = ctx.Scene.PointLight(start, Intensity, Palette[(i + 3) % Palette.Length], Range, sourceRadius, shadows: Shadows,
                name: $"wanderer {i}");
            var wander = light.Animate().Path(points, (20 + random.NextDouble() * 15) / speed).Loop();
            if (freeze) wander.Pause();
        }

        var motion = System.Environment.GetEnvironmentVariable("ZORAH_CAMERA_MOTION")?.ToLowerInvariant();
        if (motion is { Length: > 0 } && motion is not ("profile" or "always"))
            throw new ArgumentException($"ZORAH_CAMERA_MOTION must be profile or always, not '{motion}'");
        if (motion is "profile" or "always")
        {
            double lap = double.TryParse(System.Environment.GetEnvironmentVariable("ZORAH_CAMERA_SECONDS"),
                System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var parsedLap)
                && parsedLap > 0 ? parsedLap : 12.0;
            ctx.Log($"{Orbiters} orbiting and {Wanderers} wandering point lights (range {Range}, source radius {sourceRadius}, speed {speed}, intensity {Intensity}){(freeze ? " (frozen)" : "")}; camera motion '{motion}', {lap} s lap");
            await FlyCameraLoop(ctx, motion == "always", lap);
            return;
        }

        // ZORAH_CAMERA_T=t holds the camera still at a fraction t (0..1) of the fly loop, to compare modes from a loop view.
        if (double.TryParse(System.Environment.GetEnvironmentVariable("ZORAH_CAMERA_T"), System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out var heldT))
        {
            var pose = CameraPose(heldT);
            ctx.Camera.Fov(60).LookAt(pose.Eye, pose.Target, (0, 1, 0));
            ctx.Log($"{Orbiters} orbiting and {Wanderers} wandering point lights (range {Range}, source radius {sourceRadius}, speed {speed}, intensity {Intensity}){(freeze ? " (frozen)" : "")}; camera held at t={heldT}");
            await ctx.Until(() => false, timeout: null);
            return;
        }

        // An opening overview from overhead, looking straight down with -Z up the screen, then the fly camera is handed back.
        // ZORAH_KEEP_CAMERA=1 keeps the demo's remembered pose instead (logs/demo_camera_state.json), for measuring from a saved view.
        if (System.Environment.GetEnvironmentVariable("ZORAH_KEEP_CAMERA") is not ("1" or "true"))
        {
            ctx.Camera.Fov(60).LookAt(eye: (1, 38, 0), target: (1, 0, 0), up: (0, 0, -1));
            await ctx.Frames(2);
            ctx.Camera.ReleaseControl();
        }

        ctx.Log($"{Orbiters} orbiting and {Wanderers} wandering point lights (range {Range}, source radius {sourceRadius}, speed {speed}, intensity {Intensity}){(freeze ? " (frozen)" : "")}; running until stopped");
        await ctx.Until(() => false, timeout: null);
    }

    // The camera's pose a fraction t (0..1) of the way round its loop: an orbit of the region's middle whose distance and
    // height swing twice and three times a lap, staying above the rooftops,, looking at a point that drifts along the scene's long axis. It passes
    // close over one end and far from the other, so the LOD cut and the visible set change all the way round.
    static (Vec3 Eye, Vec3 Target) CameraPose(double t)
    {
        double angle = 2 * Math.PI * t;
        var center = new Vec3(12, 0, 0);
        double radius = 30 + 9 * Math.Sin(2 * angle);
        double height = 36 + 6 * Math.Sin(3 * angle + 1);
        var eye = center + new Vec3((float)(radius * 1.4 * Math.Cos(angle)), (float)height, (float)(radius * 0.8 * Math.Sin(angle)));
        var target = center + new Vec3((float)(18 * Math.Cos(angle + 0.6)), 0, (float)(6 * Math.Sin(angle * 2)));
        return (eye, target);
    }

    static async Task FlyCameraLoop(ScenarioContext ctx, bool always, double lapSeconds)
    {
        var camera = ctx.Camera.Fov(60);
        void Place(double t) { var pose = CameraPose(t); camera.LookAt(pose.Eye, pose.Target, (0, 1, 0)); }

        // The profiler's phase: settling or sampling means the camera flies; anything else means it holds the start pose.
        bool Profiling()
        {
            if (!ctx.Commands.TryInvoke("profile.status", null, out var status)) return false;
            if (!status.TryGetProperty("active", out var active) || !active.GetBoolean()) return false;
            return status.TryGetProperty("phase", out var phase) && phase.GetString() is not "waiting_for_stability";
        }

        double? startedAt = always ? ctx.Time : null;
        Place(0);
        while (true)
        {
            await ctx.NextFrame();
            if (!always)
            {
                bool profiling = Profiling();
                if (profiling && startedAt is null)
                {
                    startedAt = ctx.Time;
                    ctx.Log("camera loop started for a profile");
                }
                else if (!profiling && startedAt is not null)
                {
                    startedAt = null;
                    Place(0);
                    ctx.Log("camera loop stopped; back at the start pose");
                }
            }
            if (startedAt is { } start) Place(((ctx.Time - start) / lapSeconds) % 1.0);
        }
    }
}
