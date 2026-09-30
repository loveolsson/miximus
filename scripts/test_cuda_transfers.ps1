param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '../build'),
    [ValidateRange(1, 100)][int]$Repeats = 3
)

$ErrorActionPreference = 'Stop'
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$binary = Join-Path $BuildDir 'src/gpu/gpu_transfer_vulkan_test.exe'
if (-not (Test-Path -LiteralPath $binary)) {
    throw "Missing $binary; configure with BUILD_TESTING=ON and MIXIMUS_ENABLE_CUDA=ON, then build."
}
$runDir = Join-Path $BuildDir ("integration-tests/cuda-transfers-{0}-{1}" -f [DateTime]::UtcNow.ToString('yyyyMMddTHHmmss'), $PID)
New-Item -ItemType Directory -Path $runDir | Out-Null
$log = Join-Path $runDir 'tests.log'
Write-Output 'Requiring CUDA: fallback to Vulkan staging is forbidden.'
# Capture directly so Windows PowerShell does not turn native stderr into terminating errors.
$process = Start-Process -FilePath $binary -ArgumentList '--use-cuda', '--log-debug', "--gtest_repeat=$Repeats" `
    -RedirectStandardOutput $log -RedirectStandardError (Join-Path $runDir 'stderr.log') `
    -WindowStyle Hidden -Wait -PassThru
$output = [IO.File]::ReadAllText($log) + [IO.File]::ReadAllText((Join-Path $runDir 'stderr.log'))
if ($process.ExitCode -ne 0) {
    throw "CUDA tests failed with exit code $($process.ExitCode); see $runDir"
}
foreach ($direction in 'upload', 'readback') {
    if ($output -notmatch "Transfer completed: backend=cuda-vulkan-direct direction=$direction") {
        throw "No completed CUDA $direction found; see $runDir"
    }
}
if ($output -match 'backend=vulkan-staging') {
    throw "Unexpected Vulkan fallback; see $runDir"
}
Write-Output "PASS: actual CUDA uploads and readbacks completed, pixel/lease tests passed, and no fallback occurred. Logs: $runDir"
