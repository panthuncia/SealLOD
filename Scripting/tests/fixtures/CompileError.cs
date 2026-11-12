// Fixture: does not compile. Expected: compile_error with a diagnostic on line 7.
[Scenario("compile-error")]
public sealed class CompileError : Scenario
{
    public override async Task Run(ScenarioContext ctx)
    {
        int x = "not an int";
        await ctx.NextFrame();
    }
}
