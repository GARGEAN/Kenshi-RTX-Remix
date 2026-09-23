<#
  diagnose_deps.ps1
  ------------------------------------------------------------------
  Meson setup is failing with exit code 1 BEFORE any compilation. The cause is
  the dependency-fetch step in meson.build (line ~46):

      packman_out = run_command(update-deps.cmd 'packman-external.xml' ... check: true)

  With check:true, if packman cannot download the external SDKs (aftermath,
  USD, reflex, rtxio, glslang, slang, ...), meson setup aborts with exit 1 -
  exactly the error you are seeing. The external\ folders in the repo are
  currently EMPTY, which confirms packman has never fetched successfully.

  Meson hides packman's real error. This script runs packman DIRECTLY so you
  can see WHY it fails (network, proxy, TLS, or the packman Python bootstrap),
  and reports which external\ dependencies are present vs missing.
#>
param([string]$RepoRoot = "")
$ErrorActionPreference = "Continue"
function Log([string]$m) { Write-Host ("[deps] {0}" -f $m) }

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
  if (-not [string]::IsNullOrWhiteSpace($PSScriptRoot)) { $RepoRoot = $PSScriptRoot }
  else { $RepoRoot = (Get-Location).Path }
}
if (-not (Test-Path (Join-Path $RepoRoot "meson.build"))) {
  $sub = Join-Path $RepoRoot "dxvk-remix-DX11"
  if (Test-Path (Join-Path $sub "meson.build")) { $RepoRoot = $sub }
}
Log "Repo root: $RepoRoot"

# 1) Report current state of external\ dependencies.
$expected = @(
  "external\aftermath", "external\reflex", "external\rtxio",
  "external\nv_usd_release", "external\nv_usd_debug",
  "external\glslangvalidator", "external\slang", "external\spirv_tools",
  "external\fonts", "external\omni_core_materials", "external\nv_xxd"
)
Log "----- external\ dependency state -----"
$missing = @()
foreach ($d in $expected) {
  $full = Join-Path $RepoRoot $d
  $present = (Test-Path $full) -and ((Get-ChildItem $full -Force -ErrorAction SilentlyContinue | Measure-Object).Count -gt 0)
  if ($present) { Log ("  OK     {0}" -f $d) }
  else { Log ("  MISSING {0}" -f $d); $missing += $d }
}
if ($missing.Count -eq 0) {
  Log "All external dependencies are present. If meson setup still fails, the error is elsewhere - re-run setup and share the lines ABOVE the PowerShell throw."
} else {
  Log ("{0} of {1} dependencies are MISSING - packman has not fetched. Running it directly below to surface the real error." -f $missing.Count, $expected.Count)
}

# 2) Run packman directly so its real error is visible (meson swallows it).
$updateDeps = Join-Path $RepoRoot "scripts-common\update-deps.cmd"
$packmanXml = Join-Path $RepoRoot "packman-external.xml"
if (-not (Test-Path $updateDeps)) { Log "ERROR: $updateDeps not found."; exit 1 }
if (-not (Test-Path $packmanXml)) { Log "ERROR: $packmanXml not found."; exit 1 }

Log "----- running packman directly (this is what meson runs) -----"
Log "cmd: $updateDeps `"$packmanXml`" deps-diagnostic.log"
& cmd /c "`"$updateDeps`" `"$packmanXml`" deps-diagnostic.log" 2>&1 | ForEach-Object { Write-Host ("  pm> {0}" -f $_) }
$pmExit = $LASTEXITCODE
Log ("packman exit code: {0}" -f $pmExit)

Log "----- interpretation -----"
if ($pmExit -eq 0 -and $missing.Count -gt 0) {
  Log "packman reported success but folders are still empty - check PM_PACKAGES_ROOT and whether the linkPath junctions were created (packman uses directory junctions; a non-NTFS drive or restricted permissions can break these)."
} elseif ($pmExit -ne 0) {
  Log "packman FAILED. Common causes, in order:"
  Log "  1. Network/proxy/firewall blocking packman.nvidia.com or the package CDN."
  Log "     Test: open https://packman.nvidia.com in a browser on this machine."
  Log "  2. First-run bootstrap: packman downloads its OWN Python runtime; if that"
  Log "     download is blocked, PM_PYTHON is never set and it exits immediately."
  Log "  3. TLS/certificate interception on a corporate network."
  Log "  4. PM_PACKAGES_ROOT pointing at an unwritable or non-NTFS location."
  Log "The 'pm>' lines above contain packman's actual message - that is the real error."
}

Log "Done. Once external\ is fully populated, 'meson setup' will pass and the .trex will contain its runtime DLLs (NRD/NRC/USD/rtxio)."
