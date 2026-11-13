// Five cubes on a floor, spinning with staggered starts and bobbing, while the camera orbits them once.
//   BasicRenderer.exe --script scenarios/SpinningCubes.cs --script-exit --script-fixed-dt 0.0166667

[Scenario("spinning-cubes", TimeoutSeconds = 90, Description = "Rigid entity animation and a camera orbit.")]
public sealed class SpinningCubes : Scenario
{
    public override async Task Run(ScenarioContext ctx)
    {
        var scene = ctx.Scene;
        var sun = scene.DirectionalLight(direction: (-0.4, -1.0, -0.3), intensity: 6f);

        var floor = scene.Instantiate("assets/plane.glb");
        var cubes = Enumerable.Range(0, 5)
            .Select(i => scene.Instantiate("assets/cube.glb", position: (i * 2.0 - 4.0, 0.5, 0.0)))
            .ToArray();
        await Task.WhenAll(cubes.Select(c => c.WhenReady()).Append(floor.WhenReady()));

        for (int i = 0; i < cubes.Length; i++) {
            cubes[i].Animate().Delay(i * 0.2).Spin(Vec3.UnitY, 90);
            cubes[i].Animate().Delay(i * 0.2).MoveBy((0, 0.5, 0), 0.75, Ease.InOutSine).PingPong();
        }

        var camera = ctx.Camera.Fov(60).LookAt(eye: (0, 3, 12), target: (0, 0.5, 0));
        await ctx.WaitUntilStable();

        await camera.Orbit(center: (0, 0.5, 0), radius: 12, height: 3, seconds: 6, turns: 1, Ease.InOutSine);
        ctx.CheckNear(camera.Position, (0, 3.5f, 12), 0.05f, "a full orbit ends where it started");

        await sun.FadeTo(1f, 1.5);
        ctx.Check(Math.Abs(sun.Intensity - 1f) < 1e-4f, "the sun faded to 1");

        await ctx.Frames(2);
        ctx.Check(cubes.All(c => c.IsReady), "every cube is still in the scene");
        ctx.Check(cubes[0].WorldPosition is { } p && p.Y > 0.4f, "the first cube is rendered above the floor");
    }
}
