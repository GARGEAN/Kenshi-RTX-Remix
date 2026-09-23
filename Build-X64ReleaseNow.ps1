param(
  [switch]$NoClean,
  [switch]$NoInstall,
  [switch]$NoDepsFetch,
  [int]$DepsTimeoutSeconds = 900
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = if ($PSScriptRoot) { [IO.Path]::GetFullPath($PSScriptRoot) } else { [IO.Path]::GetFullPath((Get-Location).Path) }
$BuildDir = Join-Path $Root '_Comp64Release'
$OutputDir = Join-Path $Root '_output\x64'

function Write-Step([string]$Message) {
  Write-Host "[build] $Message" -ForegroundColor Cyan
}

function Write-Ok([string]$Message) {
  Write-Host "[build] $Message" -ForegroundColor Green
}

function Write-Warn([string]$Message) {
  Write-Host "[build] WARNING: $Message" -ForegroundColor Yellow
}

function Quote-Arg([string]$Arg) {
  if ($null -eq $Arg) { return '""' }
  if ($Arg.Length -eq 0) { return '""' }
  if ($Arg -notmatch '[\s"]') { return $Arg }
  $escaped = $Arg -replace '([\\]*)"', '$1$1\"'
  $escaped = $escaped -replace '([\\]+)$', '$1$1'
  return '"' + $escaped + '"'
}

function Get-NativeCommandLine([string]$FilePath, [string[]]$Arguments) {
  $parts = New-Object System.Collections.Generic.List[string]
  $parts.Add((Quote-Arg $FilePath))
  foreach ($arg in @($Arguments)) { $parts.Add((Quote-Arg ([string]$arg))) }
  return ($parts -join ' ')
}

function Invoke-NativeLive {
  param(
    [Parameter(Mandatory)][string]$FilePath,
    [Parameter(Mandatory)][string[]]$Arguments,
    [string]$WorkingDirectory = $Root,
    [string]$FailureMessage = 'Native command failed',
    [switch]$AllowMesonSetupFailIfBuildNinjaExists
  )

  if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
    throw "Tool not found: $FilePath"
  }
  if (-not (Test-Path -LiteralPath $WorkingDirectory -PathType Container)) {
    throw "Working directory does not exist: $WorkingDirectory"
  }

  $cmdLine = Get-NativeCommandLine -FilePath $FilePath -Arguments $Arguments
  Write-Host "[build] cmd> $cmdLine" -ForegroundColor DarkGray
  Push-Location $WorkingDirectory
  try {
    & $FilePath @Arguments
    $exitCode = if ($null -eq $global:LASTEXITCODE) { 0 } else { [int]$global:LASTEXITCODE }
  } finally {
    Pop-Location
  }

  if ($exitCode -ne 0) {
    $buildNinja = Join-Path $BuildDir 'build.ninja'
    if ($AllowMesonSetupFailIfBuildNinjaExists -and (Test-Path -LiteralPath $buildNinja -PathType Leaf)) {
      Write-Warn "Meson setup returned $exitCode after generating build.ninja; continuing directly with Ninja."
      return $exitCode
    }
    throw "$FailureMessage (exit code $exitCode)"
  }
  return $exitCode
}

function Invoke-NativeTimeout {
  param(
    [Parameter(Mandatory)][string]$FilePath,
    [Parameter(Mandatory)][string[]]$Arguments,
    [string]$WorkingDirectory = $Root,
    [int]$TimeoutSeconds = 900,
    [string]$FailureMessage = 'Native command failed'
  )
  if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) { throw "Tool not found: $FilePath" }
  $stdout = Join-Path $env:TEMP ('dxvk-build-stdout-{0}.log' -f ([guid]::NewGuid().ToString('N')))
  $stderr = Join-Path $env:TEMP ('dxvk-build-stderr-{0}.log' -f ([guid]::NewGuid().ToString('N')))
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  if ([IO.Path]::GetExtension($FilePath) -match '(?i)^\.(bat|cmd)$') {
    $psi.FileName = $env:ComSpec
    $inner = Get-NativeCommandLine -FilePath $FilePath -Arguments $Arguments
    $psi.Arguments = '/d /s /c ' + (Quote-Arg $inner)
  } else {
    $psi.FileName = $FilePath
    $psi.Arguments = (($Arguments | ForEach-Object { Quote-Arg ([string]$_) }) -join ' ')
  }
  $psi.WorkingDirectory = $WorkingDirectory
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = New-Object System.Diagnostics.Process
  $p.StartInfo = $psi
  Write-Host ('[build] cmd> {0}' -f (Get-NativeCommandLine -FilePath $FilePath -Arguments $Arguments)) -ForegroundColor DarkGray
  [void]$p.Start()
  if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
    try { $p.Kill() } catch {}
    throw "$FailureMessage timed out after $TimeoutSeconds seconds"
  }
  $out = $p.StandardOutput.ReadToEnd()
  $err = $p.StandardError.ReadToEnd()
  if ($out) { Write-Host $out }
  if ($err) { Write-Host $err -ForegroundColor DarkYellow }
  if ($p.ExitCode -ne 0) {
    throw "$FailureMessage (exit code $($p.ExitCode))"
  }
}

