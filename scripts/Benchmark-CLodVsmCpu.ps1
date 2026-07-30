[CmdletBinding()]
param(
    [string]$BuildDir = "out/build/clang-relwithdebinfo",
    [ValidateRange(1, 20)]
    [int]$Runs = 3,
    [ValidateRange(60, 3600)]
    [int]$TimeoutSeconds = 600,
    [ValidateRange(0, 2048)]
    [int]$IoAdmissionDepth = 0,
    [ValidateRange(0, 64)]
    [int]$IoWorkerCount = 0,
    [ValidateRange(0, 32)]
    [int]$IoTaskBatchSize = 0,
    [ValidateRange(0, 16384)]
    [int]$StagedPayloadGroupLimit = 0,
    [ValidateRange(0, 4096)]
    [int]$PageCreditRetryBudget = 0,
    [string]$OutputRoot = "",
    [switch]$RequestTrace,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$resolvedBuildDir = if ([IO.Path]::IsPathRooted($BuildDir)) {
    [IO.Path]::GetFullPath($BuildDir)
} else {
    [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
}
$runtimeDir = Join-Path $resolvedBuildDir "BasicRenderer"
$rendererExe = Join-Path $runtimeDir "BasicRenderer.exe"

if (-not $SkipBuild) {
    & cmake --build $resolvedBuildDir --target BasicRendererDemo -j 8
    if ($LASTEXITCODE -ne 0) {
        throw "BasicRendererDemo build failed with exit code $LASTEXITCODE"
    }
}
if (-not (Test-Path -LiteralPath $rendererExe)) {
    throw "Renderer executable was not found at '$rendererExe'"
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repoRoot (
        "out/clod-timing-benchmarks/{0}" -f
        (Get-Date -Format "yyyyMMdd-HHmmss"))
} elseif (-not [IO.Path]::IsPathRooted($OutputRoot)) {
    $OutputRoot = Join-Path $repoRoot $OutputRoot
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null

function Get-Median([double[]]$Values) {
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 0) {
        return ($sorted[$middle - 1] + $sorted[$middle]) / 2.0
    }
    return $sorted[$middle]
}

function Get-TimingMetric($Report, [string]$Name) {
    $scope = $Report.scopes | Where-Object { $_.name -eq $Name } |
        Select-Object -First 1
    if ($null -eq $scope) {
        return [pscustomobject]@{
            count = 0; mean_us = 0.0; p50_us = 0.0
            p95_us = 0.0; p99_us = 0.0; max_us = 0.0
        }
    }
    return [pscustomobject]@{
        count = [uint64]$scope.inclusive_ns.count
        mean_us = [double]$scope.inclusive_ns.mean / 1000.0
        p50_us = [double]$scope.inclusive_ns.median / 1000.0
        p95_us = [double]$scope.inclusive_ns.p95 / 1000.0
        p99_us = [double]$scope.inclusive_ns.p99 / 1000.0
        max_us = [double]$scope.inclusive_ns.max / 1000.0
    }
}

$environmentNames = @(
    "BASIC_TELEMETRY_OUTPUT_DIR",
    "SARP_CLOD_REQUEST_TRACE_OUTPUT",
    "SARP_CLOD_IO_ADMISSION_DEPTH",
    "SARP_CLOD_IO_WORKER_COUNT",
    "SARP_CLOD_IO_TASK_BATCH_SIZE",
    "SARP_CLOD_STAGED_PAYLOAD_GROUP_LIMIT",
    "SARP_CLOD_PAGE_CREDIT_RETRY_BUDGET"
)
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] =
        [Environment]::GetEnvironmentVariable($name, "Process")
}

