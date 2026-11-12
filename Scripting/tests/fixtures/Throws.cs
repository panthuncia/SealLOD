// Fixture: throws after two frames. Expected: failed, error located at the throw (line 9).
[Scenario("throws")]
public sealed class Throws : Scenario
{
    public override async Task Run(ScenarioContext ctx)
    {
        await ctx.Frames(2);
        if (ctx.Frame == 2)
            throw new InvalidOperationException("thrown on purpose");
    }
}
