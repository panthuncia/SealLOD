[CmdletBinding()]
param(
    [string]$BuildDir = "out/build/clang-relwithdebinfo",
    [ValidateSet("On", "Off", "Both")]
    [string]$ParallelSort = "On",
    [ValidateSet("On", "Off", "Both")]
    [string]$DirectHashIngest = "On",
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
    [ValidateSet("On", "Off")]
    [string]$LateCpuPageAllocation = "On",
    [ValidateRange(0, 16384)]
    [int]$StagedPayloadGroupLimit = 0,
    [ValidateRange(0, 4096)]
    [int]$PageCreditRetryBudget = 0,
    [ValidateSet("On", "Off")]
    [string]$SchedulerAging = "Off",
    [ValidateSet("On", "Off")]
    [string]$LiveBackgroundLanes = "Off",
    [string]$OutputRoot = "",
    [switch]$RequestTrace,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$resolvedBuildDir = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    [System.IO.Path]::GetFullPath($BuildDir)
}
else {
    [System.IO.Path]::GetFullPath(
        (Join-Path $repoRoot $BuildDir))
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
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputRoot = Join-Path $repoRoot "out/clod-vsm-cpu-benchmarks/$stamp"
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputRoot)) {
    $OutputRoot = Join-Path $repoRoot $OutputRoot
}
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null

$sortVariants = switch ($ParallelSort) {
    "On" { @("parallel") }
    "Off" { @("serial") }
    default { @("parallel", "serial") }
}
$ingestVariants = switch ($DirectHashIngest) {
    "On" { @("direct") }
    "Off" { @("sorted") }
    default { @("direct", "sorted") }
}
$variants = foreach ($ingest in $ingestVariants) {
    foreach ($sort in $sortVariants) {
        [pscustomobject]@{
            name = "$ingest-$sort"
            ingest = $ingest
            sort = $sort
        }
    }
}

function Get-Median {
    param([double[]]$Values)
    if ($Values.Count -eq 0) {
        return 0.0
    }
    $sorted = @($Values | Sort-Object)
    $middle = [math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) {
        return [double]$sorted[$middle]
    }
    return ([double]$sorted[$middle - 1] +
        [double]$sorted[$middle]) / 2.0
}

$previousBenchmarkOutput = $env:SARP_CLOD_VSM_BENCHMARK_OUTPUT
$previousTiming = $env:SARP_CLOD_VSM_UPGRADE_TIMING
$previousParallelSort = $env:SARP_CLOD_VSM_PARALLEL_SORT
$previousDirectHashIngest = $env:SARP_CLOD_VSM_DIRECT_HASH_INGEST
$previousRequestTraceOutput = $env:SARP_CLOD_REQUEST_TRACE_OUTPUT
$previousIoAdmissionDepth = $env:SARP_CLOD_IO_ADMISSION_DEPTH
$previousIoWorkerCount = $env:SARP_CLOD_IO_WORKER_COUNT
$previousIoTaskBatchSize = $env:SARP_CLOD_IO_TASK_BATCH_SIZE
$previousLateCpuPageAllocation =
    $env:SARP_CLOD_LATE_CPU_PAGE_ALLOCATION
$previousStagedPayloadGroupLimit =
    $env:SARP_CLOD_STAGED_PAYLOAD_GROUP_LIMIT
$previousPageCreditRetryBudget =
    $env:SARP_CLOD_PAGE_CREDIT_RETRY_BUDGET
$previousSchedulerAging = $env:SARP_CLOD_SCHEDULER_AGING
$previousLiveBackgroundLanes =
    $env:SARP_CLOD_LIVE_BACKGROUND_LANES
$runRows = [System.Collections.Generic.List[object]]::new()