$rows = [Collections.Generic.List[object]]::new()
try {
    for ($run = 1; $run -le $Runs; ++$run) {
        $runDir = Join-Path $OutputRoot ("run-{0:D2}" -f $run)
        New-Item -ItemType Directory -Path $runDir -Force | Out-Null
        $telemetryDir = Join-Path $runDir "telemetry"
        $timingPath = Join-Path $telemetryDir "summary.json"
        $tracePath = Join-Path $runDir "request-trace.json"
        $stdoutPath = Join-Path $runDir "stdout.log"
        $stderrPath = Join-Path $runDir "stderr.log"

        $env:BASIC_TELEMETRY_OUTPUT_DIR = $telemetryDir
        $env:SARP_CLOD_REQUEST_TRACE_OUTPUT =
            if ($RequestTrace) { $tracePath } else { $null }
        $env:SARP_CLOD_IO_ADMISSION_DEPTH =
            if ($IoAdmissionDepth) { "$IoAdmissionDepth" } else { $null }
        $env:SARP_CLOD_IO_WORKER_COUNT =
            if ($IoWorkerCount) { "$IoWorkerCount" } else { $null }
        $env:SARP_CLOD_IO_TASK_BATCH_SIZE =
            if ($IoTaskBatchSize) { "$IoTaskBatchSize" } else { $null }
        $env:SARP_CLOD_STAGED_PAYLOAD_GROUP_LIMIT =
            if ($StagedPayloadGroupLimit) {
                "$StagedPayloadGroupLimit"
            } else { $null }
        $env:SARP_CLOD_PAGE_CREDIT_RETRY_BUDGET =
            if ($PageCreditRetryBudget) {
                "$PageCreditRetryBudget"
            } else { $null }

        Write-Host "Running renderer timing benchmark $run/$Runs"
        $process = Start-Process `
            -FilePath $rendererExe `
            -ArgumentList "--clod-vsm-cpu-benchmark" `
            -WorkingDirectory $runtimeDir `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath `
            -PassThru
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            [void]$process.CloseMainWindow()
            if (-not $process.WaitForExit(10000)) {
                Stop-Process -Id $process.Id -Force
                $process.WaitForExit()
            }
            throw "Benchmark timed out; artifacts: $runDir"
        }
        $process.WaitForExit()
        $process.Refresh()
        if ($null -ne $process.ExitCode -and $process.ExitCode -ne 0) {
            throw "Benchmark exited with code $($process.ExitCode): $runDir"
        }
        if (-not (Test-Path -LiteralPath $timingPath)) {
            throw "Timing report was not written to '$timingPath'"
        }

        $timing = Get-Content $timingPath -Raw | ConvertFrom-Json
        $apply = Get-TimingMetric $timing "CLod.ApplyCompletions"
        $allocate = Get-TimingMetric `
            $timing "CLod.ApplyCompletions.AllocatePages"
        $resolve = Get-TimingMetric `
            $timing "CLod.ApplyCompletions.ResolvePayloads"
        $stage = Get-TimingMetric `
            $timing "CLod.ApplyCompletions.StagePayloads"
        $vsm = Get-TimingMetric $timing "CLod.VSM.DependencyBatch"
        $trace = if ($RequestTrace) {
            Get-Content $tracePath -Raw | ConvertFrom-Json
        } else { $null }

        $rows.Add([pscustomobject]@{
            run = $run
            apply_count = [double]$apply.count
            apply_mean_us = [double]$apply.mean_us
            apply_p95_us = [double]$apply.p95_us
            apply_p99_us = [double]$apply.p99_us
            apply_max_us = [double]$apply.max_us
            allocate_p95_us = [double]$allocate.p95_us
            resolve_p95_us = [double]$resolve.p95_us
            stage_p95_us = [double]$stage.p95_us
            vsm_dependency_p95_us = [double]$vsm.p95_us
            resident_groups_per_second = if ($trace) {
                [double]$trace.throughput.resident_requests_per_second
            } else { 0.0 }
            request_to_resident_p95_us = if ($trace) {
                [double]$trace.summary_us.request_to_resident.p95_us
            } else { 0.0 }
            request_to_resident_p99_us = if ($trace) {
                [double]$trace.summary_us.request_to_resident.p99_us
            } else { 0.0 }
            completion_to_commit_p95_us = if ($trace) {
                [double]$trace.summary_us.completion_to_commit.p95_us
            } else { 0.0 }
            timing_report = $timingPath
            request_trace_report =
                if ($trace) { $tracePath } else { "" }
        })
    }
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $savedEnvironment[$name], "Process")
    }
}

$rows | Export-Csv (Join-Path $OutputRoot "runs.csv") -NoTypeInformation
$metricNames = @(
    "apply_mean_us", "apply_p95_us", "apply_p99_us", "apply_max_us",
    "allocate_p95_us", "resolve_p95_us", "stage_p95_us",
    "vsm_dependency_p95_us", "resident_groups_per_second",
    "request_to_resident_p95_us", "request_to_resident_p99_us",
    "completion_to_commit_p95_us"
)
$summary = [ordered]@{ runs = $rows.Count }
foreach ($name in $metricNames) {
    $summary["median_$name"] = Get-Median @(
        $rows | ForEach-Object { [double]$_.$name })
}
$document = [pscustomobject]@{
    generated_at = (Get-Date).ToString("o")
    workload = "--clod-vsm-cpu-benchmark"
    timing_schema_version = 1
    request_trace_enabled = [bool]$RequestTrace
    configuration = @{
        io_admission_depth = $IoAdmissionDepth
        io_worker_count = $IoWorkerCount
        io_task_batch_size = $IoTaskBatchSize
        staged_payload_group_limit = $StagedPayloadGroupLimit
        page_credit_retry_budget = $PageCreditRetryBudget
    }
    runs = $rows
    summary = $summary
}
$document | ConvertTo-Json -Depth 8 |
    Set-Content (Join-Path $OutputRoot "summary.json")

Write-Host ""
Write-Host "Renderer timing benchmark (microseconds)"
$rows | Format-Table run,apply_mean_us,apply_p95_us,apply_p99_us,
    apply_max_us,vsm_dependency_p95_us -AutoSize
Write-Host "Artifacts: $OutputRoot"
