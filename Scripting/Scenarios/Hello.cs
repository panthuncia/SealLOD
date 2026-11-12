// The smallest scenario: proves the hosting path (compile, load, tick, wait, report) without touching the scene.
//   BasicRenderer.exe --script scenarios/Hello.cs --script-exit

[Scenario("hello", TimeoutSeconds = 30, Description = "Logs, waits a few frames and a second, and passes.")]
public sealed class Hello : Scenario
{
    public override async Task Run(ScenarioContext ctx)
    {
        ctx.Log("hello from C#");

        await ctx.Frames(3);
        ctx.Check(ctx.Frame == 3, "three frames elapsed");

        double start = ctx.Time;
        await ctx.Wait(1.Seconds());
        ctx.Check(ctx.Time - start >= 1.0 - 1e-6, "a second of scenario time elapsed");

        ctx.Log($"done at frame {ctx.Frame}, {ctx.Time:0.###} s");
    }
}
