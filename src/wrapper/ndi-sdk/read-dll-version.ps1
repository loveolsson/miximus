param([Parameter(Mandatory = $true)][string]$LibraryPath)

$ErrorActionPreference = 'Stop'
try {
    $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($LibraryPath)
    if ([string]::IsNullOrWhiteSpace($version.ProductVersion)) {
        throw "No product version resource in $LibraryPath"
    }
    '{0}.{1}.{2}.{3}' -f $version.ProductMajorPart, $version.ProductMinorPart,
        $version.ProductBuildPart, $version.ProductPrivatePart
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
