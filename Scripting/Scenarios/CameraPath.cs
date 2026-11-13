// A keyframed camera flight past a ring of spheres, then a parented "moon" orbiting a "planet", and a capture.
//   BasicRenderer.exe --script scenarios/CameraPath.cs --script-exit --script-fixed-dt 0.0166667 --script-result out/camera-path.json

[Scenario("camera-path", TimeoutSeconds = 120, Description = "Keyframe tracks, a spline path, parenting and an HDR capture.")]
public sealed class CameraPath : Scenario
{
    public override async Task Run(ScenarioContext ctx)
    {
        var scene = ctx.Scene;
        scene.DirectionalLight((-0.3, -1.0, 0.2), intensity: 5f);
        scene.Instantiate("assets/plane.glb");

        // A ring of spheres.
        for (int i = 0; i < 8; i++) {
            double angle = i * Math.PI / 4;
            scene.Instantiate("assets/sphere.glb", position: (6 * Math.Cos(angle), 0.5, 6 * Math.Sin(angle)));
        }

        // A planet with a moon parented to it: spinning the planet carries the moon around.
        var planet = scene.Instantiate("assets/sphere.glb", position: (0, 2, 0)).Scaled(1.5f);
        var moon = scene.Instantiate("assets/cube.glb", position: (2, 0, 0)).Scaled(0.3f);
        moon.Parent = planet;
        planet.Animate().Spin(Vec3.UnitY, 45);

        var camera = ctx.Camera.Fov(55);
        camera.LookAt(eye: (0, 6, 14), target: (0, 1, 0));
        await ctx.WaitUntilStable();

        // Keyframes: height rises, then the camera dives through the ring along a spline, facing where it goes.
        var rise = Track.Vec3()
            .Key(0, camera.Position)
            .Key(2, (0, 9, 12), Ease.InOutSine);
        await camera.Animate().Follow(rise);
        await camera.Animate().Path([(8, 3, 4), (0, 1.5, -8), (-8, 3, 4), (0, 4, 10)], seconds: 8, Ease.InOutSine, face: true);
        await camera.FlyTo(eye: (0, 4, 9), target: planet.Position, seconds: 1.5);

        // The moon went round with its parent: its world position is no longer where it started.
        await ctx.Frames(2);
        ctx.Check(moon.WorldPosition is { } m && Vec3.Distance(m, (2 * 1.5f, 2, 0)) > 0.1f, "the moon moved with the spinning planet");

        if (ctx.Commands.TryInvoke("session.status", null, out _)) {
            var job = await ctx.Capture.Hdr("captures/camera-path.dds");
            ctx.Log($"captured {job.GetProperty("path")}");
        }
    }
}
