param(
    [string]$Preset = 'windows-cef-release',
    [int]$Jobs = 8,
    [switch]$SetupOnly
)

$ErrorActionPreference = 'Stop'
$workspace = Split-Path $PSScriptRoot -Parent
$presetPath = Join-Path $workspace 'CMakeUserPresets.json'
$presets = Get-Content -LiteralPath $presetPath -Raw | ConvertFrom-Json
$configurePreset = $presets.configurePresets | Where-Object name -EQ $Preset
$buildPreset = $presets.buildPresets | Where-Object name -EQ $Preset
if (-not $configurePreset -or -not $buildPreset) {
    throw "Configure and build presets named '$Preset' are required in CMakeUserPresets.json. See docs/windows-development.md."
}

$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio C++ tools not found' }
$originalPath = @($env:PATH -split ';')
Import-Module (Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=amd64 -host_arch=amd64'

# CMake Tools can lose its inferred developer environment between configure and
# build. Persist only compiler settings in the ignored local presets, so its
# Play button and plain-shell preset builds do not depend on that inference.
$compilerEnvironment = [ordered]@{}
foreach ($name in @('INCLUDE', 'LIB', 'LIBPATH')) {
    $value = [Environment]::GetEnvironmentVariable($name)
    if (-not $value) { throw "Visual Studio did not initialize $name" }
    $compilerEnvironment[$name] = $value
}
$toolPaths = @($env:PATH -split ';' | Where-Object {
    $_ -and (($_ -notin $originalPath) -or $_.StartsWith($vsInstall, [StringComparison]::OrdinalIgnoreCase) -or
        ($env:WindowsSdkDir -and $_.StartsWith($env:WindowsSdkDir, [StringComparison]::OrdinalIgnoreCase)))
} | Select-Object -Unique)
$compilerEnvironment['PATH'] = ($toolPaths -join ';') + ';$penv{PATH}'
foreach ($entry in @($configurePreset, $buildPreset)) {
    if (-not $entry.environment) {
        $entry | Add-Member -NotePropertyName environment -NotePropertyValue ([pscustomobject]@{}) -Force
    }
    foreach ($name in $compilerEnvironment.Keys) {
        $entry.environment | Add-Member -NotePropertyName $name -NotePropertyValue $compilerEnvironment[$name] -Force
    }
}
$updated = ($presets | ConvertTo-Json -Depth 32) + [Environment]::NewLine
if ([IO.File]::ReadAllText($presetPath) -ne $updated) {
    [IO.File]::WriteAllText($presetPath, $updated, [Text.UTF8Encoding]::new($false))
}
if ($SetupOnly) { return }

Push-Location $workspace
try {
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    cmake --build --preset $Preset --parallel $Jobs
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