function Repair-PathForBuild {
  if ([string]::IsNullOrWhiteSpace($env:Path)) { return }
  $hasBundledPythonTools = Test-Path -LiteralPath (Join-Path $Root '.build_deps\python\bin') -PathType Container
  $kept = New-Object System.Collections.Generic.List[string]
  $seen = @{}
  foreach ($raw in ($env:Path -split ';')) {
    $entry = ([string]$raw).Trim()
    if ([string]::IsNullOrWhiteSpace($entry)) { continue }
    if ($entry -match '"') { continue }
    if ($entry -match '(?i)vcvars(all)?\.bat') { continue }
    try { $full = [IO.Path]::GetFullPath($entry).TrimEnd([char[]]@('\','/')) } catch { continue }
    if ($hasBundledPythonTools -and $full -match '(?i)\\AppData\\(?:Local|Roaming)\\.*\\Python(?:\\|$)') { continue }
    if (-not (Test-Path -LiteralPath $full -PathType Container)) { continue }
    $key = $full.ToLowerInvariant()
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true
    $kept.Add($full)
  }
  $env:Path = ($kept -join ';')
}

function Clear-BadBuildFlags {
  foreach ($name in @(
    'CL','_CL_','LINK','_LINK_',
    'CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS',
    'CC','CXX','LD','AR','RC',
    'CMAKE_C_FLAGS','CMAKE_CXX_FLAGS','CMAKE_EXE_LINKER_FLAGS','CMAKE_SHARED_LINKER_FLAGS'
  )) {
    try { Remove-Item -Path "ENV:\$name" -Force -ErrorAction SilentlyContinue } catch {}
  }
}

function Find-VSInstall {
  $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
  $candidates = @()
  $cmd = Get-Command 'vswhere.exe' -ErrorAction SilentlyContinue
  if ($cmd) { $candidates += $cmd.Path }
  if ($pf86) { $candidates += (Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe') }
  $vswhere = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
  if (-not $vswhere) { throw 'vswhere.exe was not found. Install Visual Studio 2022 C++ workload.' }
  $args = @('-latest','-version','[16.0,18.0)','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath')
  $path = (& $vswhere @args 2>$null | Select-Object -First 1)
  if ([string]::IsNullOrWhiteSpace([string]$path)) { throw 'Visual Studio 2022 with MSVC x86/x64 tools was not found.' }
  return ([string]$path).Trim()
}

function Import-VSEnvironment([string]$VsPath) {
  $vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvarsall.bat'
  if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) { throw "vcvarsall.bat not found: $vcvars" }
  Repair-PathForBuild
  Clear-BadBuildFlags
  Write-Step 'Loading Visual Studio x64 Native Tools environment'
  $cmdLine = 'call "' + $vcvars + '" x64 >nul && set'
  $lines = & $env:ComSpec /d /s /c $cmdLine 2>&1
  $code = if ($null -eq $global:LASTEXITCODE) { 0 } else { [int]$global:LASTEXITCODE }
  if ($code -ne 0) { throw "vcvarsall.bat failed: $($lines | Out-String)" }
  foreach ($line in $lines) {
    if ($line -match '=') {
      $pair = [string]$line -split '=', 2
      if ($pair.Count -eq 2 -and -not [string]::IsNullOrWhiteSpace($pair[0])) {
        Set-Item -Force -Path "ENV:\$($pair[0])" -Value $pair[1]
      }
    }
  }
  Repair-PathForBuild
  Clear-BadBuildFlags
  $env:MESON_FORCE_BACKTRACE = '1'
  $env:PYTHONUTF8 = '1'
}

function Add-PathFront([string]$Dir) {
  if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return }
  $env:Path = "$Dir;$env:Path"
}

function Add-PathBack([string]$Dir) {
  if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { return }
  $env:Path = "$env:Path;$Dir"
}

function Select-Tools([string]$VsPath) {
  $vsNinja = Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
  $bundledNinja = Join-Path $Root '.build_deps\python\bin\ninja.exe'
  $selectedNinja = if (Test-Path -LiteralPath $bundledNinja -PathType Leaf) {
    $bundledNinja
  } elseif (Test-Path -LiteralPath $vsNinja -PathType Leaf) {
    $vsNinja
  } else {
    throw "Neither bundled nor Visual Studio ninja.exe was found."
  }
  $env:NINJA = $selectedNinja
  Add-PathFront (Split-Path -Parent $selectedNinja)

  $mesonCandidates = New-Object System.Collections.Generic.List[string]
  # The integrated build provisions a workspace-local Python/Meson runtime.
  # Prefer it so this standalone incremental builder works on the same clean
  # machine and does not depend on a user-global Python installation.
  $mesonCandidates.Add((Join-Path $Root '.build_deps\python\bin\meson.exe'))
  if ($env:APPDATA) {
    $rp = Join-Path $env:APPDATA 'Python'
    if (Test-Path $rp) {
      Get-ChildItem -Path $rp -Directory -Filter 'Python*' -ErrorAction SilentlyContinue | Sort-Object Name -Descending | ForEach-Object {
        $mesonCandidates.Add((Join-Path $_.FullName 'Scripts\meson.exe'))
      }
    }
  }
  if ($env:LOCALAPPDATA) {
    $lp = Join-Path $env:LOCALAPPDATA 'Programs\Python'
    if (Test-Path $lp) {
      Get-ChildItem -Path $lp -Directory -Filter 'Python*' -ErrorAction SilentlyContinue | Sort-Object Name -Descending | ForEach-Object {
        $mesonCandidates.Add((Join-Path $_.FullName 'Scripts\meson.exe'))
      }
    }
  }
  $pathMeson = Get-Command 'meson.exe' -ErrorAction SilentlyContinue
  if ($pathMeson) { $mesonCandidates.Add($pathMeson.Path) }
  foreach ($m in $mesonCandidates) {
    if ($m -and (Test-Path -LiteralPath $m -PathType Leaf)) {
      Add-PathBack (Split-Path -Parent $m)
      Write-Ok "Meson: $m"
      Write-Ok "Ninja: $selectedNinja"
      return $m
    }
  }
  throw 'meson.exe was not found. Install it with: py -m pip install --user meson'
}

function Repair-FutureTimestamps {
  $now = Get-Date
  foreach ($rel in @('meson.build','meson_options.txt','build.bat','Build-X64ReleaseNow.ps1','src\usd-plugins\meson.build')) {
    $p = Join-Path $Root $rel
    if (Test-Path -LiteralPath $p) {
      $item = Get-Item -LiteralPath $p
      if ($item.LastWriteTime -gt $now.AddSeconds(1)) { $item.LastWriteTime = $now }
    }
  }
}

function Remove-StalePackmanPath([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) { return }
  $isGoodDir = Test-Path -LiteralPath $Path -PathType Container
  if ($isGoodDir) { return }
  Remove-Item -LiteralPath $Path -Force -Recurse
}

function Ensure-Deps([switch]$SkipFetch) {
  if ($SkipFetch) { return }
  $topNeeded = @(
    'external\glslangvalidator\glslangValidator.exe',
    'external\slang\slangc.exe',
    'external\spirv_tools\spirv-val.exe',
    'external\reflex\lib\NvLowLatencyVk.lib',
    'external\nv_usd_Release\lib\usd.lib'
  )
  $missingTop = @($topNeeded | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Root $_)) })
  if ($missingTop.Count -gt 0) {
    $update = Join-Path $Root 'scripts-common\update-deps.cmd'
    $xml = Join-Path $Root 'packman-external.xml'
    if (-not (Test-Path -LiteralPath $update -PathType Leaf)) { throw "Missing dependency fetch script: $update" }
    Write-Step 'Fetching top-level dependencies with timeout'
    Invoke-NativeTimeout -FilePath $update -Arguments @($xml, 'update-deps.log') -WorkingDirectory $Root -TimeoutSeconds $DepsTimeoutSeconds -FailureMessage 'Top-level Packman dependency fetch failed'
  } else {
    Write-Ok 'Top-level dependencies present'
  }

  $usdRelease = Join-Path $Root 'src\usd-plugins\_external\nv_usd\release'
  Remove-StalePackmanPath $usdRelease
  $usdNeeded = @(
    'src\usd-plugins\_external\nv_usd\release\lib\usd.lib',
    'src\usd-plugins\_external\python\python.exe'
  )
  $missingUsd = @($usdNeeded | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Root $_)) })
  if ($missingUsd.Count -gt 0) {
    $packman = Join-Path $Root 'scripts-common\packman\packman.cmd'
    $xml = Join-Path $Root 'src\usd-plugins\packman-external.xml'
    if (-not (Test-Path -LiteralPath $packman -PathType Leaf)) { throw "Missing Packman: $packman" }
    Write-Step 'Fetching USD plugin dependencies with timeout'
    Invoke-NativeTimeout -FilePath $packman -Arguments @('pull', $xml, '-p', 'windows-x86_64') -WorkingDirectory (Join-Path $Root 'src\usd-plugins') -TimeoutSeconds $DepsTimeoutSeconds -FailureMessage 'USD plugin Packman dependency fetch failed'
  } else {
    Write-Ok 'USD plugin dependencies present'
  }
}

