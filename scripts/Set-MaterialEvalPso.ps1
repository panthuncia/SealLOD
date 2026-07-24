param(
    [string] $PipeName = "BasicRenderer.Optimize.MaterialEval",
    [ValidateSet("recompile", "activate")]
    [string] $Action = "recompile",
    [string] $Label = "material-candidate",
    [hashtable] $Defines = @{},
    [UInt64] $Generation = 1
)

$ErrorActionPreference = "Stop"

function Send-SamplingRequest([hashtable] $Request) {
    $client = [System.IO.Pipes.NamedPipeClientStream]::new(
        ".", $PipeName, [System.IO.Pipes.PipeDirection]::InOut)
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

$list = Send-SamplingRequest @{
    request_id = [Environment]::TickCount64
    command = "pso.list"
}
$pipelines = @($list.pipelines | Where-Object {
    $_.name -eq "VisUtil_EvaluateMaterialGroupPSO" -or
    $_.name -eq "VisUtil_BuildEvaluateIndirectArgsPSO"
})

$results = foreach ($pipeline in $pipelines) {
    $request = @{
        request_id = [Environment]::TickCount64
        pipeline_id = $pipeline.id
    }
    if ($Action -eq "recompile") {
        $request.command = "pso.recompile"
        $request.label = $Label
        $request.defines = $Defines
    }
    else {
        $request.command = "pso.activate"
        $request.generation = $Generation
    }
    $response = Send-SamplingRequest $request
    if (-not $response.ok) {
        throw "$($pipeline.id): $($response.error)"
    }
    do {
        Start-Sleep -Milliseconds 100
        $status = Send-SamplingRequest @{
            request_id = [Environment]::TickCount64
            command = "job.status"
            job_id = $response.job_id
        }
    } while ($status.job.state -notin @("published", "failed"))
    if ($status.job.state -eq "failed") {
        throw "$($pipeline.id): $($status.job.error)"
    }
    [pscustomobject] @{
        Pipeline = $pipeline.id
        Generation = $status.job.generation
        State = $status.job.state
    }
}

$results
