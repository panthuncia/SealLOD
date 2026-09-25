$ErrorActionPreference = "Stop"
$TimeoutSeconds = 75
$SourceFiles = $args
if ($SourceFiles.Count -eq 0) {
	Write-Error "usage: validate-sysml.ps1 <file.sysml> [file.kerml ...]"
	exit 2
}
$scriptDirectory = [System.IO.Path]::GetDirectoryName($PSCommandPath)
$validator = Join-Path $scriptDirectory "validate-sysml.mjs"
$arguments = @($validator) + $SourceFiles

$validatorProcess = Start-Process node `
	-ArgumentList $arguments `
	-WorkingDirectory (Get-Location).Path `
	-NoNewWindow `
	-PassThru

if (-not $validatorProcess.WaitForExit($TimeoutSeconds * 1000)) {
	& taskkill.exe /PID $validatorProcess.Id /T /F | Out-Host
	Write-Error "SysML validation exceeded the external $TimeoutSeconds-second timeout and was terminated."
	exit 124
}

$validatorProcess.WaitForExit()
exit $validatorProcess.ExitCode
