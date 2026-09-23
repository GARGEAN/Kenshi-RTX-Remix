param(
  [string]$RuntimePath = (Join-Path $PSScriptRoot '..\external\dlss_fg_runtime\nvngx_dlssg.dll'),
  [switch]$VerifyOnly
)

$ErrorActionPreference = 'Stop'

# DLSS 310.6.0 runtime; DLFG headers still come from the separate Packman SDK.
$version = '310.6.0'
$url = 'https://raw.githubusercontent.com/NVIDIA/DLSS/d1bef2006b41eefd9d44b0a05f123993f3acbf3c/lib/Windows_x86_64/rel/nvngx_dlssg.dll'
$expectedHash = '17e00fc66d32f348fa16d387d635ac1b994aa7317b3e6d968867d1ee77af3564'
$RuntimePath = [IO.Path]::GetFullPath($RuntimePath)

if (Test-Path -LiteralPath $RuntimePath -PathType Leaf) {
  if ((Get-FileHash -LiteralPath $RuntimePath -Algorithm SHA256).Hash -eq $expectedHash) {
    Write-Host "[FG] Verified DLSS Frame Generation $version`: $RuntimePath"
    return
  }
}

if ($VerifyOnly) {
  throw "Missing or incorrect DLSS Frame Generation $version runtime: $RuntimePath. Run Build-X64ReleaseNow.ps1 without -NoDepsFetch to fetch and install it."
}

New-Item -ItemType Directory -Path (Split-Path -Parent $RuntimePath) -Force | Out-Null
$downloadPath = $RuntimePath + '.' + [guid]::NewGuid().ToString('N') + '.download'
try {
  Write-Host "[FG] Downloading DLSS Frame Generation $version from NVIDIA"
  Invoke-WebRequest -Uri $url -OutFile $downloadPath -UseBasicParsing -TimeoutSec 120
  if ((Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash -ne $expectedHash) {
    throw "DLSS Frame Generation download failed SHA-256 verification: $url"
  }
  Move-Item -LiteralPath $downloadPath -Destination $RuntimePath -Force
  Write-Host "[FG] Installed verified runtime: $RuntimePath"
} finally {
  if (Test-Path -LiteralPath $downloadPath) {
    Remove-Item -LiteralPath $downloadPath -Force
  }
}