try {
    foreach ($variant in $variants) {
        for ($run = 1; $run -le $Runs; ++$run) {
            $runDir = Join-Path $OutputRoot (
                "{0}-run-{1:D2}" -f $variant.name, $run)
            New-Item -ItemType Directory -Path $runDir -Force |
                Out-Null
            $reportPath = Join-Path $runDir "benchmark.json"
            $stdoutPath = Join-Path $runDir "stdout.log"
            $stderrPath = Join-Path $runDir "stderr.log"
            $requestTracePath = Join-Path $runDir "request-trace.json"

            $env:SARP_CLOD_VSM_BENCHMARK_OUTPUT = $reportPath
            $env:SARP_CLOD_VSM_UPGRADE_TIMING = "0"
            $env:SARP_CLOD_VSM_PARALLEL_SORT =
                if ($variant.sort -eq "parallel") { "1" } else { "0" }
            $env:SARP_CLOD_VSM_DIRECT_HASH_INGEST =
                if ($variant.ingest -eq "direct") { "1" } else { "0" }
            $env:SARP_CLOD_REQUEST_TRACE_OUTPUT =
                if ($RequestTrace) { $requestTracePath } else { $null }
            $env:SARP_CLOD_IO_ADMISSION_DEPTH =
                if ($IoAdmissionDepth -gt 0) {
                    [string]$IoAdmissionDepth
                } else { $null }
            $env:SARP_CLOD_IO_WORKER_COUNT =
                if ($IoWorkerCount -gt 0) {
                    [string]$IoWorkerCount
                } else { $null }
            $env:SARP_CLOD_IO_TASK_BATCH_SIZE =
                if ($IoTaskBatchSize -gt 0) {
                    [string]$IoTaskBatchSize
                } else { $null }
            $env:SARP_CLOD_LATE_CPU_PAGE_ALLOCATION =
                if ($LateCpuPageAllocation -eq "On") {
                    "1"
                } else { "0" }
            $env:SARP_CLOD_STAGED_PAYLOAD_GROUP_LIMIT =
                if ($StagedPayloadGroupLimit -gt 0) {
                    [string]$StagedPayloadGroupLimit
                } else { $null }
            $env:SARP_CLOD_PAGE_CREDIT_RETRY_BUDGET =
                if ($PageCreditRetryBudget -gt 0) {
                    [string]$PageCreditRetryBudget
                } else { $null }
            $env:SARP_CLOD_SCHEDULER_AGING =
                if ($SchedulerAging -eq "On") { "1" } else { "0" }
            $env:SARP_CLOD_LIVE_BACKGROUND_LANES =
                if ($LiveBackgroundLanes -eq "On") { "1" } else { "0" }

            Write-Host (
                "Running CLOD VSM CPU benchmark: variant={0} run={1}/{2}" -f
                $variant.name, $run, $Runs)
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
                throw (
                    "Benchmark timed out after {0} seconds; artifacts: {1}" -f
                    $TimeoutSeconds, $runDir)
            }
            # Complete redirected-stream handling and refresh the process
            # object before reading ExitCode. Without the second wait,
            # Windows PowerShell can leave ExitCode unset for GUI processes.
            $process.WaitForExit()
            $process.Refresh()
            $exitCode = $process.ExitCode
            if ($null -ne $exitCode -and $exitCode -ne 0) {
                throw (
                    "Benchmark exited with code {0}; artifacts: {1}" -f
                    $exitCode, $runDir)
            }

            $runtimeLog = Join-Path $runtimeDir "logs/log.txt"
            if (Test-Path -LiteralPath $runtimeLog) {
                Copy-Item -LiteralPath $runtimeLog `
                    -Destination (Join-Path $runDir "renderer.log")
            }
            if (-not (Test-Path -LiteralPath $reportPath)) {
                throw "Benchmark report was not written to '$reportPath'"
            }

            $report = Get-Content -LiteralPath $reportPath -Raw |
                ConvertFrom-Json
            $requestTraceData = if ($RequestTrace) {
                if (-not (Test-Path -LiteralPath $requestTracePath)) {
                    throw "Request trace was not written to '$requestTracePath'"
                }
                Get-Content -LiteralPath $requestTracePath -Raw |
                    ConvertFrom-Json
            }
            else {
                $null
            }
            $batch = $report.summary.dependency_batch
            $runRows.Add([pscustomobject]@{
                variant = $variant.name
                run = $run
                batches = [int]$batch.count
                mean_us = [double]$batch.mean_us
                p50_us = [double]$batch.p50_us
                p95_us = [double]$batch.p95_us
                p99_us = [double]$batch.p99_us
                max_us = [double]$batch.max_us
                inputs = [double]$report.volume.input_records
                unique_inputs =
                    [double]$report.volume.unique_input_records
                expanded_pairs =
                    [double]$report.volume.expanded_dependency_pairs
                peak_active_pairs =
                    [double]$report.volume.peak_active_dependency_pairs
                peak_active_slots =
                    [double]$report.volume.peak_active_dependency_slots
                peak_active_bytes =
                    [double]$report.volume.peak_active_dependency_bytes
                bucket_rehashes =
                    [double]$report.volume.dependency_bucket_rehashes
                traced_resident_requests =
                    if ($requestTraceData) {
                        [double]$requestTraceData.counts.resident
                    } else { 0.0 }
                traced_terminal_requests =
                    if ($requestTraceData) {
                        [double]$requestTraceData.counts.terminal
                    } else { 0.0 }
                traced_active_requests =
                    if ($requestTraceData) {
                        [double]$requestTraceData.counts.active_at_shutdown
                    } else { 0.0 }
                resident_requests_per_second =
                    if ($requestTraceData) {
                        [double](
                            $requestTraceData.throughput.resident_requests_per_second)
                    } else { 0.0 }
                request_to_resident_p95_us =
                    if ($requestTraceData) {
                        [double](
                            $requestTraceData.summary_us.request_to_resident.p95_us)
                    } else { 0.0 }
                live_request_to_resident_p95_us =
                    if ($requestTraceData) {
                        [double](
                            $requestTraceData.summary_us.live_request_to_resident.p95_us)
                    } else { 0.0 }
                request_to_resident_p99_us =
                    if ($requestTraceData) {
                        [double](
                            $requestTraceData.summary_us.request_to_resident.p99_us)
                    } else { 0.0 }
                cpu_queue_wait_p95_us =
                    if ($requestTraceData) {
                        [double]$requestTraceData.summary_us.cpu_queue_wait.p95_us
                    } else { 0.0 }
                disk_io_p95_us =
                    if ($requestTraceData) {
                        [double]$requestTraceData.summary_us.disk_io.p95_us
                    } else { 0.0 }
                completion_to_commit_p95_us =
                    if ($requestTraceData) {
                        [double](
                            $requestTraceData.summary_us.completion_to_commit.p95_us)
                    } else { 0.0 }
                commit_to_resident_p95_us =
                    if ($requestTraceData) {
                        [double](
                            $requestTraceData.summary_us.commit_to_resident.p95_us)
                    } else { 0.0 }
                submit_to_resident_p95_us =
                    if ($requestTraceData) {
                        [double](
                            $requestTraceData.summary_us.submit_to_resident.p95_us)
                    } else { 0.0 }
                ns_per_input =
                    [double]$report.volume.nanoseconds_per_input_record
                ns_per_unique_input =
                    [double]$report.volume.nanoseconds_per_unique_input_record
                ns_per_expanded_pair =
                    [double]$report.volume.nanoseconds_per_expanded_dependency_pair
                report = $reportPath
                request_trace_report =
                    if ($RequestTrace) { $requestTracePath } else { "" }
            })
        }
    }
}
finally {
    $env:SARP_CLOD_VSM_BENCHMARK_OUTPUT = $previousBenchmarkOutput
    $env:SARP_CLOD_VSM_UPGRADE_TIMING = $previousTiming
    $env:SARP_CLOD_VSM_PARALLEL_SORT = $previousParallelSort
    $env:SARP_CLOD_VSM_DIRECT_HASH_INGEST =
        $previousDirectHashIngest
    $env:SARP_CLOD_REQUEST_TRACE_OUTPUT =
        $previousRequestTraceOutput
    $env:SARP_CLOD_IO_ADMISSION_DEPTH =
        $previousIoAdmissionDepth
    $env:SARP_CLOD_IO_WORKER_COUNT =
        $previousIoWorkerCount
    $env:SARP_CLOD_IO_TASK_BATCH_SIZE =
        $previousIoTaskBatchSize
    $env:SARP_CLOD_LATE_CPU_PAGE_ALLOCATION =
        $previousLateCpuPageAllocation
    $env:SARP_CLOD_STAGED_PAYLOAD_GROUP_LIMIT =
        $previousStagedPayloadGroupLimit
    $env:SARP_CLOD_PAGE_CREDIT_RETRY_BUDGET =
        $previousPageCreditRetryBudget
    $env:SARP_CLOD_SCHEDULER_AGING =
        $previousSchedulerAging
    $env:SARP_CLOD_LIVE_BACKGROUND_LANES =
        $previousLiveBackgroundLanes
}

$runCsv = Join-Path $OutputRoot "runs.csv"
$runRows | Export-Csv -LiteralPath $runCsv -NoTypeInformation

$summaryRows = foreach ($variant in $variants) {
    $variantRows = @(
        $runRows | Where-Object variant -eq $variant.name)
    [pscustomobject]@{
        variant = $variant.name
        runs = $variantRows.Count
        median_mean_us = (Get-Median -Values @(
            $variantRows | ForEach-Object { $_.mean_us }))
        median_p50_us = (Get-Median -Values @(
            $variantRows | ForEach-Object { $_.p50_us }))
        median_p95_us = (Get-Median -Values @(
            $variantRows | ForEach-Object { $_.p95_us }))
        median_p99_us = (Get-Median -Values @(
            $variantRows | ForEach-Object { $_.p99_us }))
        median_max_us = (Get-Median -Values @(
            $variantRows | ForEach-Object { $_.max_us }))
        median_batches = (Get-Median -Values @(
            $variantRows | ForEach-Object { $_.batches }))
        median_inputs = (Get-Median -Values @(
            $variantRows | ForEach-Object { $_.inputs }))
        median_expanded_pairs = (Get-Median -Values @(
            $variantRows | ForEach-Object { $_.expanded_pairs }))
        median_peak_active_slots = (Get-Median -Values @(
            $variantRows | ForEach-Object { $_.peak_active_slots }))
        median_peak_active_bytes = (Get-Median -Values @(
            $variantRows | ForEach-Object { $_.peak_active_bytes }))
        median_bucket_rehashes = (Get-Median -Values @(
            $variantRows | ForEach-Object { $_.bucket_rehashes }))
        median_traced_resident_requests = (Get-Median -Values @(
            $variantRows | ForEach-Object {
                $_.traced_resident_requests
            }))
        median_resident_requests_per_second = (Get-Median -Values @(
            $variantRows | ForEach-Object {
                $_.resident_requests_per_second
            }))
        median_request_to_resident_p95_us = (Get-Median -Values @(
            $variantRows | ForEach-Object {
                $_.request_to_resident_p95_us
            }))
        median_request_to_resident_p99_us = (Get-Median -Values @(
            $variantRows | ForEach-Object {
                $_.request_to_resident_p99_us
            }))
        median_live_request_to_resident_p95_us = (Get-Median -Values @(
            $variantRows | ForEach-Object {
                $_.live_request_to_resident_p95_us
            }))
        median_cpu_queue_wait_p95_us = (Get-Median -Values @(
            $variantRows | ForEach-Object {
                $_.cpu_queue_wait_p95_us
            }))
        median_disk_io_p95_us = (Get-Median -Values @(
            $variantRows | ForEach-Object {
                $_.disk_io_p95_us
            }))
        median_completion_to_commit_p95_us = (Get-Median -Values @(
            $variantRows | ForEach-Object {
                $_.completion_to_commit_p95_us
            }))
        median_commit_to_resident_p95_us = (Get-Median -Values @(
            $variantRows | ForEach-Object {
                $_.commit_to_resident_p95_us
            }))
        median_submit_to_resident_p95_us = (Get-Median -Values @(
            $variantRows | ForEach-Object {
                $_.submit_to_resident_p95_us
            }))
        median_ns_per_input = (Get-Median -Values @(
            $variantRows | ForEach-Object { $_.ns_per_input }))
        median_ns_per_expanded_pair = (Get-Median -Values @(
            $variantRows | ForEach-Object {
                $_.ns_per_expanded_pair
            }))
    }
}

$summaryRows | Export-Csv `
    -LiteralPath (Join-Path $OutputRoot "summary.csv") `
    -NoTypeInformation
$summaryDocument = [pscustomobject]@{
    generated_at = (Get-Date).ToString("o")
    build_dir = $resolvedBuildDir
    workload = "--clod-vsm-cpu-benchmark"
    request_trace_enabled = [bool]$RequestTrace
    io_admission_depth = $IoAdmissionDepth
    io_worker_count = $IoWorkerCount
    io_task_batch_size = $IoTaskBatchSize
    late_cpu_page_allocation = $LateCpuPageAllocation
    staged_payload_group_limit = $StagedPayloadGroupLimit
    page_credit_retry_budget = $PageCreditRetryBudget
    scheduler_aging = $SchedulerAging
    live_background_lanes = $LiveBackgroundLanes
    runs = $runRows
    summary = $summaryRows
}
$summaryDocument | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $OutputRoot "summary.json")

Write-Host ""
Write-Host "CLOD VSM CPU benchmark results (microseconds)"
$summaryRows |
    Format-Table variant, runs, median_mean_us, median_p50_us,
        median_p95_us, median_p99_us, median_max_us,
        median_ns_per_expanded_pair -AutoSize
Write-Host "Artifacts: $OutputRoot"
