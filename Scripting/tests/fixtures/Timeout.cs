// Fixture: waits forever. Expected: failed by its 0.5 s timeout, after its finally block has run.
[Scenario("timeout", TimeoutSeconds = 0.5)]
public sealed class Timeout : Scenario
{
    public override async Task Run(ScenarioContext ctx)
    {
        try {
            await ctx.Until(() => false);
        } finally {
            ctx.Log("cleanup ran");
        }
    }
}
