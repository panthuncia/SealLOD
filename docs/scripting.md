# C# scenario scripting

Scenarios are C# files that the demo compiles and runs in-process, one slice per frame. They are for setting up and
running repeatable test situations without editing and rebuilding C++. The module lives in [`Scripting/`](../Scripting).

Status: **M0, the hosting foundation.** The runtime loads, compiles, runs, times out, reports and unloads scenarios.
Scene, camera, light and settings access (M1) and outcome tooling such as captures, control commands and hot reload
(M2) build on it. They are listed under [Roadmap](#roadmap).

## Running a scenario

```powershell
BasicRenderer.exe --script scenarios\Hello.cs --script-exit --script-fixed-dt 0.0166667 --script-result out\hello.json
```

| Option | Meaning |
|---|---|
| `--script <file.cs or dir>` | Compile and run the scenario. A directory compiles every `.cs` under it, excluding `bin\` and `obj\`. A relative path resolves against the exe's folder (the demo changes to it). |
| `--scenario <name>` | Choose a scenario when the sources define more than one. |
| `--script-fixed-dt <s>` | Advance the scenario *and* `renderer.Update` by exactly this much per frame, independent of frame rate. |
| `--script-result <path>` | Write the result document (below) when the scenario ends. |
| `--script-exit` | Quit when the scenario ends. The exit code is 0 for passed, 1 for failed or timed out, 2 for compile error, and 3 when the scripting runtime could not start. |

Without `--script-exit` the demo keeps running after the scenario ends. A script can still end the process with
`ctx.RequestExit(code)`.

The demo must run from the runtime root (`build/<preset>/BasicRenderer`); the build deploys `scripting\`,
`scenarios\` and `nethost.dll` there. `scripts\deploy_demo.ps1` copies them into a packaged demo. Running needs the
.NET 10 runtime.

## Writing one

```csharp
[Scenario("hello", TimeoutSeconds = 30)]
public sealed class Hello : Scenario
{
    public override async Task Run(ScenarioContext ctx)
    {
        ctx.Log("hello from C#");
        await ctx.Frames(3);
        ctx.Check(ctx.Frame == 3, "three frames elapsed");
        await ctx.Wait(1.Seconds());
    }
}
```

The file has no `using` lines. The compiler adds `System`, `System.Collections.Generic`, `System.Linq`,
`System.Threading`, `System.Threading.Tasks` and `BasicRenderer.Scripting` implicitly (`ScenarioCompiler.GlobalUsings`).

The scenario **passes** when `Run` returns without a failed check. It **fails** in four cases:
- `Run` throws (`ctx.Fail(message)` and `ctx.Require(cond, message)` throw on purpose).
- A `ctx.Check` failed. The scenario still runs to the end.
- It runs past `TimeoutSeconds` of scenario time. The default is 120; 0 means no limit.
- It is stopped.

| `ScenarioContext` | |
|---|---|
| `NextFrame()`, `Frames(n)` | Resume after 1 or n frames. |
| `Seconds(s)`, `Wait(TimeSpan)` | Resume on the first frame at least that much scenario time later. Always a later frame, so a loop of `Seconds(0)` still advances. |
| `Until(cond, timeout?)` | Resume on the first later frame where `cond()` holds. With a timeout, throw `TimeoutException` when it passes. |
| `Frame`, `Time`, `DeltaTime` | Frames and scenario seconds since the first frame (0 during it), and this frame's step. |
| `Cancellation` | Cancelled on stop or timeout. Pending waits throw `OperationCanceledException`, so `finally` blocks run. |
| `Log`, `Warn`, `Error` | Write to the host's log (the demo's `logs\log.txt`), prefixed `[script] [name fN]`. |
| `Check`, `Require`, `Fail`, `RequestExit` | Outcome, as described above. |

To edit with IntelliSense, open `Scripting/Scenarios/Scenarios.csproj`. Running `dotnet build` on it is a compile
check; the host never uses its output.

### The threading rule

`Run` and every continuation after an `await` run **on the host's main thread, inside its per-frame tick**, before
`renderer.Update`. Scenario code is single-threaded and needs no locks. Awaiting other tasks is fine; for example,
`await Task.Run(...)` resumes on the main thread on a later tick. Blocking with `Thread.Sleep`, `.Wait()` or `.Result`
freezes the frame.

## Result document

```json
{
  "scenario": "hello", "source": "...\\Hello.cs", "status": "passed",
  "frames": 64, "scenario_seconds": 1.05, "wall_seconds": 1.08,
  "checks": [ { "message": "three frames elapsed", "passed": true, "frame": 3, "time": 0.05, "file": "...", "line": 12 } ],
  "error": { "type": "System.InvalidOperationException", "message": "...", "file": "...\\Throws.cs", "line": 9, "stack": "..." },
  "diagnostics": [ { "file": "...", "line": 7, "column": 17, "id": "CS0029", "severity": "error", "message": "..." } ]
}
```

- `status` is `passed`, `failed` or `compile_error`. Field names are snake_case, like the control pipe's.
- `error` is present only on failure. It points at the innermost stack frame in the scenario's own code.
- Scenarios compile with a portable PDB, so stack traces carry file and line.

## Architecture

```
scenario .cs ──Roslyn──► collectible AssemblyLoadContext, unloaded when the scenario ends
     │ uses
BasicRenderer.Scripting.dll   managed, net10.0, default load context: API, FrameScheduler, runner, compiler
     │ C function tables (ScriptAbi.h), versioned, append-only
BasicRendererScripting.lib    native: hostfxr loader, ScriptHost (C++ API for host applications)
     │
host application              the demo today; SARP's renderer host later
```

| File | Role |
|---|---|
| [`include/BasicRendererScripting/ScriptAbi.h`](../Scripting/include/BasicRendererScripting/ScriptAbi.h) | The C ABI: `BRScriptApiV1` (native services) and `BRScriptRuntimeCallbacks` (managed entry points). |
| [`include/BasicRendererScripting/ScriptHost.h`](../Scripting/include/BasicRendererScripting/ScriptHost.h) | What a host application calls: `Create`, `RunScenario`, `Tick`, `Stop`, `State`, `ResultJson`. |
| [`src/DotNetRuntime.cpp`](../Scripting/src/DotNetRuntime.cpp) | nethost → hostfxr → `hdt_load_assembly` and `hdt_get_function_pointer`. |
| [`Managed/.../Runtime/ScriptRuntime.cs`](../Scripting/Managed/BasicRenderer.Scripting/Runtime/ScriptRuntime.cs) | `[UnmanagedCallersOnly]` entry points and the handshake. |
| [`Managed/.../Runtime/ScriptSession.cs`](../Scripting/Managed/BasicRenderer.Scripting/Runtime/ScriptSession.cs) | One scenario at a time: compile off-thread, load, run, report, unload. |
| [`Managed/.../Runtime/FrameScheduler.cs`](../Scripting/Managed/BasicRenderer.Scripting/Runtime/FrameScheduler.cs) | The SynchronizationContext and frame, time and condition waits. |
| [`Managed/.../Runtime/ScenarioRunner.cs`](../Scripting/Managed/BasicRenderer.Scripting/Runtime/ScenarioRunner.cs) | One scenario instance: outcome, timeout, cancellation. |

**Host lifecycle.** `ScriptHost::Create` loads CoreCLR, which can be done once per process, so only one `ScriptHost`
may be alive at a time. It calls `ScriptRuntime.Initialize` with the API table, and the runtime fills in the callback
table. After that, `RunScenario` starts a background compile, and every `Tick` advances the session. Scenarios run back
to back on one host; each one's assembly is unloaded when it ends.

**Tick order.** Each frame, the clock advances. Then the due waits resume in the order they were registered, inline.
Then work posted to the context drains. Finally the outcome is evaluated.

### ABI rules

- `ScriptAbi.h` is pure C, and every type in it is blittable. Its `static_assert`s and the managed mirrors in
  `Interop/NativeAbi.cs` pin the layouts. `Initialize` checks the host's reported struct sizes and refuses a mismatch,
  logging the reason.
- **Append-only.** A new entry goes at the end of its table and bumps `BR_SCRIPT_ABI_VERSION`. Existing entries never
  move or change signature. Update both mirrors and the size `static_assert`s in the same change.
- Strings are NUL-terminated UTF-8, valid only during the call. Entries return a `BRScriptStatus`, and
  `get_last_error` describes a failure. Managed wrappers turn a failure into `ScriptApiException`.
- Host-specific API, such as Skyrim data in SARP, goes in an extension table found through `get_extension(name,
  version)`, not in the core table.

### Adding an API entry

1. Append the function pointer to `BRScriptApiV1` in `ScriptAbi.h`, then bump `BR_SCRIPT_ABI_VERSION` and the size
   `static_assert`.
2. Implement it in `ScriptHost.cpp`: a static member of `Impl` that sets `t_lastError` on failure. Fill it in
   `Create`.
3. Append the matching `delegate* unmanaged[Cdecl]` field to `BRScriptApiV1` in `NativeAbi.cs`, and bump
   `AbiConstants.Version`.
4. Wrap it in `Native.cs` and expose it through the public API (`Api/`).
5. Cover it in `ScriptHostTests`, or in `ScriptRuntimeTests` if it is logic that needs no host.

## Build and test

- CMake option `BASICRENDERER_ENABLE_SCRIPTING` defaults to ON. It turns itself off, with a status message, when no
  .NET SDK of version 10 or later and win-x64 host pack is found ([`cmake/DotNetHosting.cmake`](../Scripting/cmake/DotNetHosting.cmake)).
  Without it the demo builds without `--script`.
- The managed projects build with `dotnet build` from custom commands, with explicit source lists in
  `Scripting/CMakeLists.txt`; add new `.cs` files there and to the `.csproj`. NuGet restores
  `Microsoft.CodeAnalysis.CSharp` (Roslyn).
- `nethost.lib` is the import library, because `libnethost.lib` is built against the static CRT. That is why
  `nethost.dll` ships beside each host.
- `basicrenderer_scripting_deploy(<target> [DESTINATIONS ...])` copies the runtime, `nethost.dll` and the example
  scenarios beside a host.
- Tests (CPU only, no device):
  - `ScriptRuntimeTests` covers scheduler ordering, waits, outcomes, timeout, stop, compile diagnostics, scenario
    selection, and that the example scenarios compile.
  - `ScriptHostTests` covers the native host end to end: it loads CoreCLR, does the handshake, runs passing, failing and
    non-compiling scenarios back to back, and refuses a second host.

```powershell
ctest -C RelWithDebInfo --test-dir build\ninja-renderer-host -R Script --output-on-failure
```

## Roadmap

1. **M1, scene and animation.**
   - Script writes are buffered and applied at the renderer's scene safe point, the `SceneExplorerEdits` stage, where
     the async scene task is not running. A new `Renderer::AddSceneMutationHook` provides that point.
   - Generation-checked entity handles.
   - Rigid model instantiation (`Scene::InstantiateModel`).
   - Camera control, with `Renderer::SetCameraInputEnabled` to turn the fly camera off.
   - Lights.
   - Typed settings, with a type query added to `SettingsManager`.
   - Tweens and keyframe tracks, evaluated from absolute scenario time.
2. **M2, the testing loop.**
   - Extract the demo's control-pipe `if` chain into a command registry, so scripts can call any pipe command
     (`ctx.Command("capture.hdr", ...)`).
   - `script.run`, `script.reload`, `script.status` and `script.stop` on the pipe.
   - `WaitUntilStable`, using the sampler's readiness criterion.
   - D3D12 validation failures fail the scenario.
   - Hot reload and a compile cache, so warm starts skip Roslyn's first compile (about 1.8 s cold).
3. **Later.**
   - Port the demo's hard-coded test flags (`--vsm-orbit-test` and the others) to scenarios.
   - SARP's renderer host, through the same `ScriptHost` and an extension table.
   - Rigid glTF animation clips, which the loader currently drops.
