// Fixture: a Task that is not awaited (a forgotten await) is a compile error; an unawaited animation is not.
[Scenario("unawaited-task")]
public sealed class UnawaitedTask : Scenario
{
    public override async Task Run(ScenarioContext ctx)
    {
        var cube = ctx.Scene.Instantiate("cube.glb");
        cube.Animate().Spin(Vec3.UnitY, 90);
        ctx.Frames(3);
        await ctx.NextFrame();
    }
}
