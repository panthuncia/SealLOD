<#
.SYNOPSIS
    Deploys the BasicRenderer demo (BasicRenderer.exe) as a self-contained folder that runs by double-clicking the exe.

.DESCRIPTION
    Copies the demo's executable and everything it loads at run time out of a build: SARP's when BasicRenderer is its
    submodule, otherwise BasicRenderer's own (out\build\<preset>):
      - the exe, from the build output (the runtime directory's copy can be stale);
      - every DLL beside it (D3D12 Agility SDK in D3D\, Streamline and DLSS in NVSL\, DirectStorage, FidelityFX, DXC,
        Slang, TBB, PIX, USD and its plugin info in usd\), the Nsight Perf host DLL, and the MSVC runtime, app-local;
      - the shader sources (compiled at run time), built-in textures, environment textures and lens prescriptions;
      - a warm shader cache: cache\shaders\store, seeded from this build's and any scene cache's (see -ShaderCacheSeed).
    The build's runtime directory links most of these to the source tree; the copy follows the links.

    The shader store (org::services::ShaderStore) keeps a shader while every file its build read still has the same
    contents, with paths relative to the exe's folder. shaders\ and SARPShaders\ are deployed as exact copies, so what
    this build compiled is valid in the deployment.

    No models folder is deployed, so the demo starts with an empty scene. Load a scene with Scene Explorer's
    "Load Folder" (for example D:\Caches\zorah, a folder of *.brscene.json scene caches) or "Load Model" (a file).

    What the demo writes beside itself (cache\, logs\, crashes\, imgui.ini) is left alone on a redeploy.

.PARAMETER Destination
    Where to deploy. Default: D:\BasicRendererDemo.

.PARAMETER BuildDir
    The CMake build directory. Default: SARP's build\ninja-renderer-host when BasicRenderer is SARP's submodule and that
    build exists, otherwise the most recently built directory under out\build that has the demo's runtime folder.

.PARAMETER Configuration
    The build configuration whose exe to deploy. Default: RelWithDebInfo.

.PARAMETER Build
    Build the BasicRendererDemo target first: through SARP's build.cmd (all cores, no install) in a SARP build, otherwise
    with cmake --build.

.PARAMETER NoShaderCache
    Don't seed cache\shaders\store. The first launch, and the first time each scene's materials show, then compile shaders.

.PARAMETER ShaderCacheSeed
    Further cache roots whose shaders\store seeds the deployment's. The build's own cache is always used, and gives the
    index if it has one. A store's index cannot be merged, so the others add only artifacts: a request they answer
    preprocesses but does not compile. Default: D:\Caches\zorah, where Zorah's material shaders were compiled.

.PARAMETER IncludeSymbols
    Also copy BasicRenderer.pdb (about 600 MB), for symbolized crash stacks in logs\.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File BasicRenderer\scripts\deploy_demo.ps1 -Build
#>
[CmdletBinding()]
param(
    [string]$Destination = 'D:\BasicRendererDemo',
    [string]$BuildDir,
    [string]$Configuration = 'RelWithDebInfo',
    [switch]$Build,
    [switch]$NoShaderCache,
    [string[]]$ShaderCacheSeed = @('D:\Caches\zorah'),
    [switch]$IncludeSymbols
)

$ErrorActionPreference = 'Stop'
$rendererRoot = Split-Path -Parent $PSScriptRoot # This repository: BasicRenderer.
$superRoot = Split-Path -Parent $rendererRoot     # SARP, when BasicRenderer is its submodule.
$sarpBuildCmd = Join-Path $superRoot 'build.cmd'
$sarpBuildDir = Join-Path $superRoot 'build\ninja-renderer-host'
if (-not $BuildDir) {
    if ((Test-Path $sarpBuildCmd) -and (Test-Path (Join-Path $sarpBuildDir 'BasicRenderer'))) { $BuildDir = $sarpBuildDir }
    else {
        $BuildDir = Get-ChildItem (Join-Path $rendererRoot 'out\build') -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName 'BasicRenderer\D3D') } |
            Sort-Object LastWriteTime | Select-Object -Last 1 -ExpandProperty FullName
        if (-not $BuildDir) { throw "No build found: pass -BuildDir, the CMake build directory that built BasicRendererDemo." }
    }
}
$BuildDir = (Resolve-Path $BuildDir).Path
$runtime = Join-Path $BuildDir 'BasicRenderer' # BASICRENDERER_OUTPUT_ROOT: the D3D, shaders and texture folders.

