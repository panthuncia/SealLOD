// Lights as scriptable entities: a spot light sweeping across cubes, a point light circling and changing colour, the
// sun fading out, and a setting toggled (and restored when the scenario ends).
//   BasicRenderer.exe --script scenarios/LightSweep.cs --script-exit --script-fixed-dt 0.0166667

[Scenario("light-sweep", TimeoutSeconds = 90, Description = "Light creation, light animation and settings.")]
public sealed class LightSweep : Scenario
{
    public override async Task Run(ScenarioContext ctx)
    {
        var scene = ctx.Scene;
        var sun = scene.DirectionalLight((-0.2, -1.0, -0.4), intensity: 3f);
        scene.Instantiate("assets/plane.glb");
        for (int x = -2; x <= 2; x++)
            for (int z = -2; z <= 2; z++)
                scene.Instantiate("assets/cube.glb", position: (x * 2.5, 0.5, z * 2.5));

        var spot = scene.SpotLight(position: (0, 6, 0), direction: (0, -1, 0), intensity: 400f, innerConeDegrees: 12, outerConeDegrees: 20,
            color: (1.0, 0.85, 0.6));
        var point = scene.PointLight(position: (4, 1.5, 0), intensity: 60f, color: (0.3, 0.6, 1.0), range: 10);

        ctx.Camera.Fov(50).LookAt(eye: (0, 9, 13), target: (0, 0, 0));
        await ctx.WaitUntilStable();

        // The spot light tilts from side to side; the point light circles the grid.
        var sweep = spot.Animate()
            .RotateTo(Quat.PointZ((0.6, -1, 0)), 1.5, Ease.InOutSine)
            .RotateTo(Quat.PointZ((-0.6, -1, 0)), 3, Ease.InOutSine)
            .RotateTo(Quat.PointZ((0, -1, 0)), 1.5, Ease.InOutSine);
        var circle = point.Animate().Orbit(center: (0, 0, 0), radius: 4, height: 1.5, seconds: 6, lookAtCenter: false);
        var tint = ctx.Tween(0, 1, 6, t => point.Color = Vec3.Lerp((0.3f, 0.6f, 1f), (1f, 0.3f, 0.2f), (float)t));
        await Task.WhenAll(sweep.Completion, circle.Completion, tint.Completion);

        ctx.CheckNear(spot.Direction, (0, -1, 0), 1e-3f, "the spot light ends pointing down");
        ctx.CheckNear(point.Color, (1f, 0.3f, 0.2f), 1e-3f, "the point light ends red");

        await sun.FadeTo(0.2f, 2, Ease.OutQuad);

        // Settings reached by name; the menu's "enableBloom" toggles bloom. Restored when the scenario ends.
        bool bloom = ctx.Settings.GetBool("enableBloom");
        ctx.Settings["enableBloom"] = !bloom;
        await ctx.Frames(30);
        ctx.Check(ctx.Settings.GetBool("enableBloom") == !bloom, "bloom toggled");
    }
}
