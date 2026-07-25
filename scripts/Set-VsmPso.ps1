param(
    [string] $PipeName = "BasicRenderer.Optimize.VSM",
    [ValidateSet("list", "recompile", "activate", "reload-workgraph")]
    [string] $Action = "list",
    [ValidateSet("all", "block-histogram", "block-emit", "culling", "hardware-raster", "software-raster")]
    [string] $Target = "all",
    [string] $Label = "vsm-candidate",
    [hashtable] $Defines = @{},
    [UInt64] $Generation = 1
)

$ErrorActionPreference = "Stop"

function Send-Request([hashtable] $Request) {
    $client = [System.IO.Pipes.NamedPipeClientStream]::new(".", $PipeName, [System.IO.Pipes.PipeDirection]::InOut)
    $client.Connect(10000)
    try {
        $writer = [System.IO.StreamWriter]::new($client)
        $writer.AutoFlush = $true
        $reader = [System.IO.StreamReader]::new($client)
        $writer.WriteLine(($Request | ConvertTo-Json -Compress -Depth 8))
        return ($reader.ReadLine() | ConvertFrom-Json)
    }
    finally {
        $client.Dispose()
    }
}

if ($Action -eq "reload-workgraph") {
    $response = Send-Request @{ request_id = [Environment]::TickCount64; command = "clod.workgraph.reload" }
    if (-not $response.ok) { throw $response.error }
    $response
    return
}

$aliases = @{
    "block-histogram" = "VirtualShadowBlockHistogram"
    "block-emit" = "VirtualShadowBlockEmit"
    "culling" = "HierarchicalCulling|WorkGraph"
    "hardware-raster" = "CompileClusterLODVirtualShadowRasterPSO|RasterizeClusters|HardwareRaster"
    "software-raster" = "SoftwareRasterizeClusters|SoftwareRaster"
}
$list = Send-Request @{ request_id = [Environment]::TickCount64; command = "pso.list" }
$pattern = if ($Target -eq "all") { "VirtualShadowBlock|HierarchicalCulling|WorkGraph|CompileClusterLODVirtualShadowRasterPSO|RasterizeClusters|SoftwareRaster" } else { $aliases[$Target] }
$pipelines = @($list.pipelines | Where-Object { $_.name -match $pattern })

if ($Action -eq "list") {
    $pipelines | Select-Object id, name, active_generation, generations
    return
}
if ($pipelines.Count -eq 0) { throw "No pipeline matched target '$Target' ($pattern)." }

$results = foreach ($pipeline in $pipelines) {
    $request = @{ request_id = [Environment]::TickCount64; pipeline_id = $pipeline.id }
    if ($Action -eq "recompile") {
        $request.command = "pso.recompile"
        $request.label = $Label
        $request.defines = $Defines
    } else {
        $request.command = "pso.activate"
        $request.generation = $Generation
    }
    $response = Send-Request $request
    if (-not $response.ok) { throw "$($pipeline.id): $($response.error)" }
    do {
        Start-Sleep -Milliseconds 100
        $status = Send-Request @{ request_id = [Environment]::TickCount64; command = "job.status"; job_id = $response.job_id }
    } while ($status.job.state -notin @("published", "failed"))
    if ($status.job.state -eq "failed") { throw "$($pipeline.id): $($status.job.error)" }
    [pscustomobject]@{ Pipeline = $pipeline.id; Name = $pipeline.name; Generation = $status.job.generation; State = $status.job.state }
}
$results
