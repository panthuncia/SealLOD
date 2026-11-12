// Fixture: one failed check, but runs to the end. Expected: failed, two checks recorded, the second failed.
[Scenario("failed-check")]
public sealed class FailedCheck : Scenario
{
    public override async Task Run(ScenarioContext ctx)
    {
        ctx.Check(true, "holds");
        await ctx.NextFrame();
        ctx.Check(1 + 1 == 3, "does not hold");
        await ctx.NextFrame();
    }
}
