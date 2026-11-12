using System.Reflection;
using System.Runtime.Loader;

namespace BasicRenderer.Scripting.Runtime;

/// <summary>
/// Holds one compiled scenario assembly so that stopping or reloading the scenario can unload its code. The scripting
/// API itself lives in the default context and is shared: scenarios bind to the already-loaded BasicRenderer.Scripting,
/// never a second copy, so API types are identical on both sides.
/// </summary>
internal sealed class ScenarioLoadContext() : AssemblyLoadContext("BasicRenderer.Scenario", isCollectible: true)
{
    private static readonly Assembly s_scripting = typeof(Scenario).Assembly;

    protected override Assembly? Load(AssemblyName assemblyName) =>
        assemblyName.Name == s_scripting.GetName().Name ? s_scripting : null;
}
