param(
    [string]$SourceDir = "_output",
    [string]$StagingRoot = "_release",
    [string]$Version,
    [string]$PackageName,
    [switch]$SkipZip
)

$ErrorActionPreference = "Stop"

function Copy-IfExists {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,

        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    if (Test-Path $Source) {
        Copy-Item -Path $Source -Destination $Destination -Force
    }
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$resolvedSourceDir = Join-Path $repoRoot $SourceDir

if (-not (Test-Path $resolvedSourceDir)) {
    throw "Source directory '$resolvedSourceDir' does not exist. Build the runtime so _output exists first."
}

if (-not $Version -or [string]::IsNullOrWhiteSpace($Version)) {
    $releaseFile = Join-Path $repoRoot "RELEASE"
    if (Test-Path $releaseFile) {
        $Version = Get-Content $releaseFile |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ -and -not $_.StartsWith('#') } |
            Select-Object -First 1
    }

    if (-not $Version) {
        $Version = "dev"
    }
}

if (-not $PackageName -or [string]::IsNullOrWhiteSpace($PackageName)) {
    $PackageName = "dxvk-remix-dx11-$Version"
}

$resolvedStagingRoot = Join-Path $repoRoot $StagingRoot
$packageDir = Join-Path $resolvedStagingRoot $PackageName
$zipPath = Join-Path $resolvedStagingRoot ($PackageName + ".zip")

if (Test-Path $packageDir) {
    Remove-Item -Path $packageDir -Recurse -Force
}

if (Test-Path $zipPath) {
    Remove-Item -Path $zipPath -Force
}

New-Item -ItemType Directory -Path $packageDir -Force | Out-Null
Copy-Item -Path (Join-Path $resolvedSourceDir "*") -Destination $packageDir -Recurse -Force

Copy-IfExists -Source (Join-Path $repoRoot "dxvk.conf") -Destination (Join-Path $packageDir "dxvk.conf")
Copy-IfExists -Source (Join-Path $repoRoot "rtx.conf") -Destination (Join-Path $packageDir "rtx.conf")

$topLevelFiles = Get-ChildItem -Path $packageDir -File | Sort-Object Name
$topLevelDirs = Get-ChildItem -Path $packageDir -Directory | Sort-Object Name

Write-Host "Packaged release layout: $packageDir" -ForegroundColor Green
Write-Host "Top-level files:" -ForegroundColor Cyan
$topLevelFiles | ForEach-Object { Write-Host "  $($_.Name)" }

Write-Host "Top-level directories:" -ForegroundColor Cyan
$topLevelDirs | ForEach-Object { Write-Host "  $($_.Name)" }

if (-not $SkipZip) {
    Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host "Created release archive: $zipPath" -ForegroundColor Green
}