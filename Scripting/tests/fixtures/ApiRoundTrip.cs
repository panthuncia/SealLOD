// Fixture for ScriptHostTests: calls every scene entry of the ABI through the managed API against the test's fake
// backend, which answers predictably (see ScriptHostTests.cpp). Passing proves the marshalling both ways.
[Scenario("api-round-trip")]
public sealed class ApiRoundTrip : Scenario
{
    public override async Task Run(ScenarioContext ctx)
    {
        var model = ctx.Scene.Instantiate("ApiRoundTrip.cs", (1, 2, 3), name: "probe");
        ctx.Check(model.IsReady, "the fake backend places models at once");
        ctx.CheckNear(model.Position, (1, 2, 3), 0, "transform written at creation reads back");
        model.Rotation = Quat.AxisAngle(Vec3.UnitY, 30);
        model.Scale = (2, 3, 4);
        var t = model.Transform;
        ctx.Check(Quat.AngleBetween(t.Rotation, Quat.AxisAngle(Vec3.UnitY, 30)) < 0.05 && t.Scale == (2, 3, 4), $"rotation and scale round-trip: {t}");
        ctx.CheckNear(model.WorldPosition!.Value, (1, 2, 3), 0, "the fake's world matrix carries the position in row 4");

        var child = ctx.Scene.Instantiate("ApiRoundTrip.cs", name: "child");
        child.Parent = model;
        ctx.Check(child.Parent == model, "parent set");

        try {
            ctx.Scene.Instantiate("no/such/file.glb");
            ctx.Fail("a missing model should throw");
        } catch (ScriptApiException e) {
            ctx.Check(e.IsNotFound && e.Message.Contains("not found"), $"not found, with the host's message: {e.Message}");
        }

        var camera = ctx.Camera;
        camera.LookAt((0, 0, 5), (0, 0, 0));
        ctx.CheckNear(camera.Position, (0, 0, 5), 0, "camera pose round-trips");
        camera.FieldOfView = 45;
        ctx.Check(Math.Abs(camera.FieldOfView - 45) < 1e-4 && Math.Abs(camera.Aspect - 1.5f) < 1e-6, $"projection round-trips ({camera.FieldOfView}, {camera.Aspect})");

        var light = ctx.Scene.SpotLight((0, 4, 0), (0, -1, 0), intensity: 50, innerConeDegrees: 10, outerConeDegrees: 20, color: (1, 0, 0), range: 9);
        light.Intensity = 75;
        ctx.Check(light.Kind == LightKind.Spot && Math.Abs(light.Intensity - 75) < 1e-6 && Math.Abs(light.Range - 9) < 1e-6
            && Math.Abs(light.OuterConeDegrees - 20) < 1e-3, "light parameters round-trip");

        ctx.Scene.Environment("sky");
        var status = ctx.Scene.Status;
        ctx.Check(status.Frame == 1234 && status.PendingWork == 5 && status.WorkloadSignature == 0xABCDEF && status.PendingShaderCompiles == 6 && status.PendingModels == 7,
            $"scene status round-trips: {status}");

        ctx.Settings.Set("flag", true);
        ctx.Settings.Set("scale", 2.5f);
        ctx.Settings.Set("count", 9u);
        ctx.Check(ctx.Settings.GetBool("flag") && Math.Abs(ctx.Settings.GetFloat("scale") - 2.5f) < 1e-6 && ctx.Settings.GetUInt("count") == 9,
            "settings round-trip");
        try {
            ctx.Settings.Set("flag", 1.0f);
            ctx.Fail("a type mismatch should throw");
        } catch (ScriptApiException e) {
            ctx.Check(e.IsTypeMismatch, "type mismatch status");
        }

        var echo = ctx.Commands.Invoke("echo", new { value = "héllo" });
        ctx.Check(echo.GetProperty("request").GetProperty("value").GetString() == "héllo", "commands carry UTF-8 both ways");

        model.Destroy();
        try {
            model.Position = (0, 0, 0);
            ctx.Fail("a destroyed entity's handle should be stale");
        } catch (ScriptApiException e) {
            ctx.Check(e.IsStaleHandle, "stale handle status");
        }
        await ctx.NextFrame();
    }
}
