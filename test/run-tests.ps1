# Run the obs-optical-flow test harnesses against an installed OBS Studio.
#
# Usage (from the unpacked tests-windows-x64 folder):
#   powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 [-ObsDir <path>] [-Adapter <n>] [-Backends d3d11,opengl]
#
# -Adapter picks the GPU for D3D11. On hybrid laptops adapter 0 is usually
# the integrated GPU; the log line "Loading up D3D11 on adapter ..." shows
# which one was used. Run once per adapter to test both GPUs.
param(
	[string]$ObsDir = "C:\Program Files\obs-studio",
	[int]$Adapter = 0,
	[string[]]$Backends = @("d3d11", "opengl")
)

$ErrorActionPreference = "Stop"
$bin = Join-Path $ObsDir "bin\64bit"
if (-not (Test-Path (Join-Path $bin "obs.dll"))) {
	Write-Error "obs.dll not found in $bin - pass -ObsDir <your OBS Studio folder>"
}
$env:PATH = "$bin;$env:PATH"   # obs.dll + libobs-d3d11/opengl graphics modules
Set-Location $PSScriptRoot

$tests = @("effect-test", "flow-test", "debug-test", "perf-test")
$results = @()
foreach ($b in $Backends) {
	foreach ($t in $tests) {
		Write-Host "`n===== $t [$b, adapter $Adapter] =====" -ForegroundColor Cyan
		& ".\$t.exe" --backend $b --adapter $Adapter --effect "data\effects\optical-flow.effect"
		$code = $LASTEXITCODE
		$results += [pscustomobject]@{ Test = $t; Backend = $b; Result = $(if ($code -eq 0) { "PASS" } else { "FAIL ($code)" }) }
	}
}

Write-Host "`n===== Summary =====" -ForegroundColor Cyan
$results | Format-Table -AutoSize
if ($results | Where-Object { $_.Result -ne "PASS" }) { exit 1 }