if ($Build) {
    Write-Host "Building BasicRendererDemo in $BuildDir..."
    $isSarpBuild = (Test-Path $sarpBuildCmd) -and (Test-Path $sarpBuildDir) -and ($BuildDir -eq (Resolve-Path $sarpBuildDir).Path)
    if ($isSarpBuild) {
        $env:SARP_SKIP_INSTALL = '1' # Not SARP's install into the game: the demo needs none of it.
        & cmd /c "`"$sarpBuildCmd`" $env:NUMBER_OF_PROCESSORS BasicRendererDemo"
        if ($LASTEXITCODE -ne 0) { throw "build.cmd failed with exit code $LASTEXITCODE" }
    }
    else {
        & cmake --build $BuildDir --config $Configuration --target BasicRendererDemo
        if ($LASTEXITCODE -ne 0) { throw "cmake --build failed with exit code $LASTEXITCODE" }
    }
}

# Multi-configuration builds put the exe in a folder per configuration; single-configuration ones don't.
$exe = @(
    (Join-Path $runtime "BasicRenderer\$Configuration\BasicRenderer.exe"),
    (Join-Path $runtime "$Configuration\BasicRenderer.exe"),
    (Join-Path $runtime 'BasicRenderer\BasicRenderer.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) { throw "No demo executable under $runtime for $Configuration. Build BasicRendererDemo first, or pass -Build." }
if (Get-Process BasicRenderer -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Destination, 'OrdinalIgnoreCase') }) {
    throw "BasicRenderer.exe is running from $Destination. Close it first."
}

New-Item -ItemType Directory -Force -Path $Destination | Out-Null

# Files copy through links: File.Copy opens the link's target.
function Copy-RuntimeFile([string]$source, [string]$target) {
    [System.IO.File]::Copy($source, $target, $true)
}

# Directories mirror through links too (robocopy follows them unless told not to). /MIR removes what the build dropped.
function Copy-RuntimeDirectory([string]$name, [string]$sourceRoot = $runtime) {
    $source = Join-Path $sourceRoot $name
    if (-not (Test-Path $source)) { throw "Missing runtime directory $source" }
    $target = Join-Path $Destination $name
    & robocopy $source $target /MIR /NFL /NDL /NJH /NJS /NP /R:2 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy of $name failed with exit code $LASTEXITCODE" }
    $global:LASTEXITCODE = 0
}

Write-Host "Deploying $exe to $Destination"
Copy-RuntimeFile $exe (Join-Path $Destination 'BasicRenderer.exe')
$pdb = [System.IO.Path]::ChangeExtension($exe, '.pdb')
$deployedPdb = Join-Path $Destination 'BasicRenderer.pdb'
if ($IncludeSymbols -and (Test-Path $pdb)) { Copy-RuntimeFile $pdb $deployedPdb }
elseif (Test-Path $deployedPdb) { Remove-Item $deployedPdb } # A stale one would mislabel stacks.

# Every DLL beside the runtime exe: imported (USD, TBB, Streamline's interposer, DirectStorage, PIX, Slang, HLSL parser,
# FidelityFX SSSR) or loaded by name (DXC, the other FidelityFX backends, Nifly).
foreach ($dll in Get-ChildItem -Path $runtime -Filter *.dll -File) {
    Copy-RuntimeFile $dll.FullName (Join-Path $Destination $dll.Name)
}

# Nsight Perf's host DLL is imported, but the build leaves it in the exe's folder and NvPerf's own.
$nvperf = Join-Path $rendererRoot 'ThirdParty\NvPerf\bin\x64\nvperf_grfx_host.dll'
if (-not (Test-Path $nvperf)) { $nvperf = Join-Path (Split-Path -Parent $exe) 'nvperf_grfx_host.dll' }
if (-not (Test-Path $nvperf)) { throw "nvperf_grfx_host.dll not found" }
Copy-RuntimeFile $nvperf (Join-Path $Destination 'nvperf_grfx_host.dll')

# The MSVC runtime and OpenMP, app-local, from the newest Visual Studio's redistributables, so the target machine needs
# no redistributable installed.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath } else { $null }
$redist = if ($vsPath) {
    Get-ChildItem (Join-Path $vsPath 'VC\Redist\MSVC') -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'x64') } | Sort-Object { [version]$_.Name } -ErrorAction SilentlyContinue | Select-Object -Last 1
} else { $null }
if ($redist) {
    foreach ($folder in Get-ChildItem (Join-Path $redist.FullName 'x64') -Directory | Where-Object { $_.Name -match '^Microsoft\.VC\d+\.(CRT|OpenMP)$' }) {
        foreach ($dll in Get-ChildItem $folder.FullName -Filter *.dll -File) { Copy-RuntimeFile $dll.FullName (Join-Path $Destination $dll.Name) }
    }
}
else {
    Write-Warning "No Visual C++ redistributable folder found: the target machine needs the VC++ 2015-2022 x64 runtime installed."
}

# The Agility SDK (D3D12SDKPath .\D3D\), Streamline plugins (NVSL\ beside the exe), USD's plugin info, shader sources
# (shaders\ and SARPShaders\, which the shader store's records name), built-in and environment textures, and the path
# tracer's lens prescriptions (lenses\, which LensManager reads beside the exe).
foreach ($name in 'D3D', 'NVSL', 'usd', 'shaders', 'SARPShaders', 'BuiltinTextures', 'textures', 'lenses') { Copy-RuntimeDirectory $name }

# The C# scenario runtime (scripting\, which --script loads beside the exe; nethost.dll is among the DLLs above) and the
# example scenarios, when the build has scripting (BASICRENDERER_ENABLE_SCRIPTING and a .NET 10 SDK).
if (Test-Path (Join-Path $runtime 'scripting')) { foreach ($name in 'scripting', 'scenarios') { Copy-RuntimeDirectory $name } }

# CompiledShaders\ is a folder the demo writes, not the cache it reads; earlier deploys copied it.
$stale = Join-Path $Destination 'CompiledShaders'
if (Test-Path $stale) { Remove-Item -Recurse -Force $stale }

# Seed the shader store. The first store with an index gives it (index.a, index.b, when newer than the deployment's);
# every seed adds its artifacts (cas\, named by their contents, so the same name is the same artifact). A request the
# index does not cover finds its artifact after preprocessing and does not compile.
if (-not $NoShaderCache) {
    $target = Join-Path $Destination 'cache\shaders\store'
    $hasIndex = $false
    foreach ($root in @((Join-Path $runtime 'cache')) + $ShaderCacheSeed) {
        $source = Join-Path $root 'shaders\store'
        if (-not (Test-Path $source)) { continue }
        Write-Host "Seeding the shader store from $source"
        $files = if ($hasIndex) { @('/XF', 'index.a', 'index.b') } else { @() }
        & robocopy $source $target /E /XO @files /NFL /NDL /NJH /NJS /NP /R:2 /W:1 | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopy of $source failed with exit code $LASTEXITCODE" }
        $global:LASTEXITCODE = 0
        $hasIndex = $hasIndex -or (Test-Path (Join-Path $source 'index.a')) -or (Test-Path (Join-Path $source 'index.b'))
    }
    $manifest = Join-Path $runtime 'cache\shaders\compiled_bundles.txt'
    if (Test-Path $manifest) { Copy-RuntimeFile $manifest (Join-Path $Destination 'cache\shaders\compiled_bundles.txt') }
}

# A previous deploy's models would make the demo load Bistro at start.
$models = Join-Path $Destination 'models'
if (Test-Path $models) { Remove-Item -Recurse -Force $models }

# Every DLL the exe names, other than Windows' and the ones it loads only if present, must now be deployed.
$optional = 'SymSrv.dll', 'tbbbind.dll', 'tbbbind_2_0.dll', 'tbbbind_2_5.dll', 'irml.dll', 'libittnotify.dll', 'tcm.dll'
$deployed = @{}
foreach ($file in Get-ChildItem $Destination -Recurse -File -Filter *.dll) { $deployed[$file.Name.ToLowerInvariant()] = $true }
$system = Join-Path $env:SystemRoot 'System32'
$missing = @()
$bytes = [System.IO.File]::ReadAllBytes((Join-Path $Destination 'BasicRenderer.exe'))
$text = [System.Text.Encoding]::ASCII.GetString($bytes)
foreach ($match in [regex]::Matches($text, '[A-Za-z0-9_.\-]+\.(dll|DLL)')) {
    $name = $match.Value
    if ($name -like 'api-ms-win-*' -or $optional -contains $name -or (Test-Path (Join-Path $system $name))) { continue }
    if (-not $deployed.ContainsKey($name.ToLowerInvariant()) -and $missing -notcontains $name) { $missing += $name }
}
if ($missing.Count -gt 0) { Write-Warning ("Named by the exe but not deployed or in System32: " + ($missing -join ', ')) }

$size = (Get-ChildItem $Destination -Recurse -File | Measure-Object Length -Sum).Sum / 1GB
Write-Host ("Deployed to {0} ({1:N2} GB). Run BasicRenderer.exe; load a scene with Scene Explorer > Load Folder." -f $Destination, $size)
