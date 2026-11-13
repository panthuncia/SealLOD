# C# scenario scripting

Scenarios are C# files the demo compiles and runs in-process, one slice per frame. Use them to set up and drive
repeatable test situations: place models and lights, move and animate them and the camera, flip settings, run control
commands, capture frames and check outcomes, all without editing and rebuilding C++. The module lives in
[`Scripting/`](../Scripting).

> **Scripted models depend on static import.** Models placed by a script are dynamic ECS renderables
> (`Scene::CreateRenderableEntityECS`). Their mesh and CLod rows reach the GPU only through a published Geometry root,
> and the static scene is the only Geometry root. `ObjectManager::AddObjectsBulk` therefore records the geometry
> coverage its draw records need, and the static import coordinator cuts a geometry-only scene to publish it
> (`StaticImportCoordinator::DrainGeometryCoverage`, woken by `StaticImportStreamingPump`). A host must attach a
> `StaticImportRuntime`, even an empty one, or dynamic models never draw. The demo always does.
>
> **Open:** a scripted model created while a static import (Scene Explorer's Load Model) is still publishing its first
> scene stayed invisible until the next static membership change, although the geometry roots covering it published.
> The demo no longer loads static models at startup (`--model` was removed), so scenarios do not hit this.

## Running a scenario

```powershell
BasicRenderer.exe --script scenarios\SpinningCubes.cs --script-exit --script-fixed-dt 0.0166667 --script-result out\cubes.json
```

| Option | Meaning |
|---|---|
| `--script <file.cs or dir>` | Compile and run the scenario. A directory compiles every `.cs` under it, excluding `bin\` and `obj\`. A relative path resolves against the exe's folder (the demo changes to it). |
| `--scenario <name>` | Choose a scenario when the sources define more than one. |
| `--script-fixed-dt <s>` | Advance the scenario *and* `renderer.Update` by exactly this much per frame. Animation is then frame-rate independent and repeatable. |
| `--script-result <path>` | Write the result document (below) when the scenario ends. |
| `--script-exit` | Quit when the scenario ends. The exit code is 0 for passed, 1 for failed or timed out, 2 for compile error, and 3 when the runtime could not start. |
| `--no-scene-overlap` | Run the scene update synchronously, from the third frame on, so a script's writes reach the very next frame (see [When writes land](#when-writes-land)). |

In the demo's Scene Explorer, **Load Script** opens a `.cs` file picker and compiles and runs the selected scenario.
It starts the scripting runtime on demand and replaces any active scenario. F5 reloads the last loaded script.
The button is disabled in hosts without a script loader.

Without `--script-exit` the demo keeps running after the scenario ends:
- **F5** runs it again from its re-read sources.
- `ctx.RequestExit(code)` ends the process from a script.

**Control pipe.** With `--sampling-control-pipe <name>` the scripting runtime starts even without `--script`. The pipe
then takes these commands (send them with `StatisticalSamplingControl`, TESTING.md):

| Command | Fields |
|---|---|
| `script.run` | `path`, optional `scenario` and `timeout_seconds` |
| `script.reload` | |
| `script.stop` | optional `reason` |
| `script.status` | Replies with `state`, `source`, `scenario`, and `result` (the last result document). |

The demo must run from the runtime root (`build/<preset>/BasicRenderer`); the build deploys `scripting\`,
`scenarios\` (with `assets\`) and `nethost.dll` there. `scripts\deploy_demo.ps1` copies them into a packaged demo.
Running needs the .NET 10 runtime.

## Writing one

```csharp
[Scenario("spinning-cubes", TimeoutSeconds = 90)]
public sealed class SpinningCubes : Scenario
{
    public override async Task Run(ScenarioContext ctx)
    {
        var sun = ctx.Scene.DirectionalLight(direction: (-0.4, -1.0, -0.3), intensity: 6f);
        var cubes = Enumerable.Range(0, 5)
            .Select(i => ctx.Scene.Instantiate("assets/cube.glb", position: (i * 2.0 - 4.0, 0.5, 0.0)))
            .ToArray();
        await Task.WhenAll(cubes.Select(c => c.WhenReady()));

        foreach (var cube in cubes) cube.Animate().Spin(Vec3.UnitY, 90);    // runs alongside, unawaited

        var camera = ctx.Camera.Fov(60).LookAt(eye: (0, 3, 12), target: (0, 0.5, 0));
        await ctx.WaitUntilStable();
        await camera.Orbit(center: (0, 0.5, 0), radius: 12, height: 3, seconds: 6, turns: 1, Ease.InOutSine);
        await sun.FadeTo(1f, 1.5);
        ctx.Check(cubes.All(c => c.IsReady), "every cube is still in the scene");
    }
}
```

The examples are in [`Scripting/Scenarios/`](../Scripting/Scenarios):

| File | Shows |
|---|---|
| `Hello.cs` | Waiting and checks only. |
| `SpinningCubes.cs` | Entity animation and a camera orbit. |
| `CameraPath.cs` | Keyframe tracks, spline paths, parenting and an HDR capture. |
| `LightSweep.cs` | Lights, tweens and settings. |

Their models are in `assets/`, written by `assets/make_assets.py`.

**Implicit usings.** The compiler adds `System`, `System.Collections.Generic`, `System.Linq`, `System.Threading`,
`System.Threading.Tasks` and `BasicRenderer.Scripting` (`ScenarioCompiler.GlobalUsings`).

**Outcome.** The scenario **passes** when `Run` returns without a failed check. It **fails** in four cases:
- `Run` throws (`ctx.Fail` and `ctx.Require` throw on purpose).
- A `ctx.Check` failed. The scenario still runs to the end.
- It runs past `TimeoutSeconds` of scenario time. The default is 120; 0 means no limit.
- It is stopped. D3D12 validation errors (with the debug layer) also stop a running scenario.

**Editing.** To edit with IntelliSense, open `Scripting/Scenarios/Scenarios.csproj`. Running `dotnet build` on it is a
compile check; the host never uses its output.

### API

**Conventions.** Vectors are `Vec3` and convert from tuples: `(1, 2.5, -3)`. Rotations are `Quat`. Angles in the API
are degrees, distances metres. The coordinate system is right-handed with +Y up. Cameras look along their local -Z;
lights shine along their local +Z.

| | |
|---|---|
| **Waiting** | `NextFrame()`, `Frames(n)`, `Seconds(s)`, `Wait(TimeSpan)`, `Until(cond, timeout?)`, `WaitUntilStable(frames = 30, timeout?)`. `WaitUntilStable` waits until nothing streams, uploads or compiles, no model is loading, and the workload signature (the demo sampler's criterion) has been unchanged for `frames` frames. |
| **Time** | `Frame`, `Time`, `DeltaTime` (scenario frames and seconds, both 0 during the first frame), `Cancellation`. |
| **Scene** | `ctx.Scene.Instantiate(path, position?, name?)` places a model at once; `await ctx.Scene.LoadModel(...)` waits until it is in the scene. Also `DirectionalLight`, `PointLight`, `SpotLight`, `Environment(name)`, `Status`, `Entities`. A relative model path is looked up beside the scenario first. Loads are cached by path. |
| **Entities** | `Transform`, `Position`, `Rotation`, `Scale` (all local), `Parent` (keeps the local transform), and `WorldMatrix` / `WorldPosition` (what the renderer last used). Also `State`, `IsReady`, `IsAlive`, `Error`, `WhenReady()`, `Destroy()`, and the fluent `At(...)`, `Rotated(...)`, `Scaled(...)`, `ChildOf(...)`. |
| **Lights** | `Color`, `Intensity`, `Range`, `SourceRadius`, `InnerConeDegrees`, `OuterConeDegrees`, `Direction`, and `FadeTo(intensity, seconds)`. `Kind` and `CastsShadows` are fixed at creation. |
| **Camera** | `ctx.Camera` provides `LookAt(eye, target)`, `FieldOfView` / `Fov(deg)`, `Near`, `Far`, `Aspect`, `Forward`, `FlyTo(eye, target, s)`, `Orbit(center, radius, height, s, turns)`, `Follow(entity, offset)`, `TakeControl()` and `ReleaseControl()`. Moving the camera turns the fly camera off; it comes back when the scenario ends. |
| **Settings** | `ctx.Settings["enableBloom"] = false`, plus `GetBool`, `GetFloat`, `GetInt`, `GetUInt`, `GetByte` and `Set(name, value)`. Settings of type bool, int, uint, byte and float are reachable. What a scenario changes is restored when it ends, unless `KeepChanges` is set. |
| **Commands** | `ctx.Commands.Invoke(name, new { field = ... })` runs any control-pipe command and returns its reply; it throws `CommandFailedException`. `TryInvoke` is the non-throwing form. `await RunJob(name, args)` polls `job.status` once a frame until the job is published. |
| **Capture** | `await ctx.Capture.Hdr(path, samples?, target?)` runs `capture.hdr` (TESTING.md) and returns the finished job. |
| **Outcome** | `Check`, `CheckNear(vec, vec, tolerance, msg)`, `Require`, `Fail`, `RequestExit`, plus `Log`, `Warn` and `Error`. |
| **Errors** | `ScriptApiException` (`IsStaleHandle`, `IsNotFound`, `IsTypeMismatch`, `IsUnsupported`) means the host refused a call. Using a destroyed entity is a stale handle, never a crash. |

### Animation

`entity.Animate()` starts an `AnimationBuilder`.

**Steps** run one after another, each from where the last ended:
- `MoveTo`, `MoveBy`, `RotateTo`, `RotateBy(axis, degrees, s)`, `ScaleTo`, `PoseTo(position, rotation, s)`.
- `Path(points, s, ease, face)`: a Catmull-Rom curve from the current position.
- `Orbit(center, radius, height, s, turns, lookAtCenter)`.
- `Follow(Track<Vec3>)` and `Follow(Track<Quat>)`.
- `Wait(s)`, and `Then(action)`, which runs once per forward pass.
- `Spin(axis, degreesPerSecond)`, which never ends, so it must be the last step.

**Timing:** `Delay(s)`, `Loop(count = forever)`, `PingPong(loops = forever)`. A loop restarts from the start state.

**Values:** `ctx.Tween(from, to, s, apply, ease)` animates any number, and `Light.FadeTo` uses it. `Track.Vec3()`,
`Track.Quat()` and `Track.Float()` build keyframe tracks: `.Key(time, value, ease)`. **Easing** curves are in `Ease.*`:
`Linear`, the `In`/`Out`/`InOut` forms of `Quad`, `Cubic` and `Sine`, `Smooth` and `OutBack`.

An animation starts when created and runs alongside the scenario. `await` it to resume when it ends; `Stop`, `Pause`
and `Resume` control it. Two rules make animations predictable:

- **Absolute time.** Progress is computed from the scenario time since the animation started, never by adding up frame
  times, so it does not drift. With `--script-fixed-dt`, two runs produce identical transforms (`ScriptRuntimeTests`
  checks this).
- **Channels.** An animation writes only the channels its steps touch: position, rotation or scale. A `Spin` and a
  `MoveBy` on the same entity therefore combine.

The runtime compiler allows an animation to be left unawaited (that is the point of them), but **an unawaited `Task` is
a compile error**. A forgotten `await ctx.Frames(3)` would otherwise silently not wait. The IDE project cannot tell
the two apart, so it turns that warning (CS4014) off.

### The threading rule

`Run` and every continuation after an `await` run **on the host's main thread, inside its per-frame tick**, before
`renderer.Update`. Each tick runs in this order:
1. Advance the clock.
2. Evaluate the animations.
3. Resume the due waits, in the order they were registered.
4. Drain posted work.
5. Evaluate the animations again, so ones started this frame apply their starting state now.
6. Evaluate the outcome.

Scenario code is single-threaded. Do not block (`Thread.Sleep`, `.Wait()`, `.Result`).

### When writes land

Script writes to transforms, lights, creations, destroys, parents and the camera lens are **buffered**. The renderer
backend applies them from a scene mutation hook (`Renderer::AddSceneMutationHook`). That hook runs in `Update`'s
`SceneExplorerEdits` stage, the point where the asynchronous scene update is not running. If the previous frame's
update is still running, the hook waits for it, so every frame's writes reach that frame's update and never race the
worker. That is what makes a fixed-step run repeatable.

With scene overlap on (the demo's default), that update is rendered one frame later, as the fly camera is. With
`--no-scene-overlap` it is rendered the same frame. Reads return the script's own last write, except for these, which
report what the renderer last used:
- `WorldMatrix` and `WorldPosition`;
- the camera's pose, until the script moves it;
- the camera's projection.

## Result document

```json
{
  "scenario": "spinning-cubes", "source": "...\\SpinningCubes.cs", "status": "passed",
  "frames": 491, "scenario_seconds": 8.167, "wall_seconds": 9.12,
  "checks": [ { "message": "...", "passed": true, "frame": 398, "time": 6.63, "file": "...", "line": 26 } ],
  "error": { "type": "System.InvalidOperationException", "message": "...", "file": "...\\Throws.cs", "line": 9, "stack": "..." },
  "diagnostics": [ { "file": "...", "line": 7, "column": 17, "id": "CS0029", "severity": "error", "message": "..." } ]
}
```

- `status` is `passed`, `failed` or `compile_error`. Field names are snake_case, like the control pipe's.
- `error` is present only on failure, and points at the innermost frame in the scenario's own code.

## Architecture

```
scenario .cs ──Roslyn──► collectible AssemblyLoadContext, unloaded when the scenario ends
     │ uses
BasicRenderer.Scripting.dll        managed, net10.0: API, animation, scheduler, runner, compiler and compile cache
     │ C function tables (ScriptAbi.h), versioned, append-only
BasicRendererScripting.lib         native host core: hostfxr loader, ScriptHost (needs no renderer)
     │ ScriptBackend (C++ interface)
BasicRendererScriptingRenderer.lib RendererScriptBackend: entity handles, buffered writes, model cache, scene hook
     │
Renderer / Scene (flecs)           and ControlCommandRegistry (the demo's commands, also served on the pipe)
```

| File | Role |
|---|---|
| [`ScriptAbi.h`](../Scripting/include/BasicRendererScripting/ScriptAbi.h) | The C ABI: `BRScriptApiV1` (24 native entries) and `BRScriptRuntimeCallbacks`. |
| [`ScriptHost.h`](../Scripting/include/BasicRendererScripting/ScriptHost.h) | What a host calls: `Create`, `RunScenario`, `Reload`, `Tick`, `Stop`, `State`, `ResultJson`. Options carry the backend and a failure probe. |
| [`ScriptBackend.h`](../Scripting/include/BasicRendererScripting/ScriptBackend.h) | The scene half of the ABI as a C++ interface. Every entry defaults to "unsupported", and a test can supply a fake. |
| [`RendererScriptBackend.cpp`](../Scripting/src/Renderer/RendererScriptBackend.cpp) | That interface over BasicRenderer: generation-checked handles (`HandleTable.h`), the write buffer, payloads loaded on worker threads, and the hook. |
| [`ControlCommandRegistry.h`](../BasicRenderer/include/BasicRenderer/Diagnostics/ControlCommandRegistry.h) | Named JSON commands: `Register`, `Invoke`, `Serve(pipe)`. The demo's commands are registered once, and the pipe and scripts share them. |
| [`Managed/.../Interop/Native.cs`](../Scripting/Managed/BasicRenderer.Scripting/Interop/Native.cs) | `IHostApi` and the native table behind it. Tests substitute `FakeHost`. |

**Renderer additions this needed:**
- `Renderer::AddSceneMutationHook` and `SetCameraInputEnabled`. The latter gates the fly camera on both the synchronous
  and the overlapped path.
- `Scene::InstantiateModel` and `ReleaseModel`: a root entity with one renderable child per part.
- `br::extensions::GetSettingValueType` (`SettingsManager::tryGetSettingType`) and a `uint8` setting getter.

**Lifecycle.**
- When a scenario starts compiling, the backend schedules the previous scenario's entities for destruction and
  restores the fly camera.
- When a scenario ends, the fly camera returns and its settings are restored. Its entities stay, so the result can be
  inspected.
- At shutdown, the backend drops its references without touching the scene.

**Compile cache.** Compiled assemblies are cached in memory and under `BASICRENDERER_SCRIPT_CACHE`. The default is
`cache\scripts` beside the exe; `off` disables the disk cache. The key is the sources' contents plus the identity of
the runtime and Roslyn, so a reload of unchanged sources skips Roslyn (about 0.8 to 1.8 s).

### ABI rules

- `ScriptAbi.h` is pure C, and every type in it is blittable. Its `static_assert`s and the managed mirrors pin the
  layouts; `Vec3`, `Quat` and `Transform` are the public managed types themselves. `Initialize` refuses a size
  mismatch and logs why.
- **Append-only.** A new entry goes at the end of its table and bumps `BR_SCRIPT_ABI_VERSION`. Existing entries never
  move or change signature.
- Strings are NUL-terminated UTF-8, valid during the call. `command_invoke`'s reply is valid until the next call.
- Host-specific API (later, SARP's) goes in an extension table, found through `get_extension(name, version)`.

### Adding an API entry

1. Append the function pointer to `BRScriptApiV1` in `ScriptAbi.h`, bump the version, and update the size
   `static_assert`.
2. Add a virtual method to `ScriptBackend` that defaults to `Unsupported(...)`.
3. Add a thunk in `ScriptHost.cpp` (`Impl::Api...`, wrapped in `Guard`) and fill it in `Create`.
4. Implement it in `RendererScriptBackend`. Anything that touches the scene is recorded and applied in `Apply`.
5. Append the matching `delegate* unmanaged[Cdecl]` field in `NativeAbi.cs`. Add the method to `IHostApi`,
   `NativeHost` and the tests' `FakeHost`.
6. Expose it in `Api/`.
7. Cover it: `fixtures/ApiRoundTrip.cs` with the fake backend in `ScriptHostTests`, plus a `ScriptRuntimeTests` case.

## Build and test

- CMake option `BASICRENDERER_ENABLE_SCRIPTING` defaults to ON. It turns itself off, with a status message, without a
  .NET SDK of version 10 or later and its win-x64 host pack ([`DotNetHosting.cmake`](../Scripting/cmake/DotNetHosting.cmake)).
- The managed projects build with `dotnet build` from custom commands, with explicit source lists in
  `Scripting/CMakeLists.txt`. NuGet restores Roslyn.
- `nethost.dll` ships beside each host, because `libnethost.lib` is built against the static CRT.
- `basicrenderer_scripting_deploy(<target> [DESTINATIONS ...])` deploys the runtime, `nethost.dll` and the scenarios.
- Tests (CPU only, no device):
  - `ScriptRuntimeTests` (29 cases) runs against `FakeHost`. It covers the scheduler, outcomes, compile errors and the
    unawaited-Task rule, the compile cache, animation timing, channels, loops, orbit and tracks, determinism, every API
    wrapper, settings restore, commands and jobs, `WaitUntilStable`, and **all example scenarios running to `passed`**.
  - `ScriptHostTests` loads CoreCLR through the native host. It calls every ABI entry through the managed API against a
    fake `ScriptBackend` (`fixtures/ApiRoundTrip.cs`). It also covers the failure probe, `Reload`, `Stop`, and refusing
    a second host.

```powershell
ctest -C RelWithDebInfo --test-dir build\ninja-renderer-host -R Script --output-on-failure
```

## Status

- **Done (M0 to M2):** everything above. Verified live in the demo:
  - The three examples pass with exit 0, and CameraPath writes a 2560x1600 RGBA32F capture through the command
    registry.
  - The pipe commands `script.run`, `script.reload` (a compile-cache hit), `script.stop` and `script.status` work.
  - The migrated commands answer through the registry.
- **Open:**
  - A model instantiated while a first static import is publishing stays invisible until the next static membership
    change (see the note at the top).
  - Later: port the demo's hard-coded test flags to scenarios, adopt the host in SARP (through an extension table),
    play rigid glTF clips through `Track`, and add static placement for large non-moving content.
