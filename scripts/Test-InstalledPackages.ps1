param(
    [string]$SarpBuildDir = (Join-Path $PSScriptRoot '..\..\build\vs2026-renderer-host'),
    [string]$Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Continue'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$SarpBuildDir = (Resolve-Path $SarpBuildDir).Path
$cachePath = Join-Path $SarpBuildDir 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath)) {
    throw "SARP build cache not found: $cachePath"
}

$cache = Get-Content -LiteralPath $cachePath
function Get-CacheValue([string]$Name) {
    $line = $cache | Where-Object { $_ -match "^$([regex]::Escape($Name)):[^=]+=" } | Select-Object -First 1
    if (-not $line) { return $null }
    return ($line -split '=', 2)[1]
}

$generator = Get-CacheValue 'CMAKE_GENERATOR'
$platform = Get-CacheValue 'CMAKE_GENERATOR_PLATFORM'
$toolset = Get-CacheValue 'CMAKE_GENERATOR_TOOLSET'
$toolchain = Get-CacheValue 'CMAKE_TOOLCHAIN_FILE'
$vcpkgRoot = Join-Path $SarpBuildDir 'vcpkg_installed'
$vcpkgTriplet = Get-CacheValue 'VCPKG_TARGET_TRIPLET'
if (-not $vcpkgTriplet) { $vcpkgTriplet = 'x64-windows-static-md' }
$vcpkgPrefix = Join-Path $vcpkgRoot $vcpkgTriplet
if (-not (Test-Path -LiteralPath $vcpkgPrefix)) {
    throw "SARP vcpkg package prefix not found: $vcpkgPrefix"
}

$runRoot = Join-Path $repoRoot (Join-Path 'out' ("installed-package-consumer-" + [guid]::NewGuid().ToString('N')))
$prefix = Join-Path $runRoot 'prefix'
New-Item -ItemType Directory -Path $prefix | Out-Null

function Invoke-CMake([string[]]$Arguments, [string]$Operation) {
    $output = & cmake @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        $output | ForEach-Object { Write-Host $_ }
        throw "$Operation failed with exit code $exitCode"
    }
}

$headerPackages = @(
    (Join-Path $SarpBuildDir 'BasicRenderer/ThirdParty/Streamline'),
    (Join-Path $SarpBuildDir 'BasicRenderer/ThirdParty/pix')
)
foreach ($packageBuild in $headerPackages) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageBuild 'cmake_install.cmake'))) {
        throw "Header package install rules not generated: $packageBuild"
    }
    Invoke-CMake @('--install', $packageBuild, '--config', $Configuration, '--prefix', $prefix) "Installing header package at $packageBuild"
}

$packages = @('BasicRHI', 'BasicTelemetry', 'BasicScene', 'OpenRenderGraph', 'ORGModuleServices')
foreach ($package in $packages) {
    $packageBuild = Join-Path $SarpBuildDir (Join-Path 'BasicRenderer' $package)
    if (-not (Test-Path -LiteralPath (Join-Path $packageBuild 'cmake_install.cmake'))) {
        throw "Install rules not generated for $package at $packageBuild"
    }
    Invoke-CMake @('--install', $packageBuild, '--config', $Configuration, '--prefix', $prefix) "Installing $package"
}

foreach ($package in $packages) {
    $consumerBuild = Join-Path $runRoot ("consumer-" + $package)
    $configureArgs = @('-S', (Join-Path $repoRoot 'tests/InstalledPackageConsumer'), '-B', $consumerBuild,
        "-DCONSUMER_PACKAGE=$package")
    if ($generator) { $configureArgs += @('-G', $generator) }
    if ($platform) { $configureArgs += @('-A', $platform) }
    if ($toolset) { $configureArgs += @('-T', $toolset) }
    if ($toolchain) { $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain" }
    $configureArgs += "-DVCPKG_TARGET_TRIPLET=$vcpkgTriplet"
    $configureArgs += "-DCMAKE_CONFIGURATION_TYPES=$Configuration"
    $configureArgs += "-DCMAKE_PREFIX_PATH=$prefix;$vcpkgPrefix"
    Invoke-CMake $configureArgs "Configuring $package consumer"

    Invoke-CMake @('--build', $consumerBuild, '--config', $Configuration) "Building $package consumer"
    Write-Host "$package independent consumer passed."
}

Write-Host "All five independent installed-package consumers passed. Staged files: $runRoot"