if (-not (Test-Path -LiteralPath (Join-Path $Root 'meson.build') -PathType Leaf)) {
  throw "meson.build was not found in $Root"
}

$vsPath = Find-VSInstall
Write-Ok "Visual Studio: $vsPath"
Import-VSEnvironment $vsPath
$meson = Select-Tools $vsPath

Repair-FutureTimestamps
Ensure-Deps -SkipFetch:$NoDepsFetch

if (-not $NoClean -and (Test-Path -LiteralPath $BuildDir)) {
  Write-Step "Removing stale build directory: $BuildDir"
  Remove-Item -LiteralPath $BuildDir -Recurse -Force
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

$mesonArgs = @(
  'setup',
  '--buildtype=release',
  '--backend=ninja',
  '-Db_vscrt=md',
  '-Denable_dxgi=true',
  '-Denable_d3d11=true',
  '-Denable_tests=false',
  '-Denable_tracy=false',
  '-Dskip_packman_fetch=true',
  $BuildDir,
  $Root
)

Write-Step 'Configuring Meson x64 release'
Invoke-NativeLive -FilePath $meson -Arguments $mesonArgs -WorkingDirectory $Root -FailureMessage 'Meson setup failed' -AllowMesonSetupFailIfBuildNinjaExists | Out-Null

$buildNinja = Join-Path $BuildDir 'build.ninja'
if (-not (Test-Path -LiteralPath $buildNinja -PathType Leaf)) {
  throw "Meson did not create build.ninja, so Ninja cannot compile: $buildNinja"
}

Write-Step 'Compiling with Ninja now'
Invoke-NativeLive -FilePath $env:NINJA -Arguments @('-C', $BuildDir, '-v') -WorkingDirectory $Root -FailureMessage 'Ninja compile failed' | Out-Null

if (-not $NoInstall) {
  Write-Step 'Installing output files'
  $installOk = $true
  try {
    Invoke-NativeLive -FilePath $meson -Arguments @('install', '-C', $BuildDir, '--no-rebuild', '--tags', 'output') -WorkingDirectory $Root -FailureMessage 'Meson install failed' | Out-Null
  } catch {
    $installOk = $false
    Write-Warn $_.Exception.Message
  }
  if (-not $installOk) {
    Write-Step 'Meson install failed; copying built DLLs manually'
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    $dlls = @(Get-ChildItem -Path $BuildDir -Recurse -File -Include 'd3d11.dll','dxgi.dll' -ErrorAction SilentlyContinue)
    foreach ($dll in $dlls) {
      Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $OutputDir $dll.Name) -Force
      $pdb = [IO.Path]::ChangeExtension($dll.FullName, '.pdb')
      if (Test-Path -LiteralPath $pdb -PathType Leaf) { Copy-Item -LiteralPath $pdb -Destination (Join-Path $OutputDir ([IO.Path]::GetFileName($pdb))) -Force }
    }
    if ($dlls.Count -eq 0) { throw 'Compile finished but no d3d11.dll or dxgi.dll was found to copy.' }
  }
}

$ngxSource = Join-Path $Root 'nv-private\hdremix\bin\release'
if (Test-Path -LiteralPath $ngxSource -PathType Container) {
  Get-ChildItem -Path $ngxSource -Filter 'nvngx_*.dll' -File -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $OutputDir -Force
  }
}

Write-Ok "DONE. Output folder: $OutputDir"
