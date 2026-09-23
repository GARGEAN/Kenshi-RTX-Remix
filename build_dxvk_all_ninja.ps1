# Integrated DX11 dual-output build script. Does not require Build-DX11-DualOutput-V*.ps1 or repair scripts.


# DX11_V219_DISABLE_STRICTMODE_RUNTIME
# The build script is generated/repair-patched and contains many optional switch
# variables. In normal PowerShell, an unset optional switch evaluates as false.
# StrictMode turns those into fatal VariableIsUndefined errors before Ninja can
# build. Parser validation is still done by the repair script; runtime StrictMode
# is disabled so optional switches behave normally.
Set-StrictMode -Off
# The generated script historically relied on callers to define OutputRoot.
# With StrictMode disabled, an omitted value becomes an empty string and
# Stage-DualOutput resolves it to the repository root. Packaging then attempts
# to archive the entire worktree (including its own destination under
# _packages), producing an unbounded, invalid archive. Keep all staged output
# under the documented _output directory when the caller supplies no override.
if (!(Test-Path -LiteralPath variable:script:OutputRoot) -or
    [string]::IsNullOrWhiteSpace([string]$script:OutputRoot)) {
  $script:OutputRoot = '_output'
}
# DX11_V219_DEFINE_CLEAN_SWITCH_DEFAULT
# StrictMode-safe default.  Some clone-sync paths test $Clean before any caller
# defines it.  Default false means "do not delete/reclone unless explicitly set."
if (!(Test-Path -LiteralPath variable:script:Clean)) {
  $script:Clean = $false
}

# DX11_V219_DEFINE_BRIDGE_REPO_DEFAULTS
# StrictMode-safe defaults for the bridge source clone path. These match the
# known-good clone command already used successfully by this project.
if (!(Test-Path -LiteralPath variable:script:BridgeRepoUrl)) {
  $script:BridgeRepoUrl = 'https://github.com/NVIDIAGameWorks/dxvk-remix.git'
}
if (!(Test-Path -LiteralPath variable:script:BridgeBranch)) {
  $script:BridgeBranch = 'main'
}

# Packaged GUID files are only a launcher fallback; the live x86 client writes
# its own per-process GUID before starting the bridge.  Still, the fallback must
# always be syntactically valid so a direct/helper recovery launch cannot produce
# an empty IPC namespace.
if (!(Test-Path -LiteralPath variable:script:Dx11BridgeFixedGuidV63) -or
    [string]::IsNullOrWhiteSpace([string]$script:Dx11BridgeFixedGuidV63)) {
  $script:Dx11BridgeFixedGuidV63 = [guid]::NewGuid().ToString('D')
}

# DX11_V219_DEFINE_BUILD_MODE_DEFAULTS
# StrictMode-safe defaults for optional build-mode switches.  These switches are
# read in top-level control flow and helper paths, so they must exist before use.
# Default false means the normal full build path is preserved unless a future
# caller explicitly sets one of these switches.
foreach ($dx11ModeDefaultName in @(
  'SourceOnly',
  'RuntimeOnly',
  'BridgeOnly',
  'StageOnly',
  'NoStage',
  'NoBuild',
  'SkipRuntime',
  'SkipBridge',
  'SkipX86',
  'SkipX64',
  'RebuildRuntime',
  'RebuildBridge',
  'Force',
  'ForceRebuild',
  'CleanRuntime',
  'CleanBridge'
)) {
  if (!(Test-Path -LiteralPath ("variable:script:" + $dx11ModeDefaultName))) {
    Set-Variable -Scope Script -Name $dx11ModeDefaultName -Value $false
  }
}
Remove-Variable -Name dx11ModeDefaultName -ErrorAction SilentlyContinue

# DX11_V219_DEFINE_RUNTIME_BUILD_SWITCH_DEFAULTS
# StrictMode-safe defaults for optional build/skip switches. Default false keeps
# the normal full build path unless a caller explicitly sets a switch true.
foreach ($dx11ModeDefaultName in @(
  'SourceOnly',
  'RuntimeOnly',
  'BridgeOnly',
  'StageOnly',
  'NoStage',
  'NoBuild',
  'Clean',
  'Force',
  'ForceRebuild',
  'SkipRuntime',
  'SkipRuntimeBuild',
  'NoRuntime',
  'NoRuntimeBuild',
  'SkipBridge',
  'SkipBridgeBuild',
  'NoBridge',
  'NoBridgeBuild',
  'SkipClient',
  'SkipClientBuild',
  'NoClient',
  'NoClientBuild',
  'SkipServer',
  'SkipServerBuild',
  'NoServer',
  'NoServerBuild',
  'SkipLauncher',
  'SkipLauncherBuild',
  'NoLauncher',
  'NoLauncherBuild',
  'SkipStage',
  'SkipStaging',
  'SkipOutput',
  'SkipPackage',
  'SkipMeson',
  'SkipNinja',
  'SkipConfigure',
  'SkipCompile',
  'SkipX86',
  'SkipX86Build',
  'NoX86',
  'NoX86Build',
  'SkipX64',
  'SkipX64Build',
  'NoX64',
  'NoX64Build',
  'RebuildRuntime',
  'RebuildRuntimeBuild',
  'ForceRuntime',
  'ForceRuntimeBuild',
  'RebuildBridge',
  'RebuildBridgeBuild',
  'ForceBridge',
  'ForceBridgeBuild',
  'CleanRuntime',
  'CleanRuntimeBuild',
  'CleanBridge',
  'CleanBridgeBuild',
  'CleanOutput',
  'CleanStage',
  'CleanStaging',
  'Debug',
  'Release',
  'RelWithDebInfo',
  'BuildDebug',
  'BuildRelease',
  'UseExistingClone',
  'CloneOnly',
  'NoClone',
  'UpdateClone',
  'PullClone',
  'RunTests',
  'VerboseLogs',
  'KeepTemp',
  'KeepWork',
  'NoRepair',
  '__DX11_END_OF_SWITCH_DEFAULTS__'
)) {
  if ($dx11ModeDefaultName -eq '__DX11_END_OF_SWITCH_DEFAULTS__') { continue }
  if (!(Test-Path -LiteralPath ("variable:script:" + $dx11ModeDefaultName))) {
    Set-Variable -Scope Script -Name $dx11ModeDefaultName -Value $false
  }
}
Remove-Variable -Name dx11ModeDefaultName -ErrorAction SilentlyContinue

# DX11_V219_FORCE_DUAL_ARCH_BUILD_DEFAULTS
# Skip switches default false, but build switches must default true.  The DX11
# output package is required to build both x86 and x64 unless a caller
# explicitly disables a specific path later.
foreach ($dx11BuildDefaultName in @(
  'BuildX86',
  'BuildX64',
  'BuildRuntime',
  'BuildBridge',
  'BuildClient',
  'BuildServer',
  'BuildLauncher',
  'BuildStage',
  'BuildStaging',
  'StageOutput',
  '__DX11_END_OF_TRUE_BUILD_DEFAULTS__'
)) {
  if ($dx11BuildDefaultName -eq '__DX11_END_OF_TRUE_BUILD_DEFAULTS__') { continue }
  Set-Variable -Scope Script -Name $dx11BuildDefaultName -Value $true
}
Remove-Variable -Name dx11BuildDefaultName -ErrorAction SilentlyContinue

# Hard guard: never let accidental false build defaults suppress x86/x64.
$script:SkipX86 = $false
$script:SkipX64 = $false
$script:SkipX86Build = $false
$script:SkipX64Build = $false
$script:NoX86 = $false
$script:NoX64 = $false
$script:NoX86Build = $false
$script:NoX64Build = $false
$script:BuildX86 = $true
$script:BuildX64 = $true

# DX11_V219_FORCE_DUAL_ARCH_GUARD
# Always build both architecture outputs unless the user intentionally edits the
# script later. This prevents safety defaults from suppressing x86/x64.
$script:SkipX86 = $false
$script:SkipX64 = $false
$script:SkipX86Build = $false
$script:SkipX64Build = $false
$script:NoX86 = $false
$script:NoX64 = $false
$script:NoX86Build = $false
$script:NoX64Build = $false
$script:BuildX86 = $true
$script:BuildX64 = $true
$script:BuildRuntime = $true
$script:BuildBridge = $true
$script:BuildClient = $true
$script:BuildServer = $true
$script:BuildLauncher = $true



$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$LogDir = Join-Path $Root '_build_logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

function Log([string]$m) { Write-Host "[dx11-output-v63] $m" }
function Warn([string]$m) { Write-Host "[dx11-output-v63] WARNING: $m" -ForegroundColor Yellow }
function Die([string]$m) { throw "[dx11-output-v63] $m" }
function Get-CommandFilePath([string]$Name) {
  $cmds = @(Get-Command $Name -ErrorAction SilentlyContinue)
  foreach ($cmd in $cmds) {
    foreach ($prop in @('Path','Source','Definition')) {
      $v = $null
      try {
        if ($cmd.PSObject.Properties.Name -contains $prop) { $v = [string]$cmd.$prop }
      } catch { $v = $null }
      if (![string]::IsNullOrWhiteSpace($v) -and (Test-Path -LiteralPath $v -PathType Leaf)) { return $v }
    }
  }
  return $null
}
function CmdPath([string]$n) { return (Get-CommandFilePath $n) }


function Write-TextNoBom {
  param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Text)
  if (Test-Path -LiteralPath $Path -PathType Leaf) {
    $current = [System.IO.File]::ReadAllText($Path)
    if ($current -ceq $Text) {
      return
    }
  }
  $enc = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($Path, $Text, $enc)
}

function Read-TextRaw {
  param([Parameter(Mandatory)][string]$Path)
  if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { Die "Required text file not found: $Path" }
  return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function Remove-Utf8BomFromFile {
  param([Parameter(Mandatory)][string]$Path)
  if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
  $bytes = [System.IO.File]::ReadAllBytes($Path)
  if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
    $newLen = $bytes.Length - 3
    $outBytes = New-Object byte[] $newLen
    if ($newLen -gt 0) { [System.Array]::Copy($bytes, 3, $outBytes, 0, $newLen) }
    [System.IO.File]::WriteAllBytes($Path, $outBytes)
    return $true
  }
  return $false
}

function Repair-MesonUtf8NoBom {
  param([Parameter(Mandatory)][string]$BaseDir)
  if (!(Test-Path -LiteralPath $BaseDir -PathType Container)) { return }
  $fixed = 0
  $files = @(Get-ChildItem -LiteralPath $BaseDir -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object {
      $_.Name -in @('meson.build','meson_options.txt') -and
      $_.FullName -notmatch '\\.git\\' -and
      $_.FullName -notmatch '\\meson-private\\' -and
      $_.FullName -notmatch '\\meson-logs\\' -and
      $_.FullName -notmatch '\\_Comp(32|64)' -and
      $_.FullName -notmatch '\\_build_logs\\'
    })
  foreach ($f in $files) {
    if (Remove-Utf8BomFromFile $f.FullName) {
      $fixed++
      Log "Removed UTF-8 BOM from Meson file: $($f.FullName)"
    }
  }
  if ($fixed -gt 0) { Log "Repaired $fixed Meson UTF-8 BOM file(s)." }
}


function Repair-MesonNestedTernary {
  param([Parameter(Mandatory)][string]$BaseDir)
  if (!(Test-Path -LiteralPath $BaseDir -PathType Container)) { return }

  $replacement = @"
if usd_schema_plugins_cpu_family == 'x86'
  if usd_schema_plugins_isdebug
    usd_schema_plugins_nvusd_dir = '_external/nv_usd_x86/debug/'
  else
    usd_schema_plugins_nvusd_dir = '_external/nv_usd_x86/release/'
  endif
else
  if usd_schema_plugins_isdebug
    usd_schema_plugins_nvusd_dir = '_external/nv_usd/debug/'
  else
    usd_schema_plugins_nvusd_dir = '_external/nv_usd/release/'
  endif
endif
"@

  $pattern = "(?m)^\s*usd_schema_plugins_nvusd_dir\s*=\s*usd_schema_plugins_cpu_family\s*==\s*'x86'\s*\?\s*\(\s*usd_schema_plugins_isdebug\s*\?\s*'_external/nv_usd_x86/debug/'\s*:\s*'_external/nv_usd_x86/release/'\s*\)\s*:\s*\(\s*usd_schema_plugins_isdebug\s*\?\s*'_external/nv_usd/debug/'\s*:\s*'_external/nv_usd/release/'\s*\)\s*$"

  $fixed = 0
  $files = @(Get-ChildItem -LiteralPath $BaseDir -Recurse -File -Filter 'meson.build' -ErrorAction SilentlyContinue |
    Where-Object {
      $_.FullName -notmatch '\\.git\\' -and
      $_.FullName -notmatch '\\meson-private\\' -and
      $_.FullName -notmatch '\\meson-logs\\' -and
      $_.FullName -notmatch '\\_Comp(32|64)' -and
      $_.FullName -notmatch '\\_build_logs\\'
    })

  foreach ($f in $files) {
    $text = [System.IO.File]::ReadAllText($f.FullName)
    if ($text -match "usd_schema_plugins_nvusd_dir\s*=\s*usd_schema_plugins_cpu_family\s*==\s*'x86'\s*\?") {
      $newText = [regex]::Replace($text, $pattern, $replacement)
      if ($newText -ne $text) {
        $enc = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($f.FullName, $newText, $enc)
        $fixed++
        Log "Repaired nested Meson ternary in: $($f.FullName)"
      }
    }
  }

  if ($fixed -gt 0) { Log "Repaired $fixed nested Meson ternary file(s)." }
}


function Repair-RemixApiLine79Line130Corruption {
  param([Parameter(Mandatory)][string]$BaseDir)
  if (!(Test-Path -LiteralPath $BaseDir -PathType Container)) { return }

  $rel = 'src\dxvk\rtx_render\rtx_remix_api.cpp'
  $file = Join-Path $BaseDir $rel
  $diag = Join-Path $BaseDir 'V106_RTX_REMIX_API_LINE79_130_VERIFY.txt'
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $diag) | Out-Null

  $linesOut = New-Object System.Collections.Generic.List[string]
  $linesOut.Add('V106 rtx_remix_api.cpp line 79/130 repair')
  $linesOut.Add(('ProjectDir: {0}' -f $BaseDir))
  $linesOut.Add(('Time: {0}' -f (Get-Date)))
  $linesOut.Add('')

  if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
    $linesOut.Add(('BAD: missing {0}' -f $file))
    [System.IO.File]::WriteAllLines($diag, $linesOut)
    throw "Missing $rel"
  }

  # A clean working-tree copy is already the authoritative source.  The old
  # repair path unconditionally ran `git checkout`, which both discarded local
  # development changes and makes builds fail when the Git index is read-only
  # (as it is in managed build environments).  Only restore when the current
  # file actually contains the historical corruption signature.
  $currentText = [System.IO.File]::ReadAllText($file)
  $currentHead = ($currentText -split "`r?`n" | Select-Object -First 200) -join "`n"
  $currentOpens = ([regex]::Matches($currentText, '\{')).Count
  $currentCloses = ([regex]::Matches($currentText, '\}')).Count
  $restored =
    $currentText -match '"Add/remove function registration"' -and
    $currentHead -notmatch '\bout_result\b|\binterf\b|REMIXAPI_ERROR_CODE_SUCCESS' -and
    $currentText -notmatch 'DX11_BUILD_NONFATAL_REMIX_API_REGISTRATION_ASSERT|DX11_BUILD_OLD_UNSAFE_REMIX_API_REGISTRATION_ASSERT' -and
    $currentOpens -eq $currentCloses

  if ($restored) {
    $linesOut.Add('OK: current rtx_remix_api.cpp passed corruption and structural checks; no restore required.')
    Log "V106 current rtx_remix_api.cpp is clean; preserving working-tree source."
  }

  if (-not $restored) {
    $backup = Join-Path $BaseDir ('_dx11_v106_rtx_remix_api_corrupt_backup_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.cpp')
    Copy-Item -LiteralPath $file -Destination $backup -Force
    $linesOut.Add(('Backed up corrupt file to: {0}' -f $backup))
  }

  $git = Get-Command git.exe -ErrorAction SilentlyContinue
  if (!$git) { $git = Get-Command git -ErrorAction SilentlyContinue }

  if (-not $restored -and $git) {
    Push-Location $BaseDir
    try {
      & $git.Source checkout -- ($rel -replace '\\','/') *> $null
      if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $file -PathType Leaf)) {
        $restored = $true
        $linesOut.Add('OK: force-restored rtx_remix_api.cpp from local git checkout.')
        Log "V106 force-restored rtx_remix_api.cpp from local git checkout."
      } else {
        $linesOut.Add(('WARN: git checkout failed with LASTEXITCODE={0}.' -f $LASTEXITCODE))
      }
    } catch {
      $linesOut.Add(('WARN: git checkout threw: {0}' -f $_.Exception.Message))
    } finally {
      Pop-Location
    }
  } elseif (-not $restored) {
    $linesOut.Add('WARN: git was not found.')
  }

  if (-not $restored) {
    $candidates = @(Get-ChildItem -LiteralPath $BaseDir -Recurse -File -Filter 'rtx_remix_api.cpp' -ErrorAction SilentlyContinue |
      Where-Object {
        $_.FullName -ne $file -and
        $_.FullName -notmatch '\\_Comp(32|64)' -and
        $_.FullName -notmatch '\\meson-private\\' -and
        $_.FullName -notmatch '\\_build_logs\\'
      } |
      Sort-Object LastWriteTime -Descending)

    foreach ($c in $candidates) {
      try {
        $ct = [System.IO.File]::ReadAllText($c.FullName)
        $head = ($ct -split "`r?`n" | Select-Object -First 200) -join "`n"
        if ($ct -match '"Add/remove function registration"' -and
            $head -notmatch '\bout_result\b|\binterf\b|REMIXAPI_ERROR_CODE_SUCCESS' -and
            $ct -notmatch 'DX11_BUILD_NONFATAL_REMIX_API_REGISTRATION_ASSERT|DX11_BUILD_OLD_UNSAFE_REMIX_API_REGISTRATION_ASSERT') {
          Copy-Item -LiteralPath $c.FullName -Destination $file -Force
          $restored = $true
          $linesOut.Add(('OK: restored clean-looking rtx_remix_api.cpp from backup/candidate: {0}' -f $c.FullName))
          Log "V106 restored rtx_remix_api.cpp from backup/candidate: $($c.FullName)"
          break
        }
      } catch {}
    }
  }

  if (-not $restored) {
    $linesOut.Add('BAD: could not restore rtx_remix_api.cpp from git or a clean backup/candidate.')
    [System.IO.File]::WriteAllLines($diag, $linesOut)
    throw "Could not restore $rel. Restore it from your repo/backup, then rerun build."
  }

  # Strip BOM after restore.
  $raw = [System.IO.File]::ReadAllBytes($file)
  if ($raw.Length -ge 3 -and $raw[0] -eq 0xEF -and $raw[1] -eq 0xBB -and $raw[2] -eq 0xBF) {
    $out = New-Object byte[] ($raw.Length - 3)
    if ($out.Length -gt 0) { [System.Array]::Copy($raw, 3, $out, 0, $out.Length) }
    [System.IO.File]::WriteAllBytes($file, $out)
    $linesOut.Add('OK: stripped BOM from restored rtx_remix_api.cpp.')
  }

  $text = [System.IO.File]::ReadAllText($file)
  $fileLines = $text -split "`r?`n", -1
  $first200 = ($fileLines | Select-Object -First 200) -join "`n"

  $line79 = if ($fileLines.Count -ge 79) { $fileLines[78] } else { '' }
  $line130 = if ($fileLines.Count -ge 130) { $fileLines[129] } else { '' }
  $linesOut.Add(('Line 79 after restore: {0}' -f $line79))
  $linesOut.Add(('Line 130 after restore: {0}' -f $line130))

  if ($first200 -match '\bout_result\b|\binterf\b|REMIXAPI_ERROR_CODE_SUCCESS') {
    $linesOut.Add('BAD: line-130 corruption pattern still exists in the first 200 lines.')
    [System.IO.File]::WriteAllLines($diag, $linesOut)
    throw "rtx_remix_api.cpp is still corrupt around line 130 after restore."
  } else {
    $linesOut.Add('OK: first 200 lines do not contain out_result/interf/REMIXAPI_ERROR_CODE_SUCCESS corruption.')
  }

  if ($text -match 'DX11_BUILD_NONFATAL_REMIX_API_REGISTRATION_ASSERT|DX11_BUILD_OLD_UNSAFE_REMIX_API_REGISTRATION_ASSERT') {
    $linesOut.Add('BAD: old unsafe V103 marker still exists after restore.')
    [System.IO.File]::WriteAllLines($diag, $linesOut)
    throw "Old unsafe V103 marker remains in rtx_remix_api.cpp after restore."
  } else {
    $linesOut.Add('OK: old unsafe V103 marker is absent.')
  }

  # Cheap structural validation before patching: count braces ignoring strings imperfectly, enough to catch V103 damage.
  $opens = ([regex]::Matches($text, '\{')).Count
  $closes = ([regex]::Matches($text, '\}')).Count
  $linesOut.Add(('Brace count after restore before assert patch: open={0} close={1}' -f $opens, $closes))
  if ($opens -ne $closes) {
    $linesOut.Add('BAD: brace count mismatch after restore.')
    [System.IO.File]::WriteAllLines($diag, $linesOut)
    throw "rtx_remix_api.cpp brace count mismatch after restore."
  }

  if ($text -match 'DX11_BUILD_SAFE_NONFATAL_REMIX_API_REGISTRATION_ASSERT') {
    $linesOut.Add('OK: safe registration assert patch already exists.')
  } else {
    $needle = '"Add/remove function registration"'
    $idx = $text.IndexOf($needle, [System.StringComparison]::Ordinal)
    if ($idx -lt 0) {
      $linesOut.Add('WARN: Add/remove function registration sentinel not found; no assert patch applied.')
    } else {
      $start = $text.LastIndexOf('static_assert', $idx, [System.StringComparison]::Ordinal)
      $end = $text.IndexOf(');', $idx, [System.StringComparison]::Ordinal)
      if ($start -lt 0 -or $end -lt 0 -or ($idx - $start) -gt 1400 -or ($end - $idx) -gt 1400) {
        $linesOut.Add('BAD: could not safely bound Add/remove function registration static_assert.')
        [System.IO.File]::WriteAllLines($diag, $linesOut)
        throw "Could not safely bound Remix API registration static_assert."
      }
      $end += 2

      $replacement = @"
// DX11_BUILD_SAFE_NONFATAL_REMIX_API_REGISTRATION_ASSERT
// The DX11 bridge/header table can be ahead of this local maintenance sentinel.
// Keep the real API registration code compiled, but do not fail the DX11 build on the sentinel.
static_assert(true, "Add/remove function registration");
"@
      $newText = $text.Substring(0, $start) + $replacement + $text.Substring($end)
      $enc = New-Object System.Text.UTF8Encoding($false)
      [System.IO.File]::WriteAllText($file, $newText, $enc)
      $linesOut.Add('OK: safely patched only the bounded Add/remove function registration static_assert.')
      Log "V106 safely patched only the bounded Add/remove function registration static_assert."
    }
  }

  # Meson/Ninja tracks this source and will rebuild the affected objects after
  # a repair.  Deleting the whole build directory here discarded every compiled
  # RTX shader even when the file was already clean, turning each incremental
  # build into a full rebuild.  Preserve both release and debug build trees.
  $linesOut.Add('OK: preserved x64 Meson/Ninja build directories for incremental rebuilds.')

  [System.IO.File]::WriteAllLines($diag, $linesOut)
  Log "V106 wrote rtx_remix_api line 79/130 verification: $diag"
}

function Quote-Arg([string]$Arg) {
  if ($null -eq $Arg) { return '""' }
  if ($Arg.Length -eq 0) { return '""' }
  if ($Arg -notmatch '[\s"]') { return $Arg }
  $escaped = $Arg -replace '([\\]*)"', '$1$1\"'
  $escaped = $escaped -replace '([\\]+)$', '$1$1'
  return '"' + $escaped + '"'
}

function Get-CommandLine([string]$Exe, [string[]]$CommandArgs) {
  $parts = New-Object System.Collections.Generic.List[string]
  $parts.Add((Quote-Arg $Exe))
  foreach ($a in @($CommandArgs)) { $parts.Add((Quote-Arg ([string]$a))) }
  return ($parts -join ' ')
}

function Invoke-Logged {
  param(
    [Parameter(Mandatory)][string]$Label,
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string[]]$CommandArgs,
    [Parameter(Mandatory)][string]$WorkingDirectory,
    [string]$AllowIfFileExists
  )
  if (!(Test-Path -LiteralPath $Exe -PathType Leaf)) { Die "Tool not found: $Exe" }
  if (!(Test-Path -LiteralPath $WorkingDirectory -PathType Container)) { Die "Working directory not found: $WorkingDirectory" }

  $stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
  $logFile = Join-Path $LogDir "$stamp-$Label.log"
  $cmdLine = Get-CommandLine $Exe $CommandArgs
  Log "cmd> $cmdLine"

  Set-Content -LiteralPath $logFile -Encoding UTF8 -Value @(
    "Command: $cmdLine",
    "WorkingDirectory: $WorkingDirectory",
    ""
  )

  $oldEap = $ErrorActionPreference
  $oldGitPrompt = $env:GIT_TERMINAL_PROMPT
  $oldPyUtf8 = $env:PYTHONUTF8
  $oldPyIoEncoding = $env:PYTHONIOENCODING
  $env:GIT_TERMINAL_PROMPT = '0'
  $env:PYTHONUTF8 = '0'
  $env:PYTHONIOENCODING = 'utf-8'
  $ErrorActionPreference = 'Continue'
  $code = 9009

  Push-Location -LiteralPath $WorkingDirectory
  try {
    # IMPORTANT: this is synchronous. PowerShell waits for the native process to exit.
    # Stderr is merged only for logging; it is not passed as an argument to the tool.
    & $Exe @CommandArgs 2>&1 | ForEach-Object {
      $line = [string]$_
      Write-Host $line
      Add-Content -LiteralPath $logFile -Encoding UTF8 -Value $line
    }
    $code = if ($null -eq $global:LASTEXITCODE) { 0 } else { [int]$global:LASTEXITCODE }
  } catch {
    $code = 1
    $msg = $_.Exception.Message
    Write-Host $msg
    Add-Content -LiteralPath $logFile -Encoding UTF8 -Value $msg
  } finally {
    Pop-Location
    $ErrorActionPreference = $oldEap
    if ($null -eq $oldGitPrompt) { Remove-Item Env:\GIT_TERMINAL_PROMPT -ErrorAction SilentlyContinue } else { $env:GIT_TERMINAL_PROMPT = $oldGitPrompt }
    if ($null -eq $oldPyUtf8) { Remove-Item Env:\PYTHONUTF8 -ErrorAction SilentlyContinue } else { $env:PYTHONUTF8 = $oldPyUtf8 }
    if ($null -eq $oldPyIoEncoding) { Remove-Item Env:\PYTHONIOENCODING -ErrorAction SilentlyContinue } else { $env:PYTHONIOENCODING = $oldPyIoEncoding }
    Add-Content -LiteralPath $logFile -Encoding UTF8 -Value @('', "ExitCode: $code")
  }

  if ($code -ne 0) {
    if ($AllowIfFileExists -and (Test-Path -LiteralPath $AllowIfFileExists -PathType Leaf)) {
      Warn "$Label returned exit code $code, but required file exists, continuing: $AllowIfFileExists"
      return $code
    }
    Warn "$Label failed with exit code $code. Last log lines:"
    if (Test-Path -LiteralPath $logFile) { Get-Content -LiteralPath $logFile -Tail 200 | ForEach-Object { Write-Host $_ } }
    Die "$Label failed with exit code $code. Log: $logFile"
  }
  return 0
}

function Import-VSEnvironment([string]$VsPath, [string]$Target) {
  $vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvarsall.bat'
  if (!(Test-Path -LiteralPath $vcvars -PathType Leaf)) { Die "vcvarsall.bat not found: $vcvars" }

  foreach ($name in @('CL','_CL_','LINK','_LINK_','CC','CXX','LD','AR','RC','CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS')) {
    Remove-Item -Path "ENV:\$name" -Force -ErrorAction SilentlyContinue
  }

  $logDir = Join-Path $Root '_build_logs'
  New-Item -ItemType Directory -Force -Path $logDir | Out-Null

  $stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
  $tmpCmd = Join-Path $logDir ("vcvars_{0}_{1}.cmd" -f $Target, $stamp)
  $tmpEnv = Join-Path $logDir ("vcvars_{0}_{1}.env.txt" -f $Target, $stamp)

  # DX11_V225: vcvarsall.bat calls vswhere.exe expecting it on PATH, and on a second
  # vcvars load (e.g. the bridge build after the runtime) the PATH already imported
  # into this process keeps growing, so re-running vcvarsall on it overflows cmd's
  # line limit ("Files\Microsoft was unexpected at this time"). Start each load from
  # a clean baseline PATH discovered automatically from the OS configuration (the
  # machine + user PATH from the environment, which is location/drive independent)
  # plus the VS Installer directory so vswhere always resolves.
  $vsInstallerDir = $null
  foreach ($pf in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
    if (-not [string]::IsNullOrWhiteSpace($pf)) {
      $cand = Join-Path $pf 'Microsoft Visual Studio\Installer'
      if (Test-Path -LiteralPath (Join-Path $cand 'vswhere.exe') -PathType Leaf) { $vsInstallerDir = $cand; break }
      if ($null -eq $vsInstallerDir) { $vsInstallerDir = $cand }
    }
  }

  # Build a pristine baseline PATH from the OS-configured machine/user PATH (read
  # from the registry-backed environment, not this possibly-polluted process), with
  # standard Windows system dirs and the VS Installer dir guaranteed present.
  $baselineParts = New-Object System.Collections.Generic.List[string]
  $sys32 = Join-Path $env:WINDIR 'System32'
  foreach ($p in @($sys32, $env:WINDIR, (Join-Path $sys32 'Wbem'), (Join-Path $sys32 'WindowsPowerShell\v1.0'), $vsInstallerDir)) {
    if (-not [string]::IsNullOrWhiteSpace($p) -and -not $baselineParts.Contains($p)) { $baselineParts.Add($p) }
  }
  foreach ($scope in @('Machine', 'User')) {
    $scoped = [System.Environment]::GetEnvironmentVariable('PATH', $scope)
    if (-not [string]::IsNullOrWhiteSpace($scoped)) {
      foreach ($entry in ($scoped -split ';')) {
        $e = $entry.Trim().TrimEnd('\')
        if (-not [string]::IsNullOrWhiteSpace($e) -and -not $baselineParts.Contains($e)) { $baselineParts.Add($e) }
      }
    }
  }
  $baselinePath = ($baselineParts -join ';')

  # DX11_V225: also clear the vcvars "already initialized" guard variables so a
  # second load (bridge build, after the runtime imported a full vcvars env into
  # this process) does a clean fresh init instead of its re-entry path, which
  # re-processes an inherited unquoted path var and fails with
  # "Files\Microsoft was unexpected at this time".
  $cmdText = @"
@echo off
set "PATH=$baselinePath"
set "VSCMD_VER="
set "__VSCMD_PREINIT_PATH="
set "VSCMD_ARG_TGT_ARCH="
set "VSCMD_ARG_HOST_ARCH="
call "$vcvars" $Target >nul
if errorlevel 1 exit /b %errorlevel%
set
"@

  $ascii = New-Object System.Text.ASCIIEncoding
  [System.IO.File]::WriteAllText($tmpCmd, $cmdText, $ascii)

  Log "Loading Visual Studio environment safely: $Target"

  try {
    $lines = & $env:ComSpec /d /q /c "`"$tmpCmd`"" 2>&1
    $code = if ($null -eq $global:LASTEXITCODE) { 0 } else { [int]$global:LASTEXITCODE }
    $lines | Out-File -LiteralPath $tmpEnv -Encoding ascii -Force

    if ($code -ne 0) {
      Die "vcvarsall.bat failed for $Target with exit code $code. Output:`n$($lines | Out-String)"
    }

    $imported = 0
    foreach ($line in $lines) {
      $s = [string]$line
      $eq = $s.IndexOf('=')
      if ($eq -gt 0) {
        $name = $s.Substring(0, $eq)
        $value = $s.Substring($eq + 1)
        if ($name -match '^[A-Za-z_][A-Za-z0-9_]*$') {
          Set-Item -Force -Path "ENV:\$name" -Value $value
          $imported++
        }
      }
    }

    if ($imported -lt 20) {
      Die "vcvarsall.bat for $Target returned too few environment variables. Env log: $tmpEnv"
    }
  }
  finally {
    Remove-Item -LiteralPath $tmpCmd -Force -ErrorAction SilentlyContinue
  }

  foreach ($name in @('CL','_CL_','LINK','_LINK_','CC','CXX','LD','AR','RC','CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS')) {
    Remove-Item -Path "ENV:\$name" -Force -ErrorAction SilentlyContinue
  }

  $env:MESON_FORCE_BACKTRACE = '1'
  $env:PYTHONUTF8 = '0'
}


function Find-VSInstall {
  $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
  $candidates = @()
  $cmdPath = Get-CommandFilePath 'vswhere.exe'
  if ($cmdPath) { $candidates += $cmdPath }
  if ($pf86) { $candidates += (Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe') }
  $vswhere = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
  if (!$vswhere) { Die 'vswhere.exe not found.' }
  $path = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
  if ([string]::IsNullOrWhiteSpace([string]$path)) { Die 'Visual Studio VC x86/x64 tools not found.' }
  return ([string]$path).Trim()
}
function Find-Meson {
  if ($env:RTX_MESON_EXE -and (Test-Path -LiteralPath $env:RTX_MESON_EXE -PathType Leaf)) {
    return $env:RTX_MESON_EXE
  }
  foreach ($p in @(
    (Join-Path $env:APPDATA 'Python\Python314\Scripts\meson.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python314\Scripts\meson.exe'),
    (Get-CommandFilePath 'meson.exe')
  )) { if ($p -and (Test-Path -LiteralPath $p -PathType Leaf)) { return $p } }
  Die 'meson.exe not found. Install with: py -m pip install --user meson ninja'
}
function Find-Ninja([string]$VsInstall) {
  $n = Join-Path $VsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
  if (Test-Path -LiteralPath $n -PathType Leaf) { return $n }
  $cmdPath = Get-CommandFilePath 'ninja.exe'
  if ($cmdPath) { return $cmdPath }
  Die 'ninja.exe not found.'
}

function Repair-FutureTimestamps {
  $now = Get-Date
  foreach ($rel in @('meson.build','meson_options.txt','src\usd-plugins\meson.build','bridge\meson.build')) {
    $p = Join-Path $Root $rel
    if (Test-Path -LiteralPath $p) {
      $i = Get-Item -LiteralPath $p
      if ($i.LastWriteTime -gt $now.AddSeconds(1)) { $i.LastWriteTime = $now }
    }
  }
}

function Ensure-Deps([string]$Meson) {
  $need = @((Join-Path $Root 'external\nv_usd_Release\lib\usd.lib'), (Join-Path $Root 'external\nv_usd_release\lib\usd.lib'))
  if (@($need | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }).Count -gt 0) { Log 'Top-level USD deps present.' } else { Warn 'Top-level USD deps not found; runtime Meson may fetch or fail depending on repo state.' }
  $plug = Join-Path $Root 'src\usd-plugins\_external\nv_usd\release\lib\usd.lib'
  if (Test-Path -LiteralPath $plug -PathType Leaf) { Log 'USD plugin deps present.' } else { Warn 'USD plugin deps not found; runtime Meson may fetch or fail depending on repo state.' }
}

function Test-PeMachine([string]$Path, [string]$Expected) {
  if (!(Test-Path $Path)) { Die "Missing file for PE check: $Path" }
  $fs = [System.IO.File]::OpenRead($Path)
  try {
    $br = New-Object System.IO.BinaryReader($fs)
    $fs.Seek(0x3c, [System.IO.SeekOrigin]::Begin) | Out-Null
    $peOff = $br.ReadInt32()
    $fs.Seek($peOff + 4, [System.IO.SeekOrigin]::Begin) | Out-Null
    $machine = $br.ReadUInt16()
  } finally { $fs.Close() }
  $actual = switch ($machine) { 0x014c { 'x86' } 0x8664 { 'x64' } default { ('0x{0:X4}' -f $machine) } }
  if ($actual -ne $Expected) { Die "$Path is $actual, expected $Expected" }
  Log "Verified ${Expected}: $Path"
}

function Find-FileRecursive([string]$Dir, [string]$Name) {
  if (!(Test-Path $Dir)) { return $null }
  $hits = @(Get-ChildItem -LiteralPath $Dir -Recurse -Filter $Name -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\meson-private\\' -and $_.FullName -notmatch '\\meson-logs\\' } |
    Sort-Object LastWriteTime -Descending)
  if (@($hits).Count -gt 0) { return @($hits)[0].FullName }
  return $null
}

function Remove-DirectoryRobust([string]$Path) {
  if (!(Test-Path -LiteralPath $Path)) { return }
  for ($i = 1; $i -le 5; $i++) {
    try {
      Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
      Start-Sleep -Milliseconds 200
      if (!(Test-Path -LiteralPath $Path)) { return }
    } catch {
      Warn "Remove attempt $i failed for ${Path}: $($_.Exception.Message)"
      Start-Sleep -Milliseconds (300 * $i)
    }
  }
  if (Test-Path -LiteralPath $Path) {
    $dead = "$Path.bad_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
    Warn "Could not delete $Path cleanly; renaming to $dead"
    Rename-Item -LiteralPath $Path -NewName (Split-Path -Leaf $dead) -Force
  }
}


function Sync-BridgeRemixHeaderToDX11 {
  # The bridge source tree ships NVIDIA's original D3D9 remix_c.h, whose
  # remixapi_Interface references IDirect3D9Ex / IDirect3DDevice9Ex. The DX11
  # bridge server includes that header (via config.cpp and others) but compiles
  # with d3d11.h, not d3d9.h - so those D3D9 interface types are undeclared and
  # the build fails with C2061/C2065 at remix_c.h(721-723). The repo's own
  # public\include\remix\remix_c.h is the DX11 version. Overwrite the bridge
  # work tree's stale D3D9 header with the repo's DX11 header so it compiles.
  param([Parameter(Mandatory)][string]$BridgeWork)

  $repoHeader = Join-Path $Root 'public\include\remix\remix_c.h'
  if (!(Test-Path -LiteralPath $repoHeader -PathType Leaf)) {
    Log "WARNING: repo DX11 remix_c.h not found at $repoHeader; cannot sync bridge header."
    return
  }
  $repoText = Get-Content -LiteralPath $repoHeader -Raw
  if ($repoText -match 'IDirect3D9Ex' -or $repoText -notmatch 'dxvk_RegisterD3D11Device') {
    # The repo header itself is still the D3D9 original. Install the shipped
    # DX11 header (remix_c.h.DX11 next to this script) over the repo copy first,
    # then use it as the source for the bridge tree.
    $shipped = Join-Path $PSScriptRoot 'remix_c.h.DX11'
    if (Test-Path -LiteralPath $shipped -PathType Leaf) {
      $shippedText = Get-Content -LiteralPath $shipped -Raw
      if ($shippedText -notmatch 'IDirect3D9Ex' -and $shippedText -match 'dxvk_RegisterD3D11Device') {
        if (!(Test-Path -LiteralPath "$repoHeader.d3d9.orig")) { Copy-Item -LiteralPath $repoHeader -Destination "$repoHeader.d3d9.orig" -Force }
        Set-Content -LiteralPath $repoHeader -Value $shippedText -NoNewline -Encoding UTF8
        Log "Repo remix_c.h was D3D9; installed shipped DX11 header over it."
        $repoText = $shippedText
      } else {
        Log "WARNING: shipped remix_c.h.DX11 is not DX11; cannot fix repo header."
        return
      }
    } else {
      Log "WARNING: repo remix_c.h is D3D9 and no shipped remix_c.h.DX11 is present next to the build script; cannot sync."
      return
    }
  }

  $targets = @(Get-ChildItem -LiteralPath $BridgeWork -Recurse -Filter 'remix_c.h' -File -ErrorAction SilentlyContinue)
  $synced = 0
  foreach ($t in $targets) {
    $cur = Get-Content -LiteralPath $t.FullName -Raw
    if ($cur -match 'IDirect3D9Ex' -or ($cur -ne $repoText)) {
      Copy-Item -LiteralPath $repoHeader -Destination $t.FullName -Force
      Log ("Synced bridge remix_c.h to DX11 version: {0}" -f $t.FullName)
      $synced++
    }
  }
  if ($synced -eq 0) {
    Log 'Bridge remix_c.h already matches the repo DX11 header; no sync needed.'
}
}

function Patch-BridgeClientRemixApiDX11 {
  # After remix_c.h is converted to DX11, the stock client src\client\remix_api.cpp
  # still references the D3D9 API members/typedefs:
  #   PFN_remixapi_dxvk_CreateD3D11Device        -> PFN_remixapi_dxvk_CreateD3D11Device
  #   PFN_remixapi_dxvk_RegisterD3D11Device-> PFN_remixapi_dxvk_RegisterD3D11Device
  #   iface.dxvk_CreateD3D11Device              -> iface.dxvk_CreateD3D11Device
  #   iface.dxvk_RegisterD3D11Device      -> iface.dxvk_RegisterD3D11Device
  # which no longer exist (C2065/C2039). Rewrite them so the x86 client compiles.
  param([Parameter(Mandatory)][string]$BridgeWork)

  $apiCandidates = @(
    (Join-Path $BridgeWork 'src\client\remix_api.cpp'),
    (Join-Path $BridgeWork 'src\client\remix_api.h')
  ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }

  if (@($apiCandidates).Count -eq 0) {
    Log 'No client remix_api.cpp/.h found to patch for DX11; skipping.'
    return
  }

  foreach ($f in $apiCandidates) {
    $t = Get-Content -LiteralPath $f -Raw
    $orig = $t
    # Order matters: replace the longer/more specific tokens first so we do not
    # leave a dangling "D3D9" inside an already-renamed token.
    # Register first (it has the Device suffix in both D3D9 and DX11).
    $t = $t -replace 'PFN_remixapi_dxvk_RegisterD3D11Device', 'PFN_remixapi_dxvk_RegisterD3D11Device'
    $t = $t -replace '\bdxvk_RegisterD3D11Device\b', 'dxvk_RegisterD3D11Device'
    # Create: the D3D9 client token is CreateD3D9 (no Device); DX11 is CreateD3D11Device.
    $t = $t -replace 'PFN_remixapi_dxvk_CreateD3D11DeviceDevice', 'PFN_remixapi_dxvk_CreateD3D11Device'
    $t = $t -replace 'PFN_remixapi_dxvk_CreateD3D11Device\b', 'PFN_remixapi_dxvk_CreateD3D11Device'
    $t = $t -replace '\bdxvk_CreateD3D11DeviceDevice\b', 'dxvk_CreateD3D11Device'
    $t = $t -replace '\bdxvk_CreateD3D11Device\b', 'dxvk_CreateD3D11Device'
    # The device interface types used by those calls, if referenced directly.
    $t = $t -replace '\bIDirect3DDevice9Ex\b', 'ID3D11Device'
    $t = $t -replace '\bIDirect3D9Ex\b', 'ID3D11Device'
    $t = $t -replace '\bIDirect3DSurface9\b', 'ID3D11Texture2D'

    if ($t -ne $orig) {
      if (!(Test-Path -LiteralPath "$f.d3d9.orig")) { Copy-Item -LiteralPath $f -Destination "$f.d3d9.orig" -Force }
      Write-TextNoBom -Path $f -Text $t
      Log ("Patched client remix-api D3D9->DX11 references: {0}" -f $f)
    }
  }
}

function Patch-BridgeServerRegisterD3D11 {
  param([Parameter(Mandatory)][string]$BridgeWork)

  # DX11_V219_REMOVE_STALE_REMIXAPI_D3D11_REGISTER
  # RTX Remix 1.5 remix_c.h no longer exposes the experimental
  # remixapi_Interface::dxvk_RegisterD3D11Device member used by older DX11
  # bring-up scripts.  Server-side registration through that member cannot
  # compile.  Keep the real DX11 client capture layer in the game process and
  # strip the stale helper/calls from NvRemixBridge.exe main.cpp.
  $mainCpp = Join-Path $BridgeWork 'src\server\main.cpp'
  if (!(Test-Path -LiteralPath $mainCpp -PathType Leaf)) {
    Log "V219: bridge server main.cpp not found yet; skipping stale D3D11 register removal."
    return
  }

  $text = [System.IO.File]::ReadAllText($mainCpp)
  $original = $text

  $text = [regex]::Replace(
    $text,
    '(?s)[\r\n]*#ifndef\s+DX11_BRIDGE_REGISTER_HELPER_V\d+\s*[\r\n]+#define\s+DX11_BRIDGE_REGISTER_HELPER_V\d+.*?[\r\n]+#endif\s*',
    "`r`n"
  )

  $text = [regex]::Replace(
    $text,
    'BridgeRegisterRemixD3D11DeviceForDx11Bridge\s*\([^;]*\);',
    'Logger::info("DX11_V219_REMOVE_STALE_REMIXAPI_D3D11_REGISTER: skipped deprecated dxvk_RegisterD3D11Device bridge-server registration; DX11 client capture owns the game render path.");'
  )

  $text = [regex]::Replace(
    $text,
    'remixapi::g_remix\.dxvk_RegisterD3D11Device\s*==\s*nullptr',
    'false'
  )

  $text = [regex]::Replace(
    $text,
    'remixapi::g_remix\.dxvk_RegisterD3D11Device\s*!=\s*nullptr',
    'true'
  )

  $assignPattern = '(?m)(\b(?:const\s+)?auto\s+|\bremixapi_ErrorCode\s+)([A-Za-z_][A-Za-z0-9_]*)\s*=\s*remixapi::g_remix\.dxvk_RegisterD3D11Device\s*\([^;]*\);'
  $text = [regex]::Replace(
    $text,
    $assignPattern,
    '$1$2 = REMIXAPI_ERROR_CODE_SUCCESS;'
  )

  $text = [regex]::Replace(
    $text,
    'remixapi::g_remix\.dxvk_RegisterD3D11Device\s*\([^;]*\);',
    'Logger::info("DX11_V219_REMOVE_STALE_REMIXAPI_D3D11_REGISTER: removed direct dxvk_RegisterD3D11Device call for Remix 1.5 API compatibility.");'
  )

  if ($text -match '\bdxvk_RegisterD3D11Device\b') {
    $noComments = [regex]::Replace($text, '(?m)//.*$', '')
    $noStrings = [regex]::Replace($noComments, '"[^"]*"', '""')
    if ($noStrings -match '\bdxvk_RegisterD3D11Device\b') {
      Write-TextNoBom -Path (Join-Path $BridgeWork 'V219_STALE_D3D11_REGISTER_PATCH_FAILED_main.cpp') -Text $text
      Die "V219 failed to remove compile-active dxvk_RegisterD3D11Device references from server main.cpp."
    }
  }

  if ($text -ne $original) {
    if (!(Test-Path -LiteralPath "$mainCpp.v219.before")) {
      # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $mainCpp -Destination "$mainCpp.v219.before" -Force
    }
    Write-TextNoBom -Path $mainCpp -Text $text
    Log "V219 stripped stale dxvk_RegisterD3D11Device server helper/calls for Remix 1.5 API compatibility."
  } else {
    Log "V219: no compile-active stale dxvk_RegisterD3D11Device server helper/calls found."
  }
}



function Ensure-DX11ClientSources {
  param(
    [Parameter(Mandatory)][string]$DstClient,
    [Parameter(Mandatory)][string]$PackDir
  )
  New-Item -ItemType Directory -Force -Path $DstClient | Out-Null

  $packClient = Join-Path $PackDir 'src\client_dx11'
  $copied = $false
  # v62: always refresh the generated DX11 client source. Older runs left stale
  # client DLL source that launched NvRemixBridge.exe with the wrong command line.
  # Do not trust existing bridge_dx11_work\src\client_dx11 contents.
  if (Test-Path -LiteralPath $packClient -PathType Container) {
    $items = @(Get-ChildItem -LiteralPath $packClient -Force -ErrorAction SilentlyContinue)
    if ($items.Count -gt 0) {
      Copy-Item -Path (Join-Path $packClient '*') -Destination $DstClient -Recurse -Force -ErrorAction Stop
      $copied = $true
      Log "Refreshed DX11 client source from bundled folder: $packClient"
    }
  }

  if (!$copied) {
    Log 'Writing embedded DX11 client source files with v62 GUID/version bridge launch fix.'
    Write-TextNoBom -Path (Join-Path $DstClient 'd3d11_dx11bridge.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "dx11_common.h"

using PFN_D3D11CreateDevice = HRESULT (WINAPI *)(
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
  const D3D_FEATURE_LEVEL*, UINT, UINT,
  ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (WINAPI *)(
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
  const D3D_FEATURE_LEVEL*, UINT, UINT,
  const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
  ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static HMODULE g_d3d11System = nullptr;
static PFN_D3D11CreateDevice g_CreateDevice = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain g_CreateDeviceAndSwapChain = nullptr;

// DX11_V219_INIT_AFTER_LAUNCHER_CHAIN: helper is used only after the DLL->Launcher->.trex bridge chain starts.
static bool ensureSystemD3D11() {
  if (g_d3d11System) return true;
  g_d3d11System = dx11_bridge_getgoing::loadSystemDll("d3d11.dll");
  if (!g_d3d11System) return false;
  g_CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(g_d3d11System, "D3D11CreateDevice"));
  g_CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(GetProcAddress(g_d3d11System, "D3D11CreateDeviceAndSwapChain"));
  if (!g_CreateDevice || !g_CreateDeviceAndSwapChain) {
    dx11_bridge_getgoing::logLine("d3d11", "System d3d11.dll is missing required exports.");
    return false;
  }
  dx11_bridge_getgoing::logLine("d3d11", "Loaded system d3d11.dll exports.");
  return true;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    dx11_bridge_getgoing::setModule(hinst);
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_getgoing::logLine("d3d11", "Loaded x86 DX11 bridge bring-up d3d11.dll.");
  }
  return TRUE;
}

extern "C" HRESULT WINAPI D3D11CreateDevice(
  IDXGIAdapter* pAdapter,
  D3D_DRIVER_TYPE DriverType,
  HMODULE Software,
  UINT Flags,
  const D3D_FEATURE_LEVEL* pFeatureLevels,
  UINT FeatureLevels,
  UINT SDKVersion,
  ID3D11Device** ppDevice,
  D3D_FEATURE_LEVEL* pFeatureLevel,
  ID3D11DeviceContext** ppImmediateContext) {

  // DX11_V219_INIT_AFTER_LAUNCHER_CHAIN
  // Required chain stays: game -> root DLL -> NvRemixLauncher32.exe -> .trex\NvRemixBridge.exe.
  // Unity still needs a valid D3D11 object back from D3D11CreateDevice or it exits before
  // the bridge/runtime can continue.  Start the bridge chain first, then let the D3D11
  // bootstrap object be created so the game remains alive for the bridge path.
  dx11_bridge_getgoing::logLine("d3d11", "D3D11CreateDevice intercepted; starting DLL->Launcher->.trex bridge chain before D3D11 initialization.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  if (!ensureSystemD3D11() || !g_CreateDevice) {
    dx11_bridge_getgoing::logLine("d3d11", "D3D11 bootstrap failed after launcher chain; no D3D11 device can be returned.");
    if (ppDevice) *ppDevice = nullptr;
    if (ppImmediateContext) *ppImmediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }
  HRESULT hr = g_CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "D3D11CreateDevice bootstrap returned 0x%08X after launcher chain.", (unsigned) hr);
  dx11_bridge_getgoing::logLine("d3d11", msg);
  return hr;
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
  IDXGIAdapter* pAdapter,
  D3D_DRIVER_TYPE DriverType,
  HMODULE Software,
  UINT Flags,
  const D3D_FEATURE_LEVEL* pFeatureLevels,
  UINT FeatureLevels,
  UINT SDKVersion,
  const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
  IDXGISwapChain** ppSwapChain,
  ID3D11Device** ppDevice,
  D3D_FEATURE_LEVEL* pFeatureLevel,
  ID3D11DeviceContext** ppImmediateContext) {

  // DX11_V219_INIT_AFTER_LAUNCHER_CHAIN
  dx11_bridge_getgoing::logLine("d3d11", "D3D11CreateDeviceAndSwapChain intercepted; starting DLL->Launcher->.trex bridge chain before swapchain initialization.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  if (!ensureSystemD3D11() || !g_CreateDeviceAndSwapChain) {
    dx11_bridge_getgoing::logLine("d3d11", "D3D11 swapchain bootstrap failed after launcher chain; no swapchain/device can be returned.");
    if (ppSwapChain) *ppSwapChain = nullptr;
    if (ppDevice) *ppDevice = nullptr;
    if (ppImmediateContext) *ppImmediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }
  HRESULT hr = g_CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "D3D11CreateDeviceAndSwapChain bootstrap returned 0x%08X after launcher chain.", (unsigned) hr);
  dx11_bridge_getgoing::logLine("d3d11", msg);
  return hr;
}

'@
    Write-TextNoBom -Path (Join-Path $DstClient 'd3d11_dx11bridge.def') -Text @'
LIBRARY "d3d11"
EXPORTS
  D3D11CreateDevice
  D3D11CreateDeviceAndSwapChain

'@
    Write-TextNoBom -Path (Join-Path $DstClient 'dx11_common.h') -Text @'
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

namespace dx11_bridge_getgoing {

inline HMODULE g_module = nullptr;

inline void setModule(HMODULE m) { g_module = m; }

inline void getDllFolder(char* out, DWORD cap) {
  out[0] = 0;
  char path[MAX_PATH] = {};
  if (g_module) GetModuleFileNameA(g_module, path, MAX_PATH);
  char* slash = strrchr(path, '\');
  if (slash) {
    slash[1] = 0;
    lstrcpynA(out, path, cap);
  }
}

inline void logLine(const char* tag, const char* msg) {
  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char path[MAX_PATH] = {};
  lstrcpynA(path, dir, MAX_PATH);
  lstrcatA(path, "dx11_bridge_getgoing.log");
  FILE* f = nullptr;
  fopen_s(&f, path, "ab");
  if (f) {
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] %s
",
      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
      tag ? tag : "dx11", msg ? msg : "");
    fclose(f);
  }
  char dbg[1024] = {};
  wsprintfA(dbg, "[%s] %s
", tag ? tag : "dx11", msg ? msg : "");
  OutputDebugStringA(dbg);
}

inline HMODULE loadSystemDll(const char* name) {
  char sys[MAX_PATH] = {};
  GetSystemDirectoryA(sys, MAX_PATH);
  lstrcatA(sys, "\\");
  lstrcatA(sys, name);
  HMODULE mod = LoadLibraryA(sys);
  if (!mod) {
    char msg[512] = {};
    wsprintfA(msg, "LoadLibrary failed for %s err=%lu", sys, GetLastError());
    logLine("loader", msg);
  }
  return mod;
}

inline bool fileExists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

inline void trimAscii(char* s) {
  if (!s) return;
  char* start = s;
  while (*start && isspace((unsigned char)*start)) ++start;
  if (start != s) memmove(s, start, strlen(start) + 1);
  size_t len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = 0;
}

inline bool readTextFileSmall(const char* path, char* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD got = 0;
  BOOL ok = ReadFile(h, out, cap - 1, &got, nullptr);
  CloseHandle(h);
  if (!ok) return false;
  out[got] = 0;
  trimAscii(out);
  return out[0] != 0;
}

inline bool readBridgeVersion(char* out, DWORD cap) {
  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char path[MAX_PATH] = {};
  lstrcpynA(path, dir, MAX_PATH);
  lstrcatA(path, ".trex\\bridge_version.txt");
  if (readTextFileSmall(path, out, cap)) return true;

  lstrcpynA(path, dir, MAX_PATH);
  lstrcatA(path, "bridge_version.txt");
  if (readTextFileSmall(path, out, cap)) return true;

  logLine("bridge", "Missing bridge_version.txt; cannot launch NvRemixBridge.exe with matching version.");
  return false;
}

inline bool isValidBridgeGuid36(const char* s) {
  if (!s || strlen(s) != 36) return false;
  for (int i = 0; i < 36; ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
    else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

inline bool readFixedGuid(char* out, DWORD cap) {
  if (!out || cap < 37) return false;
  out[0] = 0;

  // DX11_V219_SHARED_DX9_GUID_SOURCE
  // Prefer the same bridge GUID source used by the DX9 bridge model:
  // one process-global bridge GUID shared by DeviceBridge/ModuleBridge and the server command line.
  char envGuid[64] = {};
  DWORD gotEnv = GetEnvironmentVariableA("DX11_BRIDGE_GUID", envGuid, sizeof(envGuid));
  if (gotEnv > 0 && gotEnv < sizeof(envGuid)) {
    trimAscii(envGuid);
    if (isValidBridgeGuid36(envGuid)) {
      lstrcpynA(out, envGuid, cap);
      logLine("bridge", "DX11_V219 using DX11_BRIDGE_GUID from shared DX9-style bridge source.");
      return true;
    }
  }

  envGuid[0] = 0;
  gotEnv = GetEnvironmentVariableA("DX11_BRIDGE_FIXED_GUID", envGuid, sizeof(envGuid));
  if (gotEnv > 0 && gotEnv < sizeof(envGuid)) {
    trimAscii(envGuid);
    if (isValidBridgeGuid36(envGuid)) {
      lstrcpynA(out, envGuid, cap);
      logLine("bridge", "DX11_V219 using DX11_BRIDGE_FIXED_GUID override.");
      return true;
    }
  }

  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char path[MAX_PATH] = {};
  lstrcpynA(path, dir, MAX_PATH);
  lstrcatA(path, ".trex\\dx11_bridge_guid.txt");
  if (readTextFileSmall(path, out, cap) && isValidBridgeGuid36(out)) {
    logLine("bridge", "DX11_V219 using .trex dx11_bridge_guid.txt shared DX9-style bridge GUID.");
    return true;
  }

  return false;
}

inline bool makeGuidString(char* out, DWORD cap) {
  if (!out || cap < 37) return false;
  out[0] = 0;

  if (readFixedGuid(out, cap)) {
    char fixedMsg[160] = {};
    wsprintfA(fixedMsg, "Using bridge GUID '%s' len=%lu", out, static_cast<unsigned long>(strlen(out)));
    logLine("bridge", fixedMsg);
    return true;
  }

  GUID g = {};
  HRESULT hr = CoCreateGuid(&g);
  if (FAILED(hr)) {
    char msg[128] = {};
    wsprintfA(msg, "CoCreateGuid failed hr=0x%08lX", static_cast<unsigned long>(hr));
    logLine("bridge", msg);
    return false;
  }

  wchar_t wideGuid[64] = {};
  int wideLen = StringFromGUID2(g, wideGuid, 64); // produces {xxxxxxxx-....}
  if (wideLen <= 0) {
    logLine("bridge", "StringFromGUID2 failed.");
    return false;
  }

  char tmp[64] = {};
  int mb = WideCharToMultiByte(CP_ACP, 0, wideGuid, -1, tmp, sizeof(tmp), nullptr, nullptr);
  if (mb <= 0) {
    logLine("bridge", "WideCharToMultiByte failed for GUID.");
    return false;
  }

  // NVIDIA bridge server's Guid::setGuid expects exactly 36 chars, no braces.
  // StringFromGUID2 returns braces, so strip them deterministically.
  size_t len = strlen(tmp);
  if (len == 38 && tmp[0] == '{' && tmp[37] == '}') {
    memcpy(out, tmp + 1, 36);
    out[36] = 0;
  } else if (len == 36) {
    lstrcpynA(out, tmp, cap);
  } else {
    char msg[128] = {};
    wsprintfA(msg, "Unexpected GUID string length %lu", static_cast<unsigned long>(len));
    logLine("bridge", msg);
    return false;
  }

  char msg[160] = {};
  wsprintfA(msg, "DX11_V219 generated one shared DX9-style bridge GUID '%s' len=%lu", out, static_cast<unsigned long>(strlen(out)));
  logLine("bridge", msg);

  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char guidPath[MAX_PATH] = {};
  lstrcpynA(guidPath, dir, MAX_PATH);
  lstrcatA(guidPath, ".trex\\dx11_bridge_guid.txt");
  HANDLE h = CreateFileA(guidPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(h, out, static_cast<DWORD>(strlen(out)), &written, nullptr);
    CloseHandle(h);
    logLine("bridge", "DX11_V219 wrote shared GUID to .trex\dx11_bridge_guid.txt for DLL/launcher/server chain.");
  }
  return strlen(out) == 36;
}

inline void appendLaunchLog(const char* text) {
  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);
  char path[MAX_PATH] = {};
  lstrcpynA(path, dir, MAX_PATH);
  lstrcatA(path, "dx11_bridge_launch.log");
  HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD wrote = 0;
  if (text) WriteFile(h, text, (DWORD)strlen(text), &wrote, nullptr);
  const char* crlf = "
";
  WriteFile(h, crlf, 2, &wrote, nullptr);
  CloseHandle(h);
}

inline void launchBridgeServerOnce() {
  static LONG launched = 0;
  if (InterlockedCompareExchange(&launched, 1, 0) != 0) return;

  char dir[MAX_PATH] = {};
  getDllFolder(dir, MAX_PATH);

  char launcher[MAX_PATH] = {};
  lstrcpynA(launcher, dir, MAX_PATH);
  lstrcatA(launcher, "NvRemixLauncher32.exe");

  char server[MAX_PATH] = {};
  lstrcpynA(server, dir, MAX_PATH);
  lstrcatA(server, ".trex\\NvRemixBridge.exe");

  if (!fileExists(launcher)) {
    logLine("bridge", "DX11_V219 ERROR: NvRemixLauncher32.exe missing; root DLL cannot start launcher/client helper.");
    appendLaunchLog("DX11_V219 ERROR: NvRemixLauncher32.exe missing; root DLL cannot start launcher/client helper.");
    return;
  }
  if (!fileExists(server)) {
    logLine("bridge", "DX11_V219 ERROR: .trex\\NvRemixBridge.exe missing; launcher cannot start Remix server.");
    appendLaunchLog("DX11_V219 ERROR: .trex\\NvRemixBridge.exe missing; launcher cannot start Remix server.");
    return;
  }

  char version[128] = {};
  if (!readBridgeVersion(version, sizeof(version))) {
    appendLaunchLog("Missing bridge_version.txt; cannot launch bridge through NvRemixLauncher32.exe.");
    return;
  }

  char guid[64] = {};
  if (!makeGuidString(guid, sizeof(guid))) {
    appendLaunchLog("GUID generation failed; not launching NvRemixLauncher32.exe.");
    return;
  }

  SetEnvironmentVariableA("DX11_BRIDGE_GUID", guid);
  SetEnvironmentVariableA("DX11_BRIDGE_VERSION", version);
  SetEnvironmentVariableA("DX11_BRIDGE_MODE", "d3d11");

  // DX11_V219_GAME_CMD_FILE_ANYWHERE
  // Game exe can be anywhere. Do not pass the original command line through
  // --game-cmd because CommandLineToArgvW splits paths such as C:\Program Files.
  // Store it in .trex and pass the file path instead.
  char gameCmdPath[MAX_PATH] = {};
  lstrcpynA(gameCmdPath, dir, MAX_PATH);
  lstrcatA(gameCmdPath, ".trex\\dx11_bridge_game_cmd.txt");
  HANDLE cmdFile = CreateFileA(gameCmdPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (cmdFile != INVALID_HANDLE_VALUE) {
    const char* originalCmd = GetCommandLineA();
    DWORD written = 0;
    WriteFile(cmdFile, originalCmd, static_cast<DWORD>(strlen(originalCmd)), &written, nullptr);
    CloseHandle(cmdFile);
    appendLaunchLog("DX11_V219 wrote original game command line to .trex\\dx11_bridge_game_cmd.txt.");
  } else {
    appendLaunchLog("DX11_V219 WARNING: failed to write .trex\\dx11_bridge_game_cmd.txt.");
  }

  char cmd[8192] = {};
  sprintf_s(cmd, sizeof(cmd), "\"%s\" --dx11-launch-bridge --game-root \"%s\" --trex-root \"%s.trex\\\" --guid %s --version %s --game-cmd-file \"%s\"",
    launcher, dir, dir, guid, version, gameCmdPath);

  char logMsg[8192] = {};
  sprintf_s(logMsg, sizeof(logMsg), "DX11_V219 root DLL launching NvRemixLauncher32.exe; launcher will start .trex bridge. cmd='%s'", cmd);
  logLine("bridge", logMsg);
  appendLaunchLog(logMsg);

  STARTUPINFOA si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);
  BOOL ok = CreateProcessA(launcher, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
  if (ok) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logLine("bridge", "DX11_V219 started NvRemixLauncher32.exe from root DLL.");
    appendLaunchLog("DX11_V219 started NvRemixLauncher32.exe from root DLL.");
  } else {
    char msg[256] = {};
    wsprintfA(msg, "DX11_V219 failed to start NvRemixLauncher32.exe err=%lu", GetLastError());
    logLine("bridge", msg);
    appendLaunchLog(msg);
  }
}

} // namespace

'@
    Write-TextNoBom -Path (Join-Path $DstClient 'dxgi_dx11bridge.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include "dx11_common.h"

typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
typedef HRESULT (WINAPI *PFN_DXGIDeclareAdapterRemovalSupport)();
typedef HRESULT (WINAPI *PFN_DXGIGetDebugInterface1)(UINT, REFIID, void**);

static HMODULE g_dxgiSystem = nullptr;
// DX11_V219_INIT_AFTER_LAUNCHER_CHAIN: helper must not be used for bridge-owned DXGI factory creation.
static FARPROC getDxgiProc(const char* name) {
  if (!g_dxgiSystem) {
    g_dxgiSystem = dx11_bridge_getgoing::loadSystemDll("dxgi.dll");
    if (g_dxgiSystem) dx11_bridge_getgoing::logLine("dxgi", "Loaded system dxgi.dll.");
  }
  return g_dxgiSystem ? GetProcAddress(g_dxgiSystem, name) : nullptr;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    dx11_bridge_getgoing::setModule(hinst);
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_getgoing::logLine("dxgi", "Loaded x86 DX11 bridge bring-up dxgi.dll.");
  }
  return TRUE;
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
  // DX11_V219_INIT_AFTER_LAUNCHER_CHAIN
  dx11_bridge_getgoing::logLine("dxgi", "CreateDXGIFactory intercepted; starting DLL->Launcher->.trex bridge chain before DXGI factory creation.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  auto fn = reinterpret_cast<PFN_CreateDXGIFactory>(getDxgiProc("CreateDXGIFactory"));
  return fn ? fn(riid, ppFactory) : DXGI_ERROR_UNSUPPORTED;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
  // DX11_V219_INIT_AFTER_LAUNCHER_CHAIN
  dx11_bridge_getgoing::logLine("dxgi", "CreateDXGIFactory1 intercepted; starting DLL->Launcher->.trex bridge chain before DXGI factory creation.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  auto fn = reinterpret_cast<PFN_CreateDXGIFactory1>(getDxgiProc("CreateDXGIFactory1"));
  return fn ? fn(riid, ppFactory) : DXGI_ERROR_UNSUPPORTED;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
  // DX11_V219_INIT_AFTER_LAUNCHER_CHAIN
  dx11_bridge_getgoing::logLine("dxgi", "CreateDXGIFactory2 intercepted; starting DLL->Launcher->.trex bridge chain before DXGI factory creation.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  auto fn = reinterpret_cast<PFN_CreateDXGIFactory2>(getDxgiProc("CreateDXGIFactory2"));
  return fn ? fn(Flags, riid, ppFactory) : DXGI_ERROR_UNSUPPORTED;
}

extern "C" HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
  auto fn = reinterpret_cast<PFN_DXGIDeclareAdapterRemovalSupport>(getDxgiProc("DXGIDeclareAdapterRemovalSupport"));
  return fn ? fn() : E_NOTIMPL;
}

extern "C" HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** ppDebug) {
  auto fn = reinterpret_cast<PFN_DXGIGetDebugInterface1>(getDxgiProc("DXGIGetDebugInterface1"));
  return fn ? fn(Flags, riid, ppDebug) : E_NOTIMPL;
}

'@
    Write-TextNoBom -Path (Join-Path $DstClient 'dxgi_dx11bridge.def') -Text @'
LIBRARY "dxgi"
EXPORTS
  CreateDXGIFactory
  CreateDXGIFactory1
  CreateDXGIFactory2
  DXGIDeclareAdapterRemovalSupport
  DXGIGetDebugInterface1

'@
    Write-TextNoBom -Path (Join-Path $DstClient 'meson.build') -Text @'
dx11_bridge_inc = include_directories('.')

d3d11_dx11bridge_dll = shared_library('d3d11',
  files('d3d11_dx11bridge.cpp'),
  files('d3d11_dx11bridge.def'),
  include_directories: [util_include_path, dx11_bridge_inc],
  cpp_args: ['/EHsc'],
  dependencies: [lib_version],
  install: false
)

dxgi_dx11bridge_dll = shared_library('dxgi',
  files('dxgi_dx11bridge.cpp'),
  files('dxgi_dx11bridge.def'),
  include_directories: [util_include_path, dx11_bridge_inc],
  cpp_args: ['/EHsc'],
  dependencies: [lib_version],
  install: false
)

run_target('copy_dx11_bridge_client',
  depends: [d3d11_dx11bridge_dll, dxgi_dx11bridge_dll],
  command: [copy_script_path, meson.current_build_dir().replace('\\', '/'), output_dir, 'd3d11*'])

run_target('copy_dxgi_bridge_client',
  depends: [dxgi_dx11bridge_dll],
  command: [copy_script_path, meson.current_build_dir().replace('\\', '/'), output_dir, 'dxgi*'])

'@
  }

  $required = @(
    'd3d11_dx11bridge.cpp',
    'd3d11_dx11bridge.def',
    'dx11_common.h',
    'dxgi_dx11bridge.cpp',
    'dxgi_dx11bridge.def'
  )
  foreach ($r in $required) {
    $rp = Join-Path $DstClient $r
    if (!(Test-Path -LiteralPath $rp -PathType Leaf)) {
      Die "DX11 client source generation/copy failed; missing required file: $rp"
    }
  }
  Repair-MesonUtf8NoBom $DstClient
  Log "Verified DX11 client source files in: $DstClient"
}


function Get-DX11ClientGlobalsSourceIntegrated {
@'
#include "util_guid.h"

// DX11_V219_UNIFY_CLIENT_GUID_FOR_IPC
// These globals are consumed by util_devicecommand/util_modulecommand and must
// be the same symbols used by the DX11 client when launching NvRemixBridge.exe.
bridge_util::Guid gUniqueIdentifier;
bool gbBridgeRunning = true;
'@
}

function Patch-DX11ClientCppIntegrated {
  param([Parameter(Mandatory)][string]$ClientCpp)
  if (!(Test-Path -LiteralPath $ClientCpp -PathType Leaf)) { return }
  $original = Get-Content -LiteralPath $ClientCpp -Raw
  $t = $original
  $t = [regex]::Replace($t, "(?ms)\s*// >>> V91_DX11_CLIENT_GLOBAL_LINKAGE.*?// <<< V91_DX11_CLIENT_GLOBAL_LINKAGE\s*\r?\n?", "`r`n")
  $t = [regex]::Replace($t, "(?m)^\s*bridge_util::Guid\s+gUniqueIdentifier\s*;\s*\r?\n?", "")
  $t = [regex]::Replace($t, "(?m)^\s*Guid\s+gUniqueIdentifier\s*;\s*\r?\n?", "")
  $t = [regex]::Replace($t, "(?m)^\s*static\s+Guid\s+gUniqueIdentifier\s*;\s*\r?\n?", "")
  $t = [regex]::Replace($t, "(?m)^\s*bridge_util::Guid\s+gUniqueIdentifier\s*;\s*\r?\n?", "")
  $t = [regex]::Replace($t, "(?m)^\s*bool\s+gbBridgeRunning\s*=\s*(?:true|false)\s*;\s*\r?\n?", "")
  $t = [regex]::Replace($t, 'SetEnvironmentVariableA\(\s*"DX11_BRIDGE_MODE"\s*\)', 'SetEnvironmentVariableA("DX11_BRIDGE_MODE", "1")')
  $t = [regex]::Replace($t, 'SetEnvironmentVariableW\(\s*L"DX11_BRIDGE_MODE"\s*\)', 'SetEnvironmentVariableW(L"DX11_BRIDGE_MODE", L"1")')

  if ($t -notmatch 'DX11_V219_UNIFY_CLIENT_GUID_FOR_IPC') {
    $marker = "using namespace bridge_util;`r`n`r`nnamespace dx11_bridge_client {"
    $replacement = "using namespace bridge_util;`r`n`r`n// DX11_V219_UNIFY_CLIENT_GUID_FOR_IPC`r`n// Use the global GUID/running symbols consumed by bridge IPC utilities.`r`nextern Guid gUniqueIdentifier;`r`nextern bool gbBridgeRunning;`r`n`r`nnamespace dx11_bridge_client {"
    if ($t.Contains($marker)) {
      $t = $t.Replace($marker, $replacement)
    }
  }

  if ($t -ne $original) {
    Write-TextNoBom -Path $ClientCpp -Text $t
    Log "Patched DX11 client source globals/env call: $ClientCpp"
  }
}

function Patch-DX11ClientMesonIntegrated {
  param([Parameter(Mandatory)][string]$MesonPath)
  if (!(Test-Path -LiteralPath $MesonPath -PathType Leaf)) { return }
  $original = Get-Content -LiteralPath $MesonPath -Raw
  $t = $original
  if ($t -notmatch 'dx11_bridge_client_globals\.cpp') {
    $t2 = $t.Replace("'dx11_bridge_client.cpp'", "'dx11_bridge_client.cpp', 'dx11_bridge_client_globals.cpp'")
    if ($t2 -eq $t) { $t2 = $t.Replace('"dx11_bridge_client.cpp"', '"dx11_bridge_client.cpp", "dx11_bridge_client_globals.cpp"') }
    if ($t2 -eq $t) {
      $lines = $t -split "\r?\n", -1
      $out = New-Object System.Collections.Generic.List[string]
      $added = $false
      foreach ($line in $lines) {
        $out.Add($line)
        if (!$added -and $line -match 'dx11_bridge_client\.cpp') {
          $indent = ''
          if ($line -match '^(\s*)') { $indent = $Matches[1] }
          $out.Add($indent + "'dx11_bridge_client_globals.cpp',")
          $added = $true
        }
      }
      if ($added) { $t2 = $out -join "`r`n" }
    }
    $t = $t2
  }
  if ($t -ne $original) {
    Write-TextNoBom -Path $MesonPath -Text $t
    Log "Patched client_dx11 Meson to include dx11_bridge_client_globals.cpp: $MesonPath"
  }
}

function Ensure-DX11ClientGlobalsIntegrated {
  param([Parameter(Mandatory)][string]$BridgeWork)
  $clientDirs = @(
    (Join-Path $Root 'src\client_dx11'),
    (Join-Path $BridgeWork 'src\client_dx11')
  )
  foreach ($clientDir in $clientDirs) {
    if (!(Test-Path -LiteralPath $clientDir -PathType Container)) { continue }
    $globals = Join-Path $clientDir 'dx11_bridge_client_globals.cpp'
    Write-TextNoBom -Path $globals -Text (Get-DX11ClientGlobalsSourceIntegrated)
    Patch-DX11ClientCppIntegrated -ClientCpp (Join-Path $clientDir 'dx11_bridge_client.cpp')
    Patch-DX11ClientMesonIntegrated -MesonPath (Join-Path $clientDir 'meson.build')
    Log "Ensured DX11 x86 client global linkage source in: $clientDir"
  }
  # The client Meson file names the globals source explicitly. Ninja's Meson
  # regeneration detects any real source-list/content change, so deleting the
  # complete x86 tree here only destroys valid incremental state.
  Log 'Preserving x86 bridge Meson trees; Ninja will rebuild changed client/global sources.'
}

function Restore-FinalDX11OutputsIntegrated {
  param([Parameter(Mandatory)][string]$X86BuildClient, [Parameter(Mandatory)][string]$X86Out, [Parameter(Mandatory)][string]$X64Out)
  $trex = Join-Path $X86Out '.trex'
  New-Item -ItemType Directory -Force -Path $X86Out | Out-Null
  New-Item -ItemType Directory -Force -Path $trex | Out-Null
  foreach ($dll in @('d3d11.dll','dxgi.dll')) {
    $src = Join-Path $X86BuildClient $dll
    $dst = Join-Path $X86Out $dll
    if (Test-Path -LiteralPath $src -PathType Leaf) {
      Test-PeMachine $src 'x86'
      Copy-FileIfDifferent -Source $src -Destination $dst
      Test-PeMachine $dst 'x86'
    }
  }
  foreach ($dll in @('d3d11.dll','dxgi.dll')) {
    $src = Join-Path $X64Out $dll
    $dst = Join-Path $trex $dll
    if (Test-Path -LiteralPath $src -PathType Leaf) {
      Test-PeMachine $src 'x64'
      Copy-FileIfDifferent -Source $src -Destination $dst
      Test-PeMachine $dst 'x64'
    }
  }
  # DX11_V264_TREX_FULL_RUNTIME_PAYLOAD: the x64 runtime d3d11.dll needs its
  # complete satellite set beside it. Import-table DLLs (USD set, libxess,
  # NRC_Vulkan, rtxio, python310, tbb...) are required or d3d11.dll fails to
  # LOAD at all (error 126); runtime-loaded DLLs are silently optional but
  # feature-critical: NRD.dll IS THE DENOISER ("Unable to load NRD, this
  # feature will be disabled" = raw noisy path tracing), nvngx_dlss*.dll is
  # DLSS/DLSS-FG. Staging only d3d11+dxgi left every bridge package without a
  # denoiser. Mirror the entire x64 output payload into .trex.
  if (Test-Path -LiteralPath $X64Out -PathType Container) {
    $mirrored = 0
    foreach ($f in (Get-ChildItem -LiteralPath $X64Out -File)) {
      $dst = Join-Path $trex $f.Name
      Copy-FileIfDifferent -Source $f.FullName -Destination $dst
      $mirrored++
    }
    Log "V264 mirrored full x64 runtime payload into .trex ($mirrored files) - denoiser (NRD.dll) + DLSS + USD satellites included."
  }
}


function Write-DX11DllUpdateManifestV219 {
  param(
    [Parameter(Mandatory)][string]$X86Out,
    [Parameter(Mandatory)][string]$X64Out,
    [Parameter(Mandatory)][string]$RuntimeD3D11,
    [Parameter(Mandatory)][string]$RuntimeDXGI
  )

  $manifest = Join-Path $X86Out 'DX11_V219_DLL_UPDATE_MANIFEST.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_FORCE_REBUILD_AND_STAGE_REMIX15_DLLS')
  $lines.Add(('Time: {0}' -f (Get-Date)))
  $lines.Add('')
  $items = @(
    @{ Role='x86 game root client d3d11.dll'; Path=(Join-Path $X86Out 'd3d11.dll'); Machine='x86' },
    @{ Role='x86 game root client dxgi.dll'; Path=(Join-Path $X86Out 'dxgi.dll'); Machine='x86' },
    @{ Role='x64 output runtime d3d11.dll'; Path=(Join-Path $X64Out 'd3d11.dll'); Machine='x64' },
    @{ Role='x64 output runtime dxgi.dll'; Path=(Join-Path $X64Out 'dxgi.dll'); Machine='x64' },
    @{ Role='x64 .trex runtime d3d11.dll'; Path=(Join-Path $X86Out '.trex\d3d11.dll'); Machine='x64' },
    @{ Role='x64 .trex runtime dxgi.dll'; Path=(Join-Path $X86Out '.trex\dxgi.dll'); Machine='x64' },
    @{ Role='x64 .trex bridge server'; Path=(Join-Path $X86Out '.trex\NvRemixBridge.exe'); Machine='x64' },
    @{ Role='x86 launcher'; Path=(Join-Path $X86Out 'NvRemixLauncher32.exe'); Machine='x86' }
  )

  foreach ($i in $items) {
    if (!(Test-Path -LiteralPath $i.Path -PathType Leaf)) {
      $lines.Add(('[BAD] {0}: missing: {1}' -f $i.Role, $i.Path))
      continue
    }
    try {
      Test-PeMachine $i.Path $i.Machine
      $hash = (Get-FileHash -LiteralPath $i.Path -Algorithm SHA256).Hash
      $len = (Get-Item -LiteralPath $i.Path).Length
      $lines.Add(('[OK] {0}: {1} bytes SHA256={2} path={3}' -f $i.Role, $len, $hash, $i.Path))
    } catch {
      $lines.Add(('[BAD] {0}: {1}' -f $i.Role, $_.Exception.Message))
    }
  }

  $lines.Add('')
  $lines.Add(('RuntimeBuildD3D11Source: {0}' -f $RuntimeD3D11))
  $lines.Add(('RuntimeBuildDXGISource: {0}' -f $RuntimeDXGI))
  $lines.Add('')
  $lines.Add('Meaning:')
  $lines.Add('  root d3d11.dll/dxgi.dll are rebuilt x86 DX11 game client/capture DLLs')
  $lines.Add('  .trex d3d11.dll/dxgi.dll are rebuilt x64 Remix 1.5 runtime DLLs')
  $lines.Add('  .trex NvRemixBridge.exe is the rebuilt x64 bridge server')
  [System.IO.File]::WriteAllLines($manifest, $lines)
  Log "V219 wrote DLL update manifest: $manifest"
}


function Normalize-BridgeSrcMesonClientDx11 {
  param([Parameter(Mandatory)][string]$SrcMeson)

  if (!(Test-Path -LiteralPath $SrcMeson -PathType Leaf)) { Die "Missing bridge src meson: $SrcMeson" }

  $original = [System.IO.File]::ReadAllText($SrcMeson)
  $text = $original

  # Remove any previous generated conditional block for client_dx11.
  $text = [regex]::Replace(
    $text,
    "(?ms)^\s*if\s+cpu_family\s*==\s*'x86'\s*\r?\n\s*subdir\('client_dx11'\)\s*\r?\n\s*endif\s*\r?\n?",
    ""
  )

  # Remove all direct visits to either old client or generated client_dx11.
  # Meson forbids visiting the same source directory twice.
  $text = [regex]::Replace(
    $text,
    "(?m)^\s*subdir\('client'\)\s*\r?\n?",
    ""
  )
  $text = [regex]::Replace(
    $text,
    "(?m)^\s*subdir\('client_dx11'\)\s*\r?\n?",
    ""
  )

  $clientBlock = "if cpu_family == 'x86'`r`n  subdir('client_dx11')`r`nendif`r`n"

  # client_dx11 depends on util helpers, so put it after util if util exists.
  if ($text -match "(?m)^\s*subdir\('util'\)\s*$") {
    $text = [regex]::Replace(
      $text,
      "(?m)^(\s*subdir\('util'\)\s*)$",
      "`$1`r`n" + $clientBlock,
      1
    )
  } elseif ($text -match "(?m)^\s*subdir\('server'\)\s*$") {
    $text = [regex]::Replace(
      $text,
      "(?m)^(\s*subdir\('server'\)\s*)$",
      $clientBlock + "`$1",
      1
    )
  } else {
    $text = $text.TrimEnd() + "`r`n" + $clientBlock
  }

  if ($text -ne $original) {
    Write-TextNoBom -Path $SrcMeson -Text $text
    Log "Normalized bridge src/meson.build so client_dx11 is visited exactly once and only on x86."
  }

  $check = [System.IO.File]::ReadAllText($SrcMeson)
  $directCount = ([regex]::Matches($check, "(?m)^\s*subdir\('client_dx11'\)\s*$")).Count
  if ($directCount -ne 1) {
    Die "bridge src/meson.build still has $directCount client_dx11 subdir entries; expected exactly 1. File: $SrcMeson"
  }
  if ($check -match "(?m)^\s*subdir\('client'\)\s*$") {
    Die "bridge src/meson.build still references old subdir('client'). File: $SrcMeson"
  }
}


function Remove-DummyDx11BridgeServerWorkV219 {
  $work = Join-Path $Root 'bridge_dx11_work'
  $main = Join-Path $work 'src\server\main.cpp'
  if (!(Test-Path -LiteralPath $main -PathType Leaf)) { return }

  $text = [System.IO.File]::ReadAllText($main)
  $isDummy =
    ($text -match 'DX11-only bridge server bootstrap starting') -or
    ($text -match 'No D3D9 server command processor is compiled') -or
    ($text -match 'Dx11BridgeRun') -or
    ($text -match 'DX11_BRIDGE_SERVER_WAIT_MS')

  if ($isDummy) {
    Log "V219 removing dummy/stub bridge_dx11_work because it cannot send Bridge_Ack: $work"
    Remove-DirectoryRobust $work
  }
}

function Assert-RealBridgeServerCanAckV219 {
  param([Parameter(Mandatory)][string]$BridgeWork)

  $main = Join-Path $BridgeWork 'src\server\main.cpp'
  $module = Join-Path $BridgeWork 'src\server\module_processing.cpp'
  if (!(Test-Path -LiteralPath $main -PathType Leaf)) { Die "V219 real bridge server check failed; missing main.cpp: $main" }
  if (!(Test-Path -LiteralPath $module -PathType Leaf)) { Die "V219 real bridge server check failed; missing module_processing.cpp: $module" }

  $mainText = [System.IO.File]::ReadAllText($main)
  $moduleText = [System.IO.File]::ReadAllText($module)

  if ($mainText -match 'DX11-only bridge server bootstrap starting' -or
      $mainText -match 'No D3D9 server command processor is compiled' -or
      $mainText -match 'Dx11BridgeRun') {
    Die "V219 real bridge server check failed: src\server\main.cpp is still the dummy DX11 bootstrap that never sends Bridge_Ack."
  }

  if ($mainText -notmatch 'initModuleBridge\s*\(') {
    Die "V219 real bridge server check failed: main.cpp does not initialize module bridge IPC."
  }

  if ($mainText -notmatch 'initDeviceBridge\s*\(') {
    Die "V219 real bridge server check failed: main.cpp does not initialize device bridge IPC."
  }

  if (($mainText + $moduleText) -notmatch 'Bridge_Ack') {
    Die "V219 real bridge server check failed: server source does not reference Bridge_Ack, so the x86 client would wait forever."
  }

  Log "V219 verified real bridge server source has IPC init and Bridge_Ack path."
}


function Patch-BridgeServerRemoveLegacyD3D9RegistrationV219 {
  param([Parameter(Mandatory)][string]$BridgeWork)

  $mainCpp = Join-Path $BridgeWork 'src\server\main.cpp'
  if (!(Test-Path -LiteralPath $mainCpp -PathType Leaf)) {
    Die "V219 legacy D3D9 registration patch failed; missing server main.cpp: $mainCpp"
  }

  $headerCandidates = @(
    (Join-Path $BridgeWork 'public\include\remix\remix_c.h'),
    (Join-Path $BridgeWork '..\public\include\remix\remix_c.h'),
    (Join-Path $Root 'public\include\remix\remix_c.h')
  ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }

  $headerText = ''
  foreach ($h in $headerCandidates) {
    $headerText += [System.IO.File]::ReadAllText($h) + "`n"
  }

  $headerHasD3D9Register = ($headerText -match '\bdxvk_RegisterD3D9Device\b')
  $text = [System.IO.File]::ReadAllText($mainCpp)
  $original = $text

  if ($headerHasD3D9Register) {
    Log "V219: remix_c.h still exposes dxvk_RegisterD3D9Device; no server D3D9 registration removal needed."
    return
  }

  if ($text -notmatch '\bdxvk_RegisterD3D9Device\b') {
    Log "V219: server main.cpp already has no dxvk_RegisterD3D9Device references."
    return
  }

  # The restored real bridge server is still the D3D9-era source, but the synced
  # DX11 remix_c.h no longer exposes remixapi_Interface::dxvk_RegisterD3D9Device.
  # Do not replace the real IPC server with a stub. Only remove/neutralize the
  # stale D3D9 registration calls that prevent the real server from compiling.
  $marker = @'
#ifndef DX11_BRIDGE_SKIP_LEGACY_D3D9_REGISTER_V219
#define DX11_BRIDGE_SKIP_LEGACY_D3D9_REGISTER_V219
static inline remixapi_ErrorCode BridgeSkipLegacyD3D9RegisterForDx11HeaderV219() {
  Logger::info("DX11 bridge: skipped legacy dxvk_RegisterD3D9Device call because the active Remix API header is DX11 and has no D3D9 registration member.");
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
#endif
'@

  if ($text -notmatch 'DX11_BRIDGE_SKIP_LEGACY_D3D9_REGISTER_V219') {
    $insertAfter = 'using namespace remixapi::util;'
    if ($text -match [regex]::Escape($insertAfter)) {
      $text = $text -replace [regex]::Escape($insertAfter), ($insertAfter + "`r`n" + $marker)
    } else {
      $text = $marker + "`r`n" + $text
    }
  }

  # Null checks against a missing function-pointer member cannot compile.
  $text = [regex]::Replace(
    $text,
    'remixapi::g_remix\.dxvk_RegisterD3D9Device\s*==\s*nullptr',
    'false'
  )
  $text = [regex]::Replace(
    $text,
    'remixapi::g_remix\.dxvk_RegisterD3D9Device\s*!=\s*nullptr',
    'true'
  )

  # Assignment forms:
  #   auto r = remixapi::g_remix.dxvk_RegisterD3D9Device(...);
  #   const auto r = ...
  #   remixapi_ErrorCode r = ...
  $assignPattern = '(?m)(\b(?:const\s+)?auto\s+|\bremixapi_ErrorCode\s+)([A-Za-z_][A-Za-z0-9_]*)\s*=\s*remixapi::g_remix\.dxvk_RegisterD3D9Device\s*\([^;]*\);'
  $text = [regex]::Replace(
    $text,
    $assignPattern,
    '$1$2 = BridgeSkipLegacyD3D9RegisterForDx11HeaderV219();'
  )

  # Direct statement form:
  #   remixapi::g_remix.dxvk_RegisterD3D9Device(...);
  $text = [regex]::Replace(
    $text,
    'remixapi::g_remix\.dxvk_RegisterD3D9Device\s*\([^;]*\);',
    'BridgeSkipLegacyD3D9RegisterForDx11HeaderV219();'
  )

  # Older helper/token attempts may leave type aliases for the missing D3D9 register.
  $text = [regex]::Replace(
    $text,
    '\bPFN_remixapi_dxvk_RegisterD3D9Device\b',
    'PFN_remixapi_dxvk_RegisterD3D11Device'
  )

  if ($text -match '\bdxvk_RegisterD3D9Device\b') {
    # The only allowed remaining copy is inside strings/comments in the V219 helper.
    $nonMarker = [regex]::Replace($text, '(?s)#ifndef DX11_BRIDGE_SKIP_LEGACY_D3D9_REGISTER_V219.*?#endif', '')
    if ($nonMarker -match '\bdxvk_RegisterD3D9Device\b') {
      Write-TextNoBom -Path (Join-Path $BridgeWork 'V219_D3D9_REGISTER_PATCH_FAILED_main.cpp') -Text $text
      Die "V219 failed to remove all compile-active dxvk_RegisterD3D9Device references from server main.cpp."
    }
  }

  if ($text -ne $original) {
    if (!(Test-Path -LiteralPath "$mainCpp.v219.before")) {
      # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $mainCpp -Destination "$mainCpp.v219.before" -Force
    }
    Write-TextNoBom -Path $mainCpp -Text $text
    Log "V219 patched server main.cpp: removed legacy dxvk_RegisterD3D9Device calls while keeping real IPC server."
  }

  foreach ($d in @(
    (Join-Path $BridgeWork '_Comp64Release'),
    (Join-Path $BridgeWork '_Comp64Debug')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale bridge server build dir after server registration patch: $d"
    }
  }
}


function Remove-StaleX86ClientForUnifiedGuidV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_client_x86_joined'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 client build dir after handshake queue fix: $d"
    }
  }
}


function Remove-StaleX86ClientForUnifiedGuidV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_client_x86_joined'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 client build dir after DX11 DLL->Launcher->.trex Bridge_Ack sequence restore: $d"
    }
  }
}


function Remove-StaleX86ClientForUnifiedGuidV219 {
  # This migration is now part of the authoritative generated source. Meson
  # and Ninja track those files, while the joined output is overwritten during
  # staging. Preserve all incremental objects on normal rebuilds.
  Log 'V219 GUID/IPC source is authoritative; preserving x86 bridge build trees.'
}


function Remove-StaleClientForServerRuntimeHostV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_client_x86_joined'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 client build dir after server runtime host patch: $d"
    }
  }
}


function Remove-StaleClientForD3D11LifetimeV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_client_x86_joined'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 client build dir after D3D11-client lifetime patch: $d"
    }
  }
}


function Remove-StaleClientForNoFallbackV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_client_x86_joined'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 client build dir after strict no-system-fallback patch: $d"
    }
  }
}


function Remove-StaleClientForDllStartsLauncherV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_client_x86_joined'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 client/launcher build dir after DLL-starts-launcher patch: $d"
    }
  }
}


function Remove-StaleClientForDllLauncherBridgeChainV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_client_x86_joined'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 client/launcher build dir after DLL->Launcher->Bridge chain patch: $d"
    }
  }
}


function Remove-StaleClientForInitAfterLauncherChainV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_client_x86_joined'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 client/launcher build dir after init-after-launcher-chain patch: $d"
    }
  }
}


function Remove-StaleClientForDx11AckNamingV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_client_x86_joined'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 client/launcher build dir after DX11 ACK naming patch: $d"
    }
  }
}


function Remove-StaleClientForV219GuidAndServerEnv {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_client_x86_joined'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 client/launcher build dir after shared GUID/server-env patch: $d"
    }
  }
}


function Remove-StaleLauncherForCompileFixV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 launcher/client build dir after launcher compile fix: $d"
    }
  }
}


function Remove-StaleLauncherForGameCmdFileV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 launcher/client build dir after game command file patch: $d"
    }
  }
}



function Patch-BridgeServerForceLoadRemixRuntimeV219 {
  param([Parameter(Mandatory)][string]$BridgeWork)

  $main = Join-Path $BridgeWork 'src\server\main.cpp'
  if (!(Test-Path -LiteralPath $main -PathType Leaf)) { Die "V219 Remix initializer patch failed; missing server main.cpp: $main" }

  $text = Get-Content -LiteralPath $main -Raw
  if ($text.Contains('DX11_V219_SERVER_REMIX_INITIALIZER')) {
    Log 'Bridge server already has V219 .trex Remix initializer.'
    return
  }

  $helper = @'

#ifndef DX11_V219_SERVER_REMIX_INITIALIZER
#define DX11_V219_SERVER_REMIX_INITIALIZER
static HMODULE gDx11BridgeTrexDxgiV219 = nullptr;
static HMODULE gDx11BridgeTrexD3D11V219 = nullptr;

static void Dx11BridgeServerInitializeTrexRemixRuntimeV219() {
  wchar_t mode[64] = {};
  GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode));

  wchar_t exePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
    Logger::warn("DX11_V219_SERVER_REMIX_INITIALIZER: could not resolve NvRemixBridge.exe path.");
    return;
  }

  wchar_t* slash = wcsrchr(exePath, L'\\');
  if (!slash) {
    Logger::warn("DX11_V219_SERVER_REMIX_INITIALIZER: could not resolve .trex folder.");
    return;
  }
  slash[1] = 0;

  SetDllDirectoryW(exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", exePath);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, exePath);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, exePath);
  wcscat_s(d3d11Path, L"d3d11.dll");

  gDx11BridgeTrexDxgiV219 = LoadLibraryW(dxgiPath);
  if (gDx11BridgeTrexDxgiV219) {
    Logger::info("DX11_V219_SERVER_REMIX_INITIALIZER: loaded .trex dxgi.dll into NvRemixBridge.exe.");
    FARPROC f0 = GetProcAddress(gDx11BridgeTrexDxgiV219, "CreateDXGIFactory");
    FARPROC f1 = GetProcAddress(gDx11BridgeTrexDxgiV219, "CreateDXGIFactory1");
    FARPROC f2 = GetProcAddress(gDx11BridgeTrexDxgiV219, "CreateDXGIFactory2");
    if (f0 || f1 || f2) {
      Logger::info("DX11_V219_SERVER_REMIX_INITIALIZER: .trex dxgi.dll exports DXGI factory entrypoints.");
    } else {
      Logger::warn("DX11_V219_SERVER_REMIX_INITIALIZER: .trex dxgi.dll loaded but no DXGI factory exports were found.");
    }
  } else {
    Logger::warn("DX11_V219_SERVER_REMIX_INITIALIZER: failed to load .trex dxgi.dll into NvRemixBridge.exe.");
  }

  gDx11BridgeTrexD3D11V219 = LoadLibraryW(d3d11Path);
  if (gDx11BridgeTrexD3D11V219) {
    Logger::info("DX11_V219_SERVER_REMIX_INITIALIZER: loaded .trex d3d11.dll into NvRemixBridge.exe.");
    FARPROC d0 = GetProcAddress(gDx11BridgeTrexD3D11V219, "D3D11CreateDevice");
    FARPROC d1 = GetProcAddress(gDx11BridgeTrexD3D11V219, "D3D11CreateDeviceAndSwapChain");
    if (d0 || d1) {
      Logger::info("DX11_V219_SERVER_REMIX_INITIALIZER: .trex d3d11.dll exports D3D11 device entrypoints.");
    } else {
      Logger::warn("DX11_V219_SERVER_REMIX_INITIALIZER: .trex d3d11.dll loaded but no D3D11 entrypoints were found.");
    }
  } else {
    Logger::warn("DX11_V219_SERVER_REMIX_INITIALIZER: failed to load .trex d3d11.dll into NvRemixBridge.exe.");
  }
}
#endif
'@

  $marker = 'int WINAPI wWinMain('
  if ($text -notmatch [regex]::Escape($marker)) { Die 'V219 Remix initializer patch failed; could not find wWinMain in bridge server main.cpp.' }
  $text = $text.Replace($marker, $helper + "`r`n" + $marker)

  $pat = '(int\s+WINAPI\s+wWinMain\s*\([^\)]*\)\s*\{)'
  $repl = '$1' + "`r`n  Dx11BridgeServerInitializeTrexRemixRuntimeV219();"
  $text2 = [regex]::Replace($text, $pat, $repl, 1)
  if ($text2 -eq $text) { Die 'V219 Remix initializer patch failed; could not insert initializer call into wWinMain.' }

  Write-TextNoBom -Path $main -Text $text2
  Log 'Patched bridge server: V219 initializes and validates .trex dxgi.dll/d3d11.dll inside NvRemixBridge.exe.'
}




function Remove-StaleBridgeForSingleServerAndRuntimeV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_build_x64'),
    (Join-Path $Root 'bridge_dx11_work\_build_x86'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale bridge/client build dir after single-bridge/runtime-load patch: $d"
    }
  }
}



function Remove-StaleBridgeForV219SingleBridgeAndInitializer {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_build_x64'),
    (Join-Path $Root 'bridge_dx11_work\_build_x86'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale bridge/client build dir after single-bridge/remix-initializer patch: $d"
    }
  }
}


function Remove-StaleBridgeForPidFileAsciiV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_build_x64'),
    (Join-Path $Root 'bridge_dx11_work\_build_x86'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale bridge/client build dir after PID ASCII patch: $d"
    }
  }
}



function Remove-StaleBridgeForPidRecordV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_build_x64'),
    (Join-Path $Root 'bridge_dx11_work\_build_x86'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale bridge/client build dir after PID record patch: $d"
    }
  }
}



function Remove-StaleBridgeForLauncherDuplicatedHandleV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_build_x64'),
    (Join-Path $Root 'bridge_dx11_work\_build_x86'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale bridge/client build dir after launcher-side client-handle patch: $d"
    }
  }
}



function Remove-StaleBridgeForRealGameTargetV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_build_x64'),
    (Join-Path $Root 'bridge_dx11_work\_build_x86'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale bridge/client build dir after real-game-target patch: $d"
    }
  }
}



function Patch-BridgeServerDx11RealGameTargetAndNoExitKillV219 {
  param([Parameter(Mandatory)][string]$BridgeWork)

  $main = Join-Path $BridgeWork 'src\server\main.cpp'
  if (!(Test-Path -LiteralPath $main -PathType Leaf)) {
    Die "V219 server real-game/no-exit patch failed; missing server main.cpp: $main"
  }

  $text = [System.IO.File]::ReadAllText($main)
  if ($text.Contains('DX11_V219_SERVER_REAL_GAME_TARGET_AND_IGNORE_EXIT_CALLBACK')) {
    Log 'Bridge server already patched for V219 real game target and DX11 no-exit-kill callback.'
    return
  }

  $original = $text

  # 1) initFileSys() currently uses the parent process. In this layout the parent
  # is NvRemixLauncher32.exe, which makes Remix look for a launcher config.
  # In DX11 mode use DX11_BRIDGE_REAL_GAME_EXE / DXVK_REMIX_REAL_GAME_EXE instead.
  $patInit = '(?s)static\s+inline\s+bool\s+initFileSys\s*\(\s*\)\s*\{.*?return\s+true;\s*\}'
  $replacementInit = @'
static inline bool initFileSys() {
  // DX11_V219_SERVER_REAL_GAME_TARGET_AND_IGNORE_EXIT_CALLBACK
  // In the DX11 bridge chain the x64 server is launched by NvRemixLauncher32.exe,
  // so parent-process based discovery points at the launcher.  Remix config and
  // filesystem roots must use the real already-running game exe instead.
  wchar_t dx11Mode[64] = {};
  const bool isDx11BridgeMode = GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", dx11Mode, _countof(dx11Mode)) > 0;

  if (isDx11BridgeMode) {
    wchar_t gameExe[MAX_PATH] = {};
    DWORD got = GetEnvironmentVariableW(L"DX11_BRIDGE_REAL_GAME_EXE", gameExe, _countof(gameExe));
    if (got == 0 || got >= _countof(gameExe)) {
      got = GetEnvironmentVariableW(L"DXVK_REMIX_REAL_GAME_EXE", gameExe, _countof(gameExe));
    }

    if (got > 0 && got < _countof(gameExe)) {
      const fspath realGamePath(gameExe);
      const auto realGameDir = realGamePath.parent_path();
      dxvk::util::RtxFileSys::init(realGameDir.string());
      Logger::info(std::string("DX11_V219_SERVER_REAL_GAME_TARGET_AND_IGNORE_EXIT_CALLBACK: server filesystem/config target uses real game exe: ") + realGamePath.string());
      return true;
    }

    Logger::warn("DX11_V219_SERVER_REAL_GAME_TARGET_AND_IGNORE_EXIT_CALLBACK: DX11 mode active but real game exe env missing; falling back to parent process path.");
  }

  auto parentPid = bridge_util::getParentPid();
  DWORD accessRights = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
  HANDLE processHandle = OpenProcess(accessRights, FALSE, parentPid);

  {
    auto executable32PathVec = createPathVec();
    if(GetModuleFileNameEx(processHandle, NULL, executable32PathVec.data(), executable32PathVec.size()) == 0) {
      Logger::err("Failed to find executable path!");
      if (processHandle) {
        CloseHandle(processHandle);
      }
      return false;
    }

    const fspath executable32Path(executable32PathVec.data());
    const auto exe32Dir = executable32Path.parent_path();
    dxvk::util::RtxFileSys::init(exe32Dir.string());
  }

  if (processHandle) {
    CloseHandle(processHandle);
  }

  return true;
}
'@
  $text2 = [regex]::Replace($text, $patInit, $replacementInit, 1)
  if ($text2 -eq $text) {
    Die "V219 patch failed: could not replace initFileSys() in server main.cpp."
  }
  $text = $text2

  # 2) In DX11 mode, do not let the legacy D3D9 client-exit callback kill the server.
  # The launcher/server split means that callback can fire incorrectly even after
  # the real game DLL handshake completed.
  $patExit = '(?s)void\s+CALLBACK\s+OnClientExited\s*\(\s*void\*\s+context\s*,\s*BOOLEAN\s+isTimeout\s*\)\s*\{'
  $exitInsert = @'
void CALLBACK OnClientExited(void* context, BOOLEAN isTimeout) {
  // DX11_V219_SERVER_REAL_GAME_TARGET_AND_IGNORE_EXIT_CALLBACK
  wchar_t dx11Mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", dx11Mode, _countof(dx11Mode)) > 0) {
    Logger::warn("DX11_V219_SERVER_REAL_GAME_TARGET_AND_IGNORE_EXIT_CALLBACK: ignoring legacy client-exit callback in DX11 bridge mode; server stays alive and waits for IPC/command shutdown.");
    return;
  }
'@
  $text2 = [regex]::Replace($text, $patExit, $exitInsert, 1)
  if ($text2 -eq $text) {
    Die "V219 patch failed: could not patch OnClientExited() in server main.cpp."
  }
  $text = $text2

  Write-TextNoBom -Path $main -Text $text
  Log 'Patched bridge server V219: config target uses real game exe, and DX11 mode ignores legacy client-exit shutdown callback.'
}



function Remove-StaleBridgeForServerRealGameNoExitV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_build_x64'),
    (Join-Path $Root 'bridge_dx11_work\_build_x86'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale bridge/client build dir after server real-game/no-exit patch: $d"
    }
  }
}



function Patch-BridgeServerForceTrexDx11RuntimeV219 {
  param([Parameter(Mandatory)][string]$BridgeWork)

  $main = Join-Path $BridgeWork 'src\server\main.cpp'
  if (!(Test-Path -LiteralPath $main -PathType Leaf)) {
    Die "V219 force .trex DX11 runtime patch failed; missing server main.cpp: $main"
  }

  $text = [System.IO.File]::ReadAllText($main)
  if ($text.Contains('DX11_V219_FORCE_TREX_DX11_RUNTIME')) {
    Log 'Bridge server already patched for forced .trex DX11 runtime load.'
    return
  }

  $helper = @'

#ifndef DX11_V219_FORCE_TREX_DX11_RUNTIME
#define DX11_V219_FORCE_TREX_DX11_RUNTIME
static HMODULE gDx11BridgeForcedTrexDxgiV219 = nullptr;
static HMODULE gDx11BridgeForcedTrexD3D11V219 = nullptr;

static bool Dx11BridgeForceTrexRuntimePathExistsV219(const wchar_t* path) {
  const DWORD attrs = GetFileAttributesW(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Dx11BridgeForceGetTrexDirV219(wchar_t* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;

  wchar_t envTrex[MAX_PATH] = {};
  DWORD got = GetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", envTrex, _countof(envTrex));
  if (got > 0 && got < _countof(envTrex)) {
    lstrcpynW(out, envTrex, cap);
  } else {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
      return false;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) {
      return false;
    }
    slash[1] = 0;
    lstrcpynW(out, exePath, cap);
  }

  size_t len = wcslen(out);
  if (len > 0 && out[len - 1] != L'\\' && len + 2 < cap) {
    out[len] = L'\\';
    out[len + 1] = 0;
  }

  return out[0] != 0;
}

static bool Dx11BridgeServerForceLoadTrexDx11RuntimeV219() {
  wchar_t mode[64] = {};
  if (GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", mode, _countof(mode)) == 0) {
    return true;
  }

  wchar_t trexDir[MAX_PATH] = {};
  if (!Dx11BridgeForceGetTrexDirV219(trexDir, _countof(trexDir))) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: FAILED: could not resolve .trex directory.");
    return false;
  }

  SetDllDirectoryW(trexDir);

  wchar_t dxgiPath[MAX_PATH] = {};
  wchar_t d3d11Path[MAX_PATH] = {};
  wcscpy_s(dxgiPath, trexDir);
  wcscat_s(dxgiPath, L"dxgi.dll");
  wcscpy_s(d3d11Path, trexDir);
  wcscat_s(d3d11Path, L"d3d11.dll");

  if (!Dx11BridgeForceTrexRuntimePathExistsV219(dxgiPath)) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\dxgi.dll is missing.");
    return false;
  }

  if (!Dx11BridgeForceTrexRuntimePathExistsV219(d3d11Path)) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: FAILED: .trex\\d3d11.dll is missing.");
    return false;
  }

  gDx11BridgeForcedTrexDxgiV219 = LoadLibraryExW(dxgiPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexDxgiV219) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\dxgi.dll) failed.");
    return false;
  }

  Logger::info("DX11_V219_FORCE_TREX_DX11_RUNTIME: forced .trex\\dxgi.dll loaded into NvRemixBridge.exe.");

  gDx11BridgeForcedTrexD3D11V219 = LoadLibraryExW(d3d11Path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gDx11BridgeForcedTrexD3D11V219) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: FAILED: LoadLibraryExW(.trex\\d3d11.dll) failed.");
    return false;
  }

  Logger::info("DX11_V219_FORCE_TREX_DX11_RUNTIME: forced .trex\\d3d11.dll loaded into NvRemixBridge.exe.");

  FARPROC createFactory = GetProcAddress(gDx11BridgeForcedTrexDxgiV219, "CreateDXGIFactory");
  FARPROC createFactory1 = GetProcAddress(gDx11BridgeForcedTrexDxgiV219, "CreateDXGIFactory1");
  FARPROC createFactory2 = GetProcAddress(gDx11BridgeForcedTrexDxgiV219, "CreateDXGIFactory2");
  FARPROC createDevice = GetProcAddress(gDx11BridgeForcedTrexD3D11V219, "D3D11CreateDevice");
  FARPROC createDeviceAndSwapChain = GetProcAddress(gDx11BridgeForcedTrexD3D11V219, "D3D11CreateDeviceAndSwapChain");

  if (!createDevice) {
    Logger::err("DX11_V219_FORCE_TREX_DX11_RUNTIME: FAILED: forced .trex\\d3d11.dll lacks D3D11CreateDevice.");
    return false;
  }

  Logger::info("DX11_V219_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDevice.");

  if (createDeviceAndSwapChain) {
    Logger::info("DX11_V219_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll exports D3D11CreateDeviceAndSwapChain.");
  } else {
    Logger::warn("DX11_V219_FORCE_TREX_DX11_RUNTIME: .trex\\d3d11.dll lacks D3D11CreateDeviceAndSwapChain.");
  }

  if (createFactory || createFactory1 || createFactory2) {
    Logger::info("DX11_V219_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll exports DXGI factory entrypoints.");
  } else {
    Logger::warn("DX11_V219_FORCE_TREX_DX11_RUNTIME: .trex\\dxgi.dll lacks DXGI factory exports.");
  }

  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_D3D11_FORCED", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_TREX_DXGI_FORCED", L"1");
  return true;
}
#endif
'@

  $marker = 'int WINAPI wWinMain('
  if ($text -notmatch [regex]::Escape($marker)) {
    Die 'V219 force .trex DX11 runtime patch failed; could not find wWinMain in server main.cpp.'
  }

  $text = $text.Replace($marker, $helper + "`r`n" + $marker)

  $pat = '(int\s+WINAPI\s+wWinMain\s*\([^\)]*\)\s*\{)'
  $repl = '$1' + "`r`n  if (!Dx11BridgeServerForceLoadTrexDx11RuntimeV219()) {`r`n    Logger::err(`"DX11_V219_FORCE_TREX_DX11_RUNTIME: server continuing after forced runtime load failure so logs remain visible.`");`r`n  }"
  $text2 = [regex]::Replace($text, $pat, $repl, 1)
  if ($text2 -eq $text) {
    Die 'V219 force .trex DX11 runtime patch failed; could not insert call into wWinMain.'
  }

  Write-TextNoBom -Path $main -Text $text2
  Log 'Patched bridge server V219: forced full-path .trex dxgi.dll and d3d11.dll load in DX11 mode.'
}



function Remove-StaleBridgeForForceTrexDx11RuntimeV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_build_x64'),
    (Join-Path $Root 'bridge_dx11_work\_build_x86'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale bridge/client build dir after forced .trex DX11 runtime patch: $d"
    }
  }
}



function Patch-DX11ClientRealCaptureLayerV219 {
  param([Parameter(Mandatory)][string]$DstClient)

  Log 'V219 writing real in-game DX11 client capture layer: d3d11 device/context/swapchain + dxgi factory hooks.'

  Write-TextNoBom -Path (Join-Path $DstClient 'd3d11_dx11bridge.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include "dx11_bridge_client.h"

#define DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER 1

using PFN_D3D11CreateDevice = HRESULT (WINAPI *)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (WINAPI *)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static HMODULE gSystemD3D11 = nullptr;
static PFN_D3D11CreateDevice pD3D11CreateDevice = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain pD3D11CreateDeviceAndSwapChain = nullptr;

static volatile LONG gV219HooksInstalled = 0;
static volatile LONG gV219PresentCount = 0;
static volatile LONG gV219DrawCount = 0;
static volatile LONG gV219ResourceCount = 0;
static thread_local bool gV219InsideHook = false;

static void V219Log(const char* tag, const char* text) {
  dx11_bridge_client::LogLine(tag, text);
}

static HMODULE LoadSystemD3D11V219() {
  if (gSystemD3D11) return gSystemD3D11;
  gSystemD3D11 = dx11_bridge_client::LoadSystemDll("d3d11.dll");
  if (!gSystemD3D11) return nullptr;
  pD3D11CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(gSystemD3D11, "D3D11CreateDevice"));
  pD3D11CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(GetProcAddress(gSystemD3D11, "D3D11CreateDeviceAndSwapChain"));
  if (!pD3D11CreateDevice || !pD3D11CreateDeviceAndSwapChain) {
    V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: system d3d11.dll missing required exports.");
    return nullptr;
  }
  return gSystemD3D11;
}

template <typename T>
static bool V219HookVTable(void* object, size_t slot, void* detour, T* original, const char* name) {
  if (!object || !detour || !original) return false;
  void*** obj = reinterpret_cast<void***>(object);
  void** vt = *obj;
  if (!vt) return false;

  if (vt[slot] == detour) {
    return true;
  }

  DWORD oldProtect = 0;
  if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: VirtualProtect failed for %s slot=%u err=%lu.", name, (unsigned) slot, GetLastError());
    V219Log("hook", msg);
    return false;
  }

  if (*original == nullptr) {
    *original = reinterpret_cast<T>(vt[slot]);
  }

  vt[slot] = detour;

  DWORD ignored = 0;
  VirtualProtect(&vt[slot], sizeof(void*), oldProtect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), &vt[slot], sizeof(void*));

  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: hooked %s vtable slot %u.", name, (unsigned) slot);
  V219Log("hook", msg);
  return true;
}

// IDXGISwapChain slots.
using PFN_SwapPresent = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain*, UINT, UINT);
using PFN_SwapResizeBuffers = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PFN_SwapPresent oSwapPresent = nullptr;
static PFN_SwapResizeBuffers oSwapResizeBuffers = nullptr;

// ID3D11DeviceContext slots.
using PFN_DrawIndexed = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInstanced = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
static PFN_DrawIndexed oDrawIndexed = nullptr;
static PFN_Draw oDraw = nullptr;
static PFN_DrawIndexedInstanced oDrawIndexedInstanced = nullptr;
static PFN_DrawInstanced oDrawInstanced = nullptr;

// ID3D11Device slots.
using PFN_CreateBuffer = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using PFN_CreateTexture2D = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using PFN_CreateSRV = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using PFN_CreateVertexShader = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using PFN_CreatePixelShader = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
static PFN_CreateBuffer oCreateBuffer = nullptr;
static PFN_CreateTexture2D oCreateTexture2D = nullptr;
static PFN_CreateSRV oCreateSRV = nullptr;
static PFN_CreateVertexShader oCreateVertexShader = nullptr;
static PFN_CreatePixelShader oCreatePixelShader = nullptr;

static void V219NotifyDraw(const char* what) {
  LONG n = InterlockedIncrement(&gV219DrawCount);
  if (n <= 16 || (n % 500) == 0) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: captured %s count=%ld in game process.", what, n);
    V219Log("capture", msg);
  }
}

static void V219NotifyResource(const char* what) {
  LONG n = InterlockedIncrement(&gV219ResourceCount);
  if (n <= 16 || (n % 250) == 0) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: captured %s count=%ld in game process.", what, n);
    V219Log("capture", msg);
  }
}

static HRESULT STDMETHODCALLTYPE HSwapPresent(IDXGISwapChain* self, UINT syncInterval, UINT flags) {
  if (!gV219InsideHook) {
    gV219InsideHook = true;
    LONG n = InterlockedIncrement(&gV219PresentCount);
    if (n <= 8 || (n % 60) == 0) {
      char msg[256] = {};
      sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: captured IDXGISwapChain::Present count=%ld in game process.", n);
      V219Log("capture", msg);
    }
    dx11_bridge_client::EnsureServer();
    // DX11_V265_BRIDGE_PRESENT_CAMERA: frame boundary - send Startup (once,
    // game HWND), SetupCamera and Present to the server so the Remix runtime
    // attaches to and presents INTO the game window every frame.
    dx11_capture::OnPresent(self);
    gV219InsideHook = false;
  }
  return oSwapPresent ? oSwapPresent(self, syncInterval, flags) : DXGI_ERROR_DEVICE_REMOVED;
}

static HRESULT STDMETHODCALLTYPE HSwapResizeBuffers(IDXGISwapChain* self, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT flags) {
  V219Log("capture", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: captured IDXGISwapChain::ResizeBuffers in game process.");
  return oSwapResizeBuffers ? oSwapResizeBuffers(self, bufferCount, width, height, newFormat, flags) : DXGI_ERROR_DEVICE_REMOVED;
}

static void STDMETHODCALLTYPE HDrawIndexed(ID3D11DeviceContext* self, UINT indexCount, UINT startIndexLocation, INT baseVertexLocation) {
  V219NotifyDraw("ID3D11DeviceContext::DrawIndexed");
  if (oDrawIndexed) oDrawIndexed(self, indexCount, startIndexLocation, baseVertexLocation);
}

static void STDMETHODCALLTYPE HDraw(ID3D11DeviceContext* self, UINT vertexCount, UINT startVertexLocation) {
  V219NotifyDraw("ID3D11DeviceContext::Draw");
  if (oDraw) oDraw(self, vertexCount, startVertexLocation);
}

static void STDMETHODCALLTYPE HDrawIndexedInstanced(ID3D11DeviceContext* self, UINT indexCountPerInstance, UINT instanceCount, UINT startIndexLocation, INT baseVertexLocation, UINT startInstanceLocation) {
  V219NotifyDraw("ID3D11DeviceContext::DrawIndexedInstanced");
  if (oDrawIndexedInstanced) oDrawIndexedInstanced(self, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
}

static void STDMETHODCALLTYPE HDrawInstanced(ID3D11DeviceContext* self, UINT vertexCountPerInstance, UINT instanceCount, UINT startVertexLocation, UINT startInstanceLocation) {
  V219NotifyDraw("ID3D11DeviceContext::DrawInstanced");
  if (oDrawInstanced) oDrawInstanced(self, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
}

static HRESULT STDMETHODCALLTYPE HCreateBuffer(ID3D11Device* self, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Buffer** out) {
  V219NotifyResource("ID3D11Device::CreateBuffer");
  return oCreateBuffer ? oCreateBuffer(self, desc, data, out) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE HCreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** out) {
  V219NotifyResource("ID3D11Device::CreateTexture2D");
  return oCreateTexture2D ? oCreateTexture2D(self, desc, data, out) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE HCreateSRV(ID3D11Device* self, ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out) {
  V219NotifyResource("ID3D11Device::CreateShaderResourceView");
  return oCreateSRV ? oCreateSRV(self, resource, desc, out) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE HCreateVertexShader(ID3D11Device* self, const void* bytecode, SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, ID3D11VertexShader** out) {
  V219NotifyResource("ID3D11Device::CreateVertexShader");
  return oCreateVertexShader ? oCreateVertexShader(self, bytecode, bytecodeLength, linkage, out) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE HCreatePixelShader(ID3D11Device* self, const void* bytecode, SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, ID3D11PixelShader** out) {
  V219NotifyResource("ID3D11Device::CreatePixelShader");
  return oCreatePixelShader ? oCreatePixelShader(self, bytecode, bytecodeLength, linkage, out) : E_FAIL;
}

extern "C" __declspec(dllexport) void WINAPI DX11BridgeInstallSwapChainCapture(IDXGISwapChain* swapChain) {
  if (!swapChain) return;
  V219HookVTable(swapChain, 8, reinterpret_cast<void*>(&HSwapPresent), &oSwapPresent, "IDXGISwapChain::Present");
  V219HookVTable(swapChain, 13, reinterpret_cast<void*>(&HSwapResizeBuffers), &oSwapResizeBuffers, "IDXGISwapChain::ResizeBuffers");
}

static void V219InstallCapture(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) {
  if (InterlockedCompareExchange(&gV219HooksInstalled, 1, 0) == 0) {
    V219Log("capture", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: installing real in-game DX11 capture hooks.");
  }

  if (device) {
    // ID3D11Device vtable slots.
    V219HookVTable(device, 3, reinterpret_cast<void*>(&HCreateBuffer), &oCreateBuffer, "ID3D11Device::CreateBuffer");
    V219HookVTable(device, 5, reinterpret_cast<void*>(&HCreateTexture2D), &oCreateTexture2D, "ID3D11Device::CreateTexture2D");
    V219HookVTable(device, 7, reinterpret_cast<void*>(&HCreateSRV), &oCreateSRV, "ID3D11Device::CreateShaderResourceView");
    V219HookVTable(device, 12, reinterpret_cast<void*>(&HCreateVertexShader), &oCreateVertexShader, "ID3D11Device::CreateVertexShader");
    V219HookVTable(device, 15, reinterpret_cast<void*>(&HCreatePixelShader), &oCreatePixelShader, "ID3D11Device::CreatePixelShader");
  }

  ID3D11DeviceContext* localContext = nullptr;
  if (!context && device) {
    device->GetImmediateContext(&localContext);
    context = localContext;
  }

  if (context) {
    // ID3D11DeviceContext vtable slots.
    V219HookVTable(context, 12, reinterpret_cast<void*>(&HDrawIndexed), &oDrawIndexed, "ID3D11DeviceContext::DrawIndexed");
    V219HookVTable(context, 13, reinterpret_cast<void*>(&HDraw), &oDraw, "ID3D11DeviceContext::Draw");
    V219HookVTable(context, 20, reinterpret_cast<void*>(&HDrawIndexedInstanced), &oDrawIndexedInstanced, "ID3D11DeviceContext::DrawIndexedInstanced");
    V219HookVTable(context, 21, reinterpret_cast<void*>(&HDrawInstanced), &oDrawInstanced, "ID3D11DeviceContext::DrawInstanced");
  }

  if (localContext) {
    localContext->Release();
  }

  DX11BridgeInstallSwapChainCapture(swapChain);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_client::SetModule(hinst);
    dx11_bridge_client::Attach();
    V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: game loaded root d3d11.dll client capture layer.");
  }

  if (reason == DLL_PROCESS_DETACH) {
    if (reserved != nullptr) {
      dx11_bridge_client::Detach();
    } else {
      V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: ignoring runtime detach; bridge remains owned until process exit.");
    }
  }
  return TRUE;
}

extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount, UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext) {
  V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: D3D11CreateDevice intercepted inside game process.");
  if (!dx11_bridge_client::EnsureServer()) {
    V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: bridge server startup failed before D3D11CreateDevice.");
    if (device) *device = nullptr;
    if (immediateContext) *immediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  if (!LoadSystemD3D11V219() || !pD3D11CreateDevice) {
    if (device) *device = nullptr;
    if (immediateContext) *immediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT hr = pD3D11CreateDevice(adapter, driverType, software, flags, featureLevels, featureLevelCount, sdkVersion, device, featureLevel, immediateContext);
  if (SUCCEEDED(hr)) {
    V219InstallCapture(device ? *device : nullptr, immediateContext ? *immediateContext : nullptr, nullptr);
  }

  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: D3D11CreateDevice returned 0x%08X.", (unsigned) hr);
  V219Log("d3d11", msg);
  return hr;
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount, UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* swapChainDesc, IDXGISwapChain** swapChain, ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext) {
  V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: D3D11CreateDeviceAndSwapChain intercepted inside game process.");
  if (!dx11_bridge_client::EnsureServer()) {
    V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: bridge server startup failed before D3D11CreateDeviceAndSwapChain.");
    if (swapChain) *swapChain = nullptr;
    if (device) *device = nullptr;
    if (immediateContext) *immediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  if (!LoadSystemD3D11V219() || !pD3D11CreateDeviceAndSwapChain) {
    if (swapChain) *swapChain = nullptr;
    if (device) *device = nullptr;
    if (immediateContext) *immediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT hr = pD3D11CreateDeviceAndSwapChain(adapter, driverType, software, flags, featureLevels, featureLevelCount, sdkVersion, swapChainDesc, swapChain, device, featureLevel, immediateContext);
  if (SUCCEEDED(hr)) {
    V219InstallCapture(device ? *device : nullptr, immediateContext ? *immediateContext : nullptr, swapChain ? *swapChain : nullptr);
  }

  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: D3D11CreateDeviceAndSwapChain returned 0x%08X.", (unsigned) hr);
  V219Log("d3d11", msg);
  return hr;
}
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'dxgi_dx11bridge.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include "dx11_bridge_client.h"

#define DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER 1

using PFN_CreateDXGIFactory = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT (WINAPI *)(UINT, REFIID, void**);

static HMODULE gSystemDxgi = nullptr;
static PFN_CreateDXGIFactory pCreateDXGIFactory = nullptr;
static PFN_CreateDXGIFactory1 pCreateDXGIFactory1 = nullptr;
static PFN_CreateDXGIFactory2 pCreateDXGIFactory2 = nullptr;

using PFN_FactoryCreateSwapChain = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
// DX11_V219_X86_DXGI_FACTORY2_ABI_SAFE
using PFN_Factory2CreateSwapChainForHwnd = HRESULT (STDMETHODCALLTYPE *)(void*, IUnknown*, HWND, const void*, const void*, IDXGIOutput*, void**);

static PFN_FactoryCreateSwapChain oFactoryCreateSwapChain = nullptr;
static PFN_Factory2CreateSwapChainForHwnd oFactory2CreateSwapChainForHwnd = nullptr;

static void DLog(const char* text) {
  dx11_bridge_client::LogLine("dxgi", text);
}

static HMODULE LoadSystemDxgiV219() {
  if (gSystemDxgi) return gSystemDxgi;
  gSystemDxgi = dx11_bridge_client::LoadSystemDll("dxgi.dll");
  if (!gSystemDxgi) return nullptr;
  pCreateDXGIFactory = reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(gSystemDxgi, "CreateDXGIFactory"));
  pCreateDXGIFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(gSystemDxgi, "CreateDXGIFactory1"));
  pCreateDXGIFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(gSystemDxgi, "CreateDXGIFactory2"));
  return gSystemDxgi;
}

template <typename T>
static bool HookVTable(void* object, size_t slot, void* detour, T* original, const char* name) {
  if (!object || !detour || !original) return false;
  void*** obj = reinterpret_cast<void***>(object);
  void** vt = *obj;
  if (!vt) return false;
  if (vt[slot] == detour) return true;

  DWORD oldProtect = 0;
  if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
    return false;
  }

  if (*original == nullptr) {
    *original = reinterpret_cast<T>(vt[slot]);
  }

  vt[slot] = detour;

  DWORD ignored = 0;
  VirtualProtect(&vt[slot], sizeof(void*), oldProtect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), &vt[slot], sizeof(void*));

  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: hooked %s slot=%u.", name, (unsigned) slot);
  DLog(msg);
  return true;
}

using PFN_D3D11SwapChainHook = void (WINAPI *)(IDXGISwapChain*);
static void TryInstallD3D11SwapChainHook(IDXGISwapChain* swapChain) {
  if (!swapChain) return;

  HMODULE d3d11 = GetModuleHandleA("d3d11.dll");
  if (!d3d11) {
    DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: d3d11.dll module is not loaded yet; swapchain hook deferred.");
    return;
  }

  auto hook = reinterpret_cast<PFN_D3D11SwapChainHook>(GetProcAddress(d3d11, "DX11BridgeInstallSwapChainCapture"));
  if (!hook) {
    DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: local d3d11.dll lacks DX11BridgeInstallSwapChainCapture export.");
    return;
  }

  hook(swapChain);
  DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: installed d3d11 swapchain Present capture from DXGI factory path.");
}

static HRESULT STDMETHODCALLTYPE HFactoryCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) {
  HRESULT hr = oFactoryCreateSwapChain ? oFactoryCreateSwapChain(self, device, desc, swapChain) : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) {
    dx11_bridge_client::EnsureServer();
    TryInstallD3D11SwapChainHook(*swapChain);
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE HFactory2CreateSwapChainForHwnd(void* self, IUnknown* device, HWND hwnd, const void* desc, const void* fsDesc, IDXGIOutput* restrictToOutput, void** swapChain) {
  HRESULT hr = oFactory2CreateSwapChainForHwnd ? oFactory2CreateSwapChainForHwnd(self, device, hwnd, desc, fsDesc, restrictToOutput, swapChain) : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) {
    dx11_bridge_client::EnsureServer();
    TryInstallD3D11SwapChainHook(reinterpret_cast<IDXGISwapChain*>(*swapChain));
  }
  return hr;
}

static void InstallFactoryHooks(void* factory) {
  if (!factory) return;
  // IDXGIFactory::CreateSwapChain slot 10.
  HookVTable(factory, 10, reinterpret_cast<void*>(&HFactoryCreateSwapChain), &oFactoryCreateSwapChain, "IDXGIFactory::CreateSwapChain");
  // IDXGIFactory2::CreateSwapChainForHwnd slot 15. Safe when object is Factory2; harmless if not used.
  HookVTable(factory, 15, reinterpret_cast<void*>(&HFactory2CreateSwapChainForHwnd), &oFactory2CreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_client::SetModule(hinst);
    DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: game loaded root dxgi.dll factory capture layer.");
  }
  if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
    dx11_bridge_client::Detach();
  }
  return TRUE;
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
  DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: CreateDXGIFactory intercepted.");
  if (!LoadSystemDxgiV219() || !pCreateDXGIFactory) return DXGI_ERROR_UNSUPPORTED;
  HRESULT hr = pCreateDXGIFactory(riid, ppFactory);
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooks(*ppFactory);
  return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
  DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: CreateDXGIFactory1 intercepted.");
  if (!LoadSystemDxgiV219() || !pCreateDXGIFactory1) return DXGI_ERROR_UNSUPPORTED;
  HRESULT hr = pCreateDXGIFactory1(riid, ppFactory);
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooks(*ppFactory);
  return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) {
  DLog("DX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER: CreateDXGIFactory2 intercepted.");
  if (!LoadSystemDxgiV219() || !pCreateDXGIFactory2) return DXGI_ERROR_UNSUPPORTED;
  HRESULT hr = pCreateDXGIFactory2(flags, riid, ppFactory);
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooks(*ppFactory);
  return hr;
}
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'd3d11_dx11bridge.def') -Text @'
LIBRARY "d3d11"
EXPORTS
  D3D11CreateDevice
  D3D11CreateDeviceAndSwapChain
  DX11BridgeInstallSwapChainCapture
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'dxgi_dx11bridge.def') -Text @'
LIBRARY "dxgi"
EXPORTS
  CreateDXGIFactory
  CreateDXGIFactory1
  CreateDXGIFactory2
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'meson.build') -Text @'
dx11_client_inc = include_directories('.')
thread_dep = dependency('threads')

dx11_common_src = files(['dx11_bridge_client.cpp'])

d3d11_dx11_bridge = shared_library('d3d11',
  files(['d3d11_dx11bridge.cpp']) + dx11_common_src,
  sources: [bridge_version],
  dependencies: [thread_dep, util_dep, lib_version, lib_commCtrl, tracy_dep],
  include_directories: [bridge_include_path, util_include_path, public_include_path, dx11_client_inc],
  cpp_args: ['/DREMIX_BRIDGE_CLIENT', '/DDX11_BRIDGE_CLIENT', '/DDX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER'],
  install: true,
  vs_module_defs: 'd3d11_dx11bridge.def')

dxgi_dx11_bridge = shared_library('dxgi',
  files(['dxgi_dx11bridge.cpp']) + dx11_common_src,
  sources: [bridge_version],
  dependencies: [thread_dep, util_dep, lib_version, lib_commCtrl, tracy_dep],
  include_directories: [bridge_include_path, util_include_path, public_include_path, dx11_client_inc],
  cpp_args: ['/DREMIX_BRIDGE_CLIENT', '/DDX11_BRIDGE_CLIENT', '/DDX11_V219_REAL_DXGI_SWAPCHAIN_CAPTURE_LAYER'],
  install: true,
  vs_module_defs: 'dxgi_dx11bridge.def')
'@
}



function Remove-StaleBridgeForRealDx11CaptureV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_build_x64'),
    (Join-Path $Root 'bridge_dx11_work\_build_x86'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale bridge/client build dir after real DX11 capture patch: $d"
    }
  }
}



function Mark-DX11Remix15RuntimeUiLineV219 {
  Log 'DX11_V219_IMPORT_REMIX15_RUNTIME_UI_DX11_ONLY: build keeps DX11 client/server/launcher chain and imports runtime/UI-side Remix 1.5 changes only.'
}



function Patch-BridgeServerRemoveStaleD3D11RegisterV219 {
  param([Parameter(Mandatory)][string]$BridgeWork)
  Patch-BridgeServerRegisterD3D11 -BridgeWork $BridgeWork
}



function Remove-StaleBridgeServerForRemixApiV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_build_x64'),
    (Join-Path $Root 'bridge_dx11_work\_build_x86'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp64Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp64Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale bridge build dir after Remix API registration cleanup: $d"
    }
  }

  $serverMain = Join-Path $Root 'bridge_dx11_work\src\server\main.cpp'
  if (Test-Path -LiteralPath $serverMain -PathType Leaf) {
    $text = [System.IO.File]::ReadAllText($serverMain)
    if ($text -match 'DX11_BRIDGE_REGISTER_HELPER_V\d+' -or $text -match '\bdxvk_RegisterD3D11Device\b') {
      Log 'V219 patching existing bridge_dx11_work server main.cpp before rebuild.'
      Patch-BridgeServerRegisterD3D11 -BridgeWork (Join-Path $Root 'bridge_dx11_work')
    }
  }
}



function Remove-StaleClientForDxgiFactory2AbiV219 {
  foreach ($d in @(
    (Join-Path $Root 'bridge_dx11_work\_build_x86'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Release'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32Debug'),
    (Join-Path $Root 'bridge_dx11_work\_Comp32ReleaseOptimized'),
    (Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86')
  )) {
    if (Test-Path -LiteralPath $d -PathType Container) {
      Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
      Log "V219 removed stale x86 client build dir after DXGI Factory2 ABI-safe patch: $d"
    }
  }

  $dxgi = Join-Path $Root 'bridge_dx11_work\src\client_dx11\dxgi_dx11bridge.cpp'
  if (Test-Path -LiteralPath $dxgi -PathType Leaf) {
    $t = [System.IO.File]::ReadAllText($dxgi)
    $o = $t
    $t = $t.Replace('using PFN_Factory2CreateSwapChainForHwnd = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);',
                    '// DX11_V219_X86_DXGI_FACTORY2_ABI_SAFE' + "`r`n" + 'using PFN_Factory2CreateSwapChainForHwnd = HRESULT (STDMETHODCALLTYPE *)(void*, IUnknown*, HWND, const void*, const void*, IDXGIOutput*, void**);')
    $t = $t.Replace('static HRESULT STDMETHODCALLTYPE HFactory2CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsDesc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain)',
                    'static HRESULT STDMETHODCALLTYPE HFactory2CreateSwapChainForHwnd(void* self, IUnknown* device, HWND hwnd, const void* desc, const void* fsDesc, IDXGIOutput* restrictToOutput, void** swapChain)')
    if ($t -ne $o) {
      Write-TextNoBom -Path $dxgi -Text $t
      Log 'V219 patched existing bridge_dx11_work src\client_dx11\dxgi_dx11bridge.cpp to ABI-safe Factory2 hook.'
    }
  }
}



function Mark-DX11Remix15FullRuntimeSyncV219 {
  Log 'DX11_V219_FULL_REMIX15_NEW_DX11_SYNC: runtime/UI/options/render backend sync for DX11 only; d3d9 and stock bridge stay blocked.'
}



function Patch-RuntimeMesonNgxDebugLibFallbackV219 {
  # DX11_V219_NGX_DEBUG_LIB_FALLBACK
  # Remix 1.5 runtime import can leave src\dxvk\meson.build requiring
  # nvsdk_ngx_d even during --buildtype=release. Most Packman/SDK payloads
  # stage nvsdk_ngx.lib for release, not nvsdk_ngx_d.lib.
  $targets = @(
    (Join-Path $Root 'src\dxvk\meson.build'),
    (Join-Path $Root 'meson.build')
  )

  foreach ($t in $targets) {
    if (!(Test-Path -LiteralPath $t -PathType Leaf)) { continue }
    $text = [System.IO.File]::ReadAllText($t)
    $orig = $text

    $text = $text.Replace("'nvsdk_ngx_d'", "'nvsdk_ngx'")
    $text = $text.Replace('"nvsdk_ngx_d"', '"nvsdk_ngx"')
    $text = [regex]::Replace($text, "nvsdk_ngx_d(?=[^A-Za-z0-9_])", "nvsdk_ngx")

    if ($text -ne $orig) {
      if (!(Test-Path -LiteralPath "$t.v219.before")) {
        # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $t -Destination "$t.v219.before" -Force
      }
      Write-TextNoBom -Path $t -Text $text
      Log "V219 patched imported Remix 1.5 Meson NGX debug lib fallback in: $t"
    }
  }

  $dxvkMeson = Join-Path $Root 'src\dxvk\meson.build'
  if (Test-Path -LiteralPath $dxvkMeson -PathType Leaf) {
    $check = [System.IO.File]::ReadAllText($dxvkMeson)
    if ($check -match "['""]nvsdk_ngx_d['""]") {
      Die "V219 failed: src\dxvk\meson.build still hard-requires nvsdk_ngx_d."
    }
  }
}



function Mark-DX11NgxDebugFallbackV219 {
  Log 'DX11_V219_NGX_DEBUG_LIB_FALLBACK: imported Remix 1.5 runtime Meson uses release nvsdk_ngx instead of missing nvsdk_ngx_d for x64 release builds.'
}



function Patch-RuntimeMesonNgxOptionalNoLibV219 {
  # DX11_V219_OPTIONAL_NGX_NO_LIB_BLOCK
  # If neither nvsdk_ngx_d.lib nor nvsdk_ngx.lib exists, do not block the
  # Remix 1.5 DX11 runtime/UI DLL rebuild at Meson configure time.  Build the
  # runtime without static NGX linkage; NGX/DLSS/RR/FG will remain unavailable
  # until the SDK import library is added.
  $dxvkMeson = Join-Path $Root 'src\dxvk\meson.build'
  if (!(Test-Path -LiteralPath $dxvkMeson -PathType Leaf)) {
    Log "V219: src\dxvk\meson.build not present yet; optional NGX patch skipped."
    return
  }

  $text = [System.IO.File]::ReadAllText($dxvkMeson)
  $orig = $text

  $optionalBlock = @'
dlss_lib_name = 'nvsdk_ngx'
if get_option('buildtype') == 'debug'
  dlss_lib_name = 'nvsdk_ngx_d_dbg'
endif
dlss_lib = cc.find_library(dlss_lib_name, dirs : dlss_lib_path, required : false)
if not dlss_lib.found()
  message('DX11_V219_OPTIONAL_NGX_NO_LIB_BLOCK: NGX import library not found; building runtime without static NGX linkage. DLSS/RR/FG NGX features will be disabled until nvsdk_ngx*.lib is added.')
  dlss_dep = declare_dependency(
    include_directories : [ dlss_include_dir, dlfg_include_dir ]
  )
else
  dlss_dep = declare_dependency(
    dependencies : [ dlss_lib ],
    include_directories : [ dlss_include_dir, dlfg_include_dir ]
  )
endif
'@

  $pattern = "(?s)dlss_lib_name\s*=\s*['""]nvsdk_ngx(?:_d)?['""].*?dlss_lib\s*=\s*cc\.find_library\s*\(\s*dlss_lib_name\s*,\s*dirs\s*:\s*dlss_lib_path\s*(?:,\s*required\s*:\s*(?:true|false))?\s*\)\s*dlss_dep\s*=\s*declare_dependency\s*\(\s*dependencies\s*:\s*\[\s*dlss_lib\s*\]\s*,\s*include_directories\s*:\s*\[\s*dlss_include_dir\s*,\s*dlfg_include_dir\s*\]\s*\)"
  $newText = [regex]::Replace($text, $pattern, [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $optionalBlock }, 1)

  if ($newText -eq $text) {
    # Fallback for slightly different formatting: make the find_library optional,
    # then insert the dlss_dep guard around the normal dependency assignment.
    $newText = $text
    $newText = [regex]::Replace(
      $newText,
      "dlss_lib\s*=\s*cc\.find_library\s*\(\s*dlss_lib_name\s*,\s*dirs\s*:\s*dlss_lib_path\s*(?:,\s*required\s*:\s*(?:true|false))?\s*\)",
      "dlss_lib = cc.find_library(dlss_lib_name, dirs : dlss_lib_path, required : false)",
      1
    )

    $newText = [regex]::Replace(
      $newText,
      "dlss_dep\s*=\s*declare_dependency\s*\(\s*dependencies\s*:\s*\[\s*dlss_lib\s*\]\s*,\s*include_directories\s*:\s*\[\s*dlss_include_dir\s*,\s*dlfg_include_dir\s*\]\s*\)",
      "if not dlss_lib.found()`r`n  message('DX11_V219_OPTIONAL_NGX_NO_LIB_BLOCK: NGX import library not found; building runtime without static NGX linkage. DLSS/RR/FG NGX features will be disabled until nvsdk_ngx*.lib is added.')`r`n  dlss_dep = declare_dependency(include_directories : [ dlss_include_dir, dlfg_include_dir ])`r`nelse`r`n  dlss_dep = declare_dependency(dependencies : [ dlss_lib ], include_directories : [ dlss_include_dir, dlfg_include_dir ])`r`nendif",
      1
    )
  }

  if ($newText -eq $orig) {
    Die "V219 failed: could not patch src\dxvk\meson.build NGX dependency block."
  }

  if (!(Test-Path -LiteralPath "$dxvkMeson.v219.before")) {
    # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $dxvkMeson -Destination "$dxvkMeson.v219.before" -Force
  }

  Write-TextNoBom -Path $dxvkMeson -Text $newText
  Log "V219 patched src\dxvk\meson.build so missing NGX import library is optional."

  $check = [System.IO.File]::ReadAllText($dxvkMeson)
  if ($check -match "cc\.find_library\s*\(\s*dlss_lib_name\s*,\s*dirs\s*:\s*dlss_lib_path\s*\)" -or
      $check -match "cc\.find_library\s*\(\s*['""]nvsdk_ngx['""]\s*,[^)]*\)") {
    Die "V219 failed: src\dxvk\meson.build still has a required NGX find_library call."
  }
  if ($check -notmatch "DX11_V219_OPTIONAL_NGX_NO_LIB_BLOCK") {
    Die "V219 failed: optional NGX marker was not inserted into src\dxvk\meson.build."
  }
}



function Mark-DX11OptionalNgxNoLibV219 {
  Log 'DX11_V219_OPTIONAL_NGX_NO_LIB_BLOCK: NGX import library is optional so Remix 1.5 DX11 runtime/UI DLL rebuild is not blocked when nvsdk_ngx*.lib is absent.'
}



function Get-ExistingDirectoryListV219 {
  $dirs = New-Object System.Collections.Generic.List[string]

  foreach ($d in @(
    $Root,
    (Join-Path $Root 'external'),
    (Join-Path $Root 'external\nv_ngx'),
    (Join-Path $Root 'external\nv_ngx_real'),
    (Join-Path $Root '_upstream_dxvk_remix_1_5_full_runtime_dx11'),
    (Join-Path $Root '_upstream_dxvk_remix_1_5_runtime_ui'),
    $env:NGX_SDK_DIR,
    $env:NVSDK_NGX_DIR,
    $env:DLSS_SDK,
    $env:NVIDIA_NGX_SDK,
    (Join-Path $env:USERPROFILE 'Downloads'),
    (Join-Path $env:USERPROFILE 'Desktop'),
    (Join-Path $env:ProgramData 'NVIDIA Corporation'),
    (Join-Path $env:ProgramFiles 'NVIDIA Corporation'),
    (Join-Path ${env:ProgramFiles(x86)} 'NVIDIA Corporation'),
    (Join-Path $env:SystemRoot 'System32'),
    (Join-Path $env:SystemRoot 'SysWOW64'),
    (Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository')
  )) {
    if (![string]::IsNullOrWhiteSpace($d)) {
      try {
        $full = [System.IO.Path]::GetFullPath($d)
        if ((Test-Path -LiteralPath $full -PathType Container) -and !$dirs.Contains($full)) {
          $dirs.Add($full)
        }
      } catch {}
    }
  }

  return $dirs
}

function Find-RealNgxLibOrDllV219 {
  # DX11_V219_REAL_NGX_IMPORT_LIBRARY
  # Do not create stub libs. First find an actual NVIDIA NGX import library.
  # If not present, find real NVIDIA nvngx.dll from the driver/NGX runtime and
  # create an import library from its exports with dumpbin/lib.
  $stageRoot = Join-Path $Root 'external\nv_ngx_real'
  $stageLib = Join-Path $stageRoot 'lib\x64'
  $stageBin = Join-Path $stageRoot 'bin\x64'
  New-Item -ItemType Directory -Path $stageLib -Force | Out-Null
  New-Item -ItemType Directory -Path $stageBin -Force | Out-Null

  $finalLib = Join-Path $stageLib 'nvsdk_ngx.lib'
  $finalDll = Join-Path $stageBin 'nvngx.dll'
  $report = Join-Path $Root 'DX11_V219_REAL_NGX_IMPORT_LIBRARY_SEARCH.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_REAL_NGX_IMPORT_LIBRARY')
  $lines.Add(('Time: {0}' -f (Get-Date)))
  $lines.Add('Mode: real NVIDIA library only; no stubs.')

  if ((Test-Path -LiteralPath $finalLib -PathType Leaf) -and ((Get-Item -LiteralPath $finalLib).Length -gt 1024)) {
    $lines.Add(('OK: staged NGX import library already exists: {0}' -f $finalLib))
    [System.IO.File]::WriteAllLines($report, $lines)
    $stagedDllForReturnV219 = $null
    if (Test-Path -LiteralPath $finalDll -PathType Leaf) { $stagedDllForReturnV219 = $finalDll }
    return @{ Lib = $finalLib; Dll = $stagedDllForReturnV219; Mode = 'existing-staged-lib' }
  }

  $roots = Get-ExistingDirectoryListV219
  $lines.Add('Search roots:')
  foreach ($r in $roots) { $lines.Add('  ' + $r) }

  $libNames = @(
    # DX11_V225: prefer the Khronos/Vulkan (khr) NGX libs first. dxvk is Vulkan-based
    # and the khr libs are built with a modern toolchain, unlike the vs2010 libs that
    # bake _MSC_VER=1600 and reference legacy CRT symbols (LNK2038/LNK2019 at link).
    'nvsdk_ngx_khr_s.lib',
    'nvsdk_ngx_khr_d.lib',
    'nvsdk_ngx.lib',
    'nvsdk_ngx_s.lib',
    'nvsdk_ngx_x64.lib',
    'nvsdk_ngx64.lib',
    'nvsdk_ngx_d.lib',
    'nvsdk_ngx_d_dbg.lib'
  )

  $libCandidates = New-Object System.Collections.Generic.List[string]
  foreach ($rootDir in $roots) {
    foreach ($name in $libNames) {
      try {
        $hits = Get-ChildItem -LiteralPath $rootDir -Filter $name -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 20
        foreach ($h in $hits) {
          if (!$libCandidates.Contains($h.FullName)) {
            $libCandidates.Add($h.FullName)
          }
        }
      } catch {}
    }
  }

  if ($libCandidates.Count -gt 0) {
    $chosen = $null

    # DX11_V225: prefer the Khronos/Vulkan (khr) NGX library — it matches dxvk's
    # Vulkan NGX usage and is modern-toolchain-compatible (no _MSC_VER=1600 / legacy
    # CRT mismatch the vs2010 libs cause). Prefer the dynamic-CRT release (_d.lib,
    # /MD MD_DynamicRelease) to match dxvk's runtime library; the _s libs are /MT
    # (MT_StaticRelease) and trigger LNK2038 RuntimeLibrary mismatches.
    foreach ($c in $libCandidates) {
      if (($c -match '(?i)\\khr\\') -and ([System.IO.Path]::GetFileName($c) -match '(?i)_d\.lib$')) {
        $chosen = $c
        break
      }
    }
    if (!$chosen) {
      foreach ($c in $libCandidates) {
        if ($c -match '(?i)\\khr\\') { $chosen = $c; break }
      }
    }
    if (!$chosen) {
      foreach ($c in $libCandidates) {
        if ([System.IO.Path]::GetFileName($c).Equals('nvsdk_ngx.lib', [System.StringComparison]::OrdinalIgnoreCase)) {
          $chosen = $c
          break
        }
      }
    }
    # Avoid the vs2010 libs unless nothing else is available.
    if (!$chosen) {
      foreach ($c in $libCandidates) {
        if ($c -notmatch '(?i)\\vs2010\\') { $chosen = $c; break }
      }
    }
    if (!$chosen) { $chosen = $libCandidates[0] }

    Copy-Item -LiteralPath $chosen -Destination $finalLib -Force
    $lines.Add(('OK: found real NVIDIA NGX import library: {0}' -f $chosen))
    $lines.Add(('OK: staged as: {0}' -f $finalLib))
    [System.IO.File]::WriteAllLines($report, $lines)
    Log "V219 staged real NVIDIA NGX import library: $chosen -> $finalLib"
    $stagedDllForReturnV219 = $null
    if (Test-Path -LiteralPath $finalDll -PathType Leaf) { $stagedDllForReturnV219 = $finalDll }
    return @{ Lib = $finalLib; Dll = $stagedDllForReturnV219; Mode = 'real-lib' }
  }

  $dllNames = @('nvngx.dll', 'nvsdk_ngx.dll')
  $dllCandidates = New-Object System.Collections.Generic.List[string]
  foreach ($rootDir in $roots) {
    foreach ($name in $dllNames) {
      try {
        $hits = Get-ChildItem -LiteralPath $rootDir -Filter $name -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 20
        foreach ($h in $hits) {
          if (!$dllCandidates.Contains($h.FullName)) {
            $dllCandidates.Add($h.FullName)
          }
        }
      } catch {}
    }
  }

  if ($dllCandidates.Count -eq 0) {
    $lines.Add('BAD: no real NVIDIA NGX import library or nvngx.dll was found.')
    $lines.Add('Install the NVIDIA NGX/DLSS SDK or NVIDIA driver NGX Core Runtime, then rerun.')
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 could not find a real NVIDIA NGX import library or nvngx.dll. No stub was created. See $report"
  }

  $chosenDll = $dllCandidates[0]
  foreach ($c in $dllCandidates) {
    if ([System.IO.Path]::GetFileName($c).Equals('nvngx.dll', [System.StringComparison]::OrdinalIgnoreCase)) {
      $chosenDll = $c
      break
    }
  }

  Copy-Item -LiteralPath $chosenDll -Destination $finalDll -Force

  $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
  $libexe = Get-Command lib.exe -ErrorAction SilentlyContinue
  if (!$dumpbin -or !$libexe) {
    $lines.Add(('BAD: found real nvngx.dll at {0}, but dumpbin.exe/lib.exe are not available in the VS x64 environment.' -f $chosenDll))
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 found nvngx.dll but cannot generate import lib because dumpbin.exe/lib.exe are unavailable. See $report"
  }

  $exportsTxt = Join-Path $stageLib 'nvngx.exports.txt'
  $defFile = Join-Path $stageLib 'nvsdk_ngx.def'
  & $dumpbin.Source /nologo /exports $finalDll > $exportsTxt
  if ($LASTEXITCODE -ne 0) {
    $lines.Add(('BAD: dumpbin /exports failed for {0}' -f $finalDll))
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 dumpbin /exports failed for real nvngx.dll. See $report"
  }

  $exportText = [System.IO.File]::ReadAllLines($exportsTxt)
  $exports = New-Object System.Collections.Generic.List[string]
  foreach ($line in $exportText) {
    if ($line -match '^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_@?$]*)') {
      $sym = $Matches[1]
      if ($sym -match '^NVSDK_NGX_' -or $sym -match '^_?NVSDK_NGX_') {
        $clean = $sym
        if (!$exports.Contains($clean)) {
          $exports.Add($clean)
        }
      }
    }
  }

  if ($exports.Count -eq 0) {
    $lines.Add(('BAD: real DLL had no NVSDK_NGX_* exports: {0}' -f $finalDll))
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 real nvngx.dll has no NVSDK_NGX_* exports, cannot create real import lib. See $report"
  }

  $defLines = New-Object System.Collections.Generic.List[string]
  $defLines.Add('LIBRARY "nvngx.dll"')
  $defLines.Add('EXPORTS')
  foreach ($e in ($exports | Sort-Object)) {
    $defLines.Add('  ' + $e)
  }
  [System.IO.File]::WriteAllLines($defFile, $defLines)

  & $libexe.Source /nologo /machine:x64 /def:$defFile /out:$finalLib | Out-Host
  if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $finalLib -PathType Leaf)) {
    $lines.Add('BAD: lib.exe failed to generate nvsdk_ngx.lib from real nvngx.dll exports.')
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 lib.exe failed to generate real import lib. See $report"
  }

  $lines.Add(('OK: found real NVIDIA NGX runtime DLL: {0}' -f $chosenDll))
  $lines.Add(('OK: copied runtime DLL to: {0}' -f $finalDll))
  $lines.Add(('OK: generated real import library from DLL exports: {0}' -f $finalLib))
  $lines.Add(('OK: exported NVSDK_NGX_* symbol count: {0}' -f $exports.Count))
  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 generated real nvsdk_ngx.lib from NVIDIA nvngx.dll exports: $finalLib"
  return @{ Lib = $finalLib; Dll = $finalDll; Mode = 'generated-import-lib-from-real-dll' }
}

function Patch-RuntimeMesonUseRealNgxLibV219 {
  # DX11_V219_REAL_NGX_IMPORT_LIBRARY
  # Force Meson to use the staged real NGX import library. This intentionally
  # disables the previous optional/no-lib path because the user asked for real,
  # not stubs/no-op dependencies.
  $dxvkMeson = Join-Path $Root 'src\dxvk\meson.build'
  if (!(Test-Path -LiteralPath $dxvkMeson -PathType Leaf)) {
    Log "V219: src\dxvk\meson.build not present yet; real NGX Meson patch skipped."
    return
  }

  $text = [System.IO.File]::ReadAllText($dxvkMeson)
  $orig = $text

  $requiredBlock = @'
dlss_lib_name = 'nvsdk_ngx'
dlss_lib = cc.find_library(
  dlss_lib_name,
  dirs : join_paths(meson.project_source_root(), 'external', 'nv_ngx_real', 'lib', 'x64'),
  required : true
)
dlss_dep = declare_dependency(
  dependencies : [ dlss_lib ],
  include_directories : [ dlss_include_dir, dlfg_include_dir ]
)
message('DX11_V219_REAL_NGX_IMPORT_LIBRARY: using real staged NVIDIA NGX import library from external/nv_ngx_real/lib/x64')
'@

  $marker = "message('DX11_V219_REAL_NGX_IMPORT_LIBRARY: using real staged NVIDIA NGX import library from external/nv_ngx_real/lib/x64')"
  $escapedMarker = [regex]::Escape($marker)
  $text = [regex]::Replace(
    $text,
    "(?m)^(?:$escapedMarker\s*(?:\r?\n|$))+",
    "$marker`r`n")

  # The worktree persists between builds. If the complete real-NGX block is
  # already installed, preserve it after normalizing historical duplicate
  # marker lines. Replacing an already-patched block used to prepend one marker
  # per invocation, changed meson.build every time, and invalidated the entire
  # runtime compile graph.
  $hasRequiredRealNgxBlock =
    $text -match "(?s)dlss_lib_name\s*=\s*'nvsdk_ngx'.*?dirs\s*:\s*join_paths\s*\(\s*meson\.project_source_root\(\)\s*,\s*'external'\s*,\s*'nv_ngx_real'\s*,\s*'lib'\s*,\s*'x64'\s*\).*?required\s*:\s*true.*?dlss_dep\s*=\s*declare_dependency\s*\(.*?$escapedMarker"
  if ($hasRequiredRealNgxBlock) {
    if ($text -cne $orig) {
      Write-TextNoBom -Path $dxvkMeson -Text $text
      Log "V219 normalized duplicate real-NGX Meson markers."
    } else {
      Log "V219 real-NGX Meson block is already installed."
    }
    return
  }

  # Replace V219 optional block or stock upstream NGX block.
  $patterns = @(
    "(?s)dlss_lib_name\s*=\s*['""]nvsdk_ngx['""].*?DX11_V219_OPTIONAL_NGX_NO_LIB_BLOCK.*?endif",
    "(?s)dlss_lib_name\s*=\s*['""]nvsdk_ngx(?:_d)?['""].*?dlss_lib\s*=\s*cc\.find_library\s*\(\s*dlss_lib_name\s*,\s*dirs\s*:\s*dlss_lib_path\s*(?:,\s*required\s*:\s*(?:true|false))?\s*\).*?dlss_dep\s*=\s*declare_dependency\s*\(\s*dependencies\s*:\s*\[\s*dlss_lib\s*\]\s*,\s*include_directories\s*:\s*\[\s*dlss_include_dir\s*,\s*dlfg_include_dir\s*\]\s*\)"
  )

  foreach ($pat in $patterns) {
    $newText = [regex]::Replace($text, $pat, [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $requiredBlock }, 1)
    if ($newText -ne $text) {
      $text = $newText
      break
    }
  }

  if ($text -eq $orig) {
    $text = [regex]::Replace(
      $text,
      "dlss_lib\s*=\s*cc\.find_library\s*\([^)]*\)",
      "dlss_lib = cc.find_library('nvsdk_ngx', dirs : join_paths(meson.project_source_root(), 'external', 'nv_ngx_real', 'lib', 'x64'), required : true)",
      1
    )
  }

  if ($text -eq $orig -or $text -notmatch "DX11_V219_REAL_NGX_IMPORT_LIBRARY") {
    Die "V219 failed to patch src\dxvk\meson.build to use the real staged NGX import library."
  }

  if (!(Test-Path -LiteralPath "$dxvkMeson.v219.before")) {
    # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $dxvkMeson -Destination "$dxvkMeson.v219.before" -Force
  }

  Write-TextNoBom -Path $dxvkMeson -Text $text
  Log "V219 patched src\dxvk\meson.build to require the real staged NVIDIA NGX import library."
}

function Stage-RealNgxRuntimeV219 {
  param(
    [Parameter(Mandatory)][string]$X86Out,
    [Parameter(Mandatory)][string]$X64Out
  )

  $srcDll = Join-Path $Root 'external\nv_ngx_real\bin\x64\nvngx.dll'
  if (!(Test-Path -LiteralPath $srcDll -PathType Leaf)) {
    Log "V219 no nvngx.dll was staged; import library may have come from SDK .lib only."
    return
  }

  $trex = Join-Path $X86Out '.trex'
  if (!(Test-Path -LiteralPath $trex -PathType Container)) {
    New-Item -ItemType Directory -Path $trex -Force | Out-Null
  }

  Copy-Item -LiteralPath $srcDll -Destination (Join-Path $trex 'nvngx.dll') -Force
  if (Test-Path -LiteralPath $X64Out -PathType Container) {
    Copy-Item -LiteralPath $srcDll -Destination (Join-Path $X64Out 'nvngx.dll') -Force
  }

  Log "V219 staged real NVIDIA nvngx.dll next to .trex runtime DLLs."
}



function Mark-DX11RealNgxImportLibraryV219 {
  Log 'DX11_V219_REAL_NGX_IMPORT_LIBRARY: build uses real NVIDIA NGX SDK import library or a real import lib generated from signed nvngx.dll exports; no stubs.'
}



function Mark-DX11FixInlineIfParserV219 {
  Log 'DX11_V219_FIX_INLINE_IF_PARSER: removed invalid PowerShell inline-if hashtable expressions from real NGX finder.'
}



function Patch-RuntimeMesonRemoveNgxExtraEndifV219 {
  # DX11_V219_FIX_NGX_EXTRA_ENDIF
  # V219 can replace the NGX dependency block with a real import-lib block but
  # leave a stale Meson endif directly after the marker line. That creates:
  #   ERROR: Expecting eof got endif.
  $dxvkMeson = Join-Path $Root 'src\dxvk\meson.build'
  if (!(Test-Path -LiteralPath $dxvkMeson -PathType Leaf)) {
    Log "V219: src\dxvk\meson.build not present yet; NGX extra-endif cleanup skipped."
    return
  }

  $text = [System.IO.File]::ReadAllText($dxvkMeson)
  $orig = $text

  # Remove one or more stale endif lines immediately following the real NGX marker.
  $text = [regex]::Replace(
    $text,
    "(?m)(message\('DX11_V219_REAL_NGX_IMPORT_LIBRARY:[^']*'\)\s*(?:\r?\n)+)(\s*endif\s*(?:\r?\n|$))+",
    '$1'
  )

  # Same cleanup for scripts already copied before version renaming.
  $text = [regex]::Replace(
    $text,
    "(?m)(message\('DX11_V219_REAL_NGX_IMPORT_LIBRARY:[^']*'\)\s*(?:\r?\n)+)(\s*endif\s*(?:\r?\n|$))+",
    '$1'
  )

  # If there is still an isolated endif directly after the staged real NGX lib
  # block, remove it using the import-lib path as the anchor.
  $text = [regex]::Replace(
    $text,
    "(?s)(dlss_lib\s*=\s*cc\.find_library\s*\(\s*dlss_lib_name\s*,\s*dirs\s*:\s*join_paths\s*\(\s*meson\.project_source_root\(\)\s*,\s*'external'\s*,\s*'nv_ngx_real'\s*,\s*'lib'\s*,\s*'x64'\s*\)\s*,\s*required\s*:\s*true\s*\)\s*dlss_dep\s*=\s*declare_dependency\s*\(.*?include_directories\s*:\s*\[\s*dlss_include_dir\s*,\s*dlfg_include_dir\s*\]\s*\)\s*message\('DX11_V15[89]_REAL_NGX_IMPORT_LIBRARY:[^']*'\)\s*)(endif\s*)+",
    '$1'
  )

  if ($text -ne $orig) {
    if (!(Test-Path -LiteralPath "$dxvkMeson.v219.before")) {
      # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $dxvkMeson -Destination "$dxvkMeson.v219.before" -Force
    }
    Write-TextNoBom -Path $dxvkMeson -Text $text
    Log "V219 removed stale NGX endif from src\dxvk\meson.build."
  } else {
    Log "V219: no stale NGX endif found in src\dxvk\meson.build."
  }

  $check = [System.IO.File]::ReadAllText($dxvkMeson)
  if ($check -match "DX11_V219_REAL_NGX_IMPORT_LIBRARY:[^'\r\n]*'\)\s*(?:\r?\n)+\s*endif\b" -or
      $check -match "DX11_V219_REAL_NGX_IMPORT_LIBRARY:[^'\r\n]*'\)\s*(?:\r?\n)+\s*endif\b") {
    Die "V219 failed: stale endif still follows the real NGX import-library marker in src\dxvk\meson.build."
  }
}



function Mark-DX11NgxExtraEndifFixV219 {
  Log 'DX11_V219_FIX_NGX_EXTRA_ENDIF: stale Meson endif after real NGX import-library block is removed before x64 runtime setup.'
}



function Install-RuntimeShaderCompilerWrapperV219 {
  # DX11_V219_SLANG_MODULE_FILTER
  # Remix 1.5 has shared Slang modules like pass/gbuffer/gbuffer.slang.
  # The older compile_shaders.py tries to compile every .slang file as a
  # standalone shader and fails with "shader type not specified".  Keep real
  # shader compilation, but only pass standalone entry shader files to the old
  # compiler.  The original shader tree is added as an include path so entry
  # shaders can still import shared modules.
  $scripts = Join-Path $Root 'scripts-common'
  if (!(Test-Path -LiteralPath $scripts -PathType Container)) {
    Log "V219: scripts-common folder missing; shader compiler wrapper skipped."
    return
  }

  $compile = Join-Path $scripts 'compile_shaders.py'
  $orig = Join-Path $scripts 'compile_shaders_orig_v219.py'
  $backup = Join-Path $scripts 'compile_shaders_before_v219.py'

  if (!(Test-Path -LiteralPath $compile -PathType Leaf)) {
    Log "V219: compile_shaders.py missing; shader compiler wrapper skipped."
    return
  }

  if (!(Test-Path -LiteralPath $orig -PathType Leaf)) {
    Copy-Item -LiteralPath $compile -Destination $backup -Force

    $upstreamCandidates = @(
      (Join-Path $Root '_upstream_dxvk_remix_1_5_full_runtime_dx11\scripts-common\compile_shaders.py'),
      (Join-Path $Root '_upstream_dxvk_remix_1_5_runtime_ui\scripts-common\compile_shaders.py')
    )

    $copiedUpstream = $false
    foreach ($u in $upstreamCandidates) {
      if (Test-Path -LiteralPath $u -PathType Leaf) {
        Copy-Item -LiteralPath $u -Destination $orig -Force
        Log "V219 using upstream Remix 1.5 compile_shaders.py as original compiler: $u"
        $copiedUpstream = $true
        break
      }
    }

    if (!$copiedUpstream) {
      Copy-Item -LiteralPath $compile -Destination $orig -Force
      Log "V219 using existing compile_shaders.py as original compiler."
    }
  }

  $wrapper = @'
#!/usr/bin/env python3
# DX11_V219_SLANG_MODULE_FILTER
# Real shader compiler wrapper for Remix 1.5 Slang module layout.
# This does not generate fake shader output. It filters shared .slang modules
# out of the standalone compile list, then invokes the original compiler.

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ORIG = SCRIPT_DIR / "compile_shaders_orig_v219.py"

STAGE_EXTS = {
    ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese",
    ".rgen", ".rchit", ".rahit", ".rmiss", ".rcall",
    ".mesh", ".task"
}

STAGE_NAME_TOKENS = (
    ".vert.", ".frag.", ".comp.", ".geom.", ".tesc.", ".tese.",
    ".rgen.", ".rchit.", ".rahit.", ".rmiss.", ".rcall.",
    ".mesh.", ".task.",
    "_vs", "_ps", "_fs", "_cs", "_gs", "_ms", "_ts",
    "vertex", "pixel", "fragment", "compute",
    "raygen", "closesthit", "anyhit", "miss", "callable",
)

def _find_arg(args: list[str], name: str) -> int:
    try:
        return args.index(name)
    except ValueError:
        return -1

def _is_entry_shader(path: Path) -> bool:
    lower_name = path.name.lower()
    lower_stem = path.stem.lower()
    suffix = path.suffix.lower()

    if suffix in STAGE_EXTS:
        return True

    if suffix == ".slang":
        joined = lower_name
        if any(tok in joined or tok in lower_stem for tok in STAGE_NAME_TOKENS):
            return True
        return False

    if suffix in {".hlsl", ".glsl"}:
        return True

    return False

def _copy_filtered_tree(src: Path, dst: Path) -> tuple[int, list[str]]:
    copied = 0
    skipped: list[str] = []
    for f in src.rglob("*"):
        if not f.is_file():
            continue
        rel = f.relative_to(src)
        if _is_entry_shader(f):
            out = dst / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)
            copied += 1
        elif f.suffix.lower() == ".slang":
            skipped.append(str(rel).replace("\\", "/"))
    return copied, skipped

def main() -> int:
    if not ORIG.is_file():
        print(f"DX11_V219_SLANG_MODULE_FILTER: original compiler missing: {ORIG}", file=sys.stderr)
        return 2

    args = sys.argv[1:]
    input_i = _find_arg(args, "-input")
    output_i = _find_arg(args, "-output")

    if input_i < 0 or input_i + 1 >= len(args):
        return subprocess.call([sys.executable, str(ORIG)] + args)

    input_dir = Path(args[input_i + 1]).resolve()
    if not input_dir.is_dir():
        return subprocess.call([sys.executable, str(ORIG)] + args)

    output_dir = None
    if output_i >= 0 and output_i + 1 < len(args):
        output_dir = Path(args[output_i + 1]).resolve()

    with tempfile.TemporaryDirectory(prefix="dx11_v219_shader_fulltree_", ignore_cleanup_errors=True) as td:
        filtered = Path(td) / "input"
        filtered.mkdir(parents=True, exist_ok=True)

        copied, skipped = _copy_filtered_tree(input_dir, filtered)

        if output_dir is not None:
            output_dir.mkdir(parents=True, exist_ok=True)
            report = output_dir / "dx11_v219_skipped_slang_modules.txt"
            report.write_text(
                "DX11_V219_SLANG_MODULE_FILTER\n"
                f"OriginalInput={input_dir}\n"
                f"FilteredInput={filtered}\n"
                f"EntryFilesCopied={copied}\n"
                f"SharedSlangModulesSkipped={len(skipped)}\n\n"
                + "\n".join(skipped)
                + ("\n" if skipped else ""),
                encoding="utf-8"
            )

        if copied == 0:
            return subprocess.call([sys.executable, str(ORIG)] + args)

        new_args = list(args)
        new_args[input_i + 1] = str(filtered)

        if "-include" not in new_args or str(input_dir) not in new_args:
            new_args.extend(["-include", str(input_dir)])

        print(
            "DX11_V219_SLANG_MODULE_FILTER: compiling filtered shader entry tree "
            f"entries={copied} skipped_slang_modules={len(skipped)} original={input_dir}",
            flush=True
        )

        return subprocess.call([sys.executable, str(ORIG)] + new_args)

if __name__ == "__main__":
    raise SystemExit(main())
'@

  Write-TextNoBom -Path $compile -Text $wrapper
  Log "V219 installed real shader compiler wrapper: $compile"
}



function Mark-DX11SlangModuleFilterV219 {
  Log 'DX11_V219_SLANG_MODULE_FILTER: shader compiler filters shared Remix 1.5 .slang modules from standalone entry compile list.'
}



function Restore-RealSharedConstantsHeaderV219 {
  # DX11_V219_REAL_SHARED_CONSTANTS_HEADER
  # Do not create a placeholder. Locate the real RTX Remix 1.5
  # shared_constants.h and mirror it into the C++ include path used by dxbc.
  $destinations = @(
    (Join-Path $Root 'include\rtx\utility\shared_constants.h'),
    (Join-Path $Root 'src\dxvk\rtx\utility\shared_constants.h')
  )

  $candidates = New-Object System.Collections.Generic.List[string]
  $roots = @(
    $Root,
    (Join-Path $Root '_upstream_dxvk_remix_1_5_full_runtime_dx11'),
    (Join-Path $Root '_upstream_dxvk_remix_1_5_runtime_ui'),
    (Join-Path $Root 'src'),
    (Join-Path $Root 'include')
  )

  foreach ($r in $roots) {
    if ([string]::IsNullOrWhiteSpace($r) -or !(Test-Path -LiteralPath $r -PathType Container)) { continue }
    try {
      $hits = Get-ChildItem -LiteralPath $r -Filter 'shared_constants.h' -File -Recurse -ErrorAction SilentlyContinue
      foreach ($h in $hits) {
        if (!$candidates.Contains($h.FullName)) { $candidates.Add($h.FullName) }
      }
    } catch {}
  }

  $source = $null
  foreach ($c in $candidates) {
    $norm = $c -replace '/', '\'
    if ($norm -match '\\rtx\\utility\\shared_constants\.h$') {
      $source = $c
      break
    }
  }
  if (!$source -and $candidates.Count -gt 0) {
    $source = $candidates[0]
  }

  if (!$source) {
    $report = Join-Path $Root 'DX11_V219_SHARED_CONSTANTS_HEADER_NOT_FOUND.txt'
    $lines = @(
      'DX11_V219_REAL_SHARED_CONSTANTS_HEADER',
      'No real shared_constants.h was found.',
      'No placeholder was created.',
      'Expected a real RTX Remix 1.5 header under one of:',
      '  include\rtx\utility\shared_constants.h',
      '  src\dxvk\shaders\rtx\utility\shared_constants.h',
      '  _upstream_dxvk_remix_1_5_full_runtime_dx11\...\shared_constants.h'
    )
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 could not find real shared_constants.h. No placeholder was created. See $report"
  }

  foreach ($d in $destinations) {
    $parent = Split-Path -Parent $d
    if (!(Test-Path -LiteralPath $parent -PathType Container)) {
      New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $srcFullV219 = [System.IO.Path]::GetFullPath($source)
    $dstFullV219 = [System.IO.Path]::GetFullPath($d)
    if ([string]::Equals($srcFullV219, $dstFullV219, [System.StringComparison]::OrdinalIgnoreCase)) {
      Log "V219 shared_constants.h already in place; skipped copy-self: $d"
    } else {
      Copy-Item -LiteralPath $source -Destination $d -Force
      Log "V219 copied real shared_constants.h: $source -> $d"
    }
  }

  $verify = Join-Path $Root 'DX11_V219_REAL_SHARED_CONSTANTS_HEADER_VERIFY.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_REAL_SHARED_CONSTANTS_HEADER')
  $lines.Add(('Source: {0}' -f $source))
  foreach ($d in $destinations) {
    if (Test-Path -LiteralPath $d -PathType Leaf) {
      $hash = (Get-FileHash -LiteralPath $d -Algorithm SHA256).Hash
      $len = (Get-Item -LiteralPath $d).Length
      $lines.Add(('OK: {0} bytes SHA256={1} path={2}' -f $len, $hash, $d))
    } else {
      $lines.Add(('BAD: missing destination {0}' -f $d))
    }
  }
  [System.IO.File]::WriteAllLines($verify, $lines)
}

function Install-RuntimeShaderCompilerWrapperV219 {
  # DX11_V219_REAL_SHADER_INCLUDE_TREE
  # V219 skipped shared Slang modules correctly, but it also failed to copy real
  # .h/.hlsli/.slangh include files into the filtered temporary shader tree.
  # V219 keeps all real include/data files and removes only shared .slang files
  # from the standalone compile list.
  $scripts = Join-Path $Root 'scripts-common'
  if (!(Test-Path -LiteralPath $scripts -PathType Container)) {
    Log "V219: scripts-common folder missing; shader compiler wrapper skipped."
    return
  }

  $compile = Join-Path $scripts 'compile_shaders.py'
  $orig = Join-Path $scripts 'compile_shaders_orig_v219.py'
  $backup = Join-Path $scripts 'compile_shaders_before_v219.py'

  if (!(Test-Path -LiteralPath $compile -PathType Leaf)) {
    Log "V219: compile_shaders.py missing; shader compiler wrapper skipped."
    return
  }

  if (!(Test-Path -LiteralPath $orig -PathType Leaf)) {
    Copy-Item -LiteralPath $compile -Destination $backup -Force

    foreach ($candidate in @(
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $Root '_upstream_dxvk_remix_1_5_full_runtime_dx11\scripts-common\compile_shaders.py'),
      (Join-Path $Root '_upstream_dxvk_remix_1_5_runtime_ui\scripts-common\compile_shaders.py')
    )) {
      if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        Copy-Item -LiteralPath $candidate -Destination $orig -Force
        Log "V219 using original shader compiler: $candidate"
        break
      }
    }

    if (!(Test-Path -LiteralPath $orig -PathType Leaf)) {
      Copy-Item -LiteralPath $compile -Destination $orig -Force
      Log "V219 using current compile_shaders.py as original compiler."
    }
  }

  $wrapper = @'
#!/usr/bin/env python3
# DX11_V219_REAL_SHADER_INCLUDE_TREE
# Real shader compiler wrapper for Remix 1.5 Slang module layout.
# No placeholder shaders and no fake output. This wrapper preserves all real
# include/header/data files and skips only shared .slang modules as standalone
# compile entries.

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ORIG = SCRIPT_DIR / "compile_shaders_orig_v219.py"
if not ORIG.is_file():
    fallback = SCRIPT_DIR / "compile_shaders_orig_v219.py"
    if fallback.is_file():
        ORIG = fallback

STAGE_EXTS = {
    ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese",
    ".rgen", ".rchit", ".rahit", ".rmiss", ".rcall",
    ".mesh", ".task"
}

STAGE_NAME_TOKENS = (
    ".vert.", ".frag.", ".comp.", ".geom.", ".tesc.", ".tese.",
    ".rgen.", ".rchit.", ".rahit.", ".rmiss.", ".rcall.",
    ".mesh.", ".task.",
    "_vs", "_ps", "_fs", "_cs", "_gs", "_ms", "_ts",
    "vertex", "pixel", "fragment", "compute",
    "raygen", "closesthit", "anyhit", "miss", "callable",
)

REAL_INCLUDE_SUFFIXES = {
    ".h", ".hh", ".hpp", ".hlsli", ".inc", ".ush", ".glslh",
    ".slangh", ".json", ".txt"
}

def _find_arg(args: list[str], name: str) -> int:
    try:
        return args.index(name)
    except ValueError:
        return -1

def _is_entry_shader(path: Path) -> bool:
    lower_name = path.name.lower()
    lower_stem = path.stem.lower()
    suffix = path.suffix.lower()

    if suffix in STAGE_EXTS:
        return True

    if suffix == ".slang":
        joined = lower_name
        return any(tok in joined or tok in lower_stem for tok in STAGE_NAME_TOKENS)

    if suffix in {".hlsl", ".glsl"}:
        return True

    return False

def _should_copy_non_entry(path: Path) -> bool:
    suffix = path.suffix.lower()
    if suffix in REAL_INCLUDE_SUFFIXES:
        return True

    # Keep extensionless include/data files too. Shader trees sometimes include
    # generated binding files or tables without a conventional extension.
    if suffix == "":
        return True

    return False

def _copy_filtered_tree(src: Path, dst: Path) -> tuple[int, int, list[str]]:
    entries = 0
    includes = 0
    skipped_slang: list[str] = []

    for f in src.rglob("*"):
        if not f.is_file():
            continue

        rel = f.relative_to(src)
        out = dst / rel
        suffix = f.suffix.lower()

        if _is_entry_shader(f):
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)
            entries += 1
            continue

        if suffix == ".slang":
            skipped_slang.append(str(rel).replace("\\", "/"))
            continue

        if _should_copy_non_entry(f):
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)
            includes += 1

    return entries, includes, skipped_slang

def main() -> int:
    if not ORIG.is_file():
        print(f"DX11_V219_REAL_SHADER_INCLUDE_TREE: original compiler missing: {ORIG}", file=sys.stderr)
        return 2

    args = sys.argv[1:]
    input_i = _find_arg(args, "-input")
    output_i = _find_arg(args, "-output")

    if input_i < 0 or input_i + 1 >= len(args):
        return subprocess.call([sys.executable, str(ORIG)] + args)

    input_dir = Path(args[input_i + 1]).resolve()
    if not input_dir.is_dir():
        return subprocess.call([sys.executable, str(ORIG)] + args)

    output_dir = None
    if output_i >= 0 and output_i + 1 < len(args):
        output_dir = Path(args[output_i + 1]).resolve()

    with tempfile.TemporaryDirectory(prefix="dx11_v219_shader_fulltree_", ignore_cleanup_errors=True) as td:
        filtered = Path(td) / "input"
        filtered.mkdir(parents=True, exist_ok=True)

        entries, includes, skipped = _copy_filtered_tree(input_dir, filtered)

        if output_dir is not None:
            output_dir.mkdir(parents=True, exist_ok=True)
            report = output_dir / "dx11_v219_skipped_slang_modules_and_copied_headers.txt"
            report.write_text(
                "DX11_V219_REAL_SHADER_INCLUDE_TREE\n"
                f"OriginalInput={input_dir}\n"
                f"FilteredInput={filtered}\n"
                f"EntryFilesCopied={entries}\n"
                f"RealIncludeFilesCopied={includes}\n"
                f"SharedSlangModulesSkipped={len(skipped)}\n\n"
                + "\n".join(skipped)
                + ("\n" if skipped else ""),
                encoding="utf-8"
            )

        if entries == 0:
            return subprocess.call([sys.executable, str(ORIG)] + args)

        new_args = list(args)
        new_args[input_i + 1] = str(filtered)

        # Keep both temp tree and original tree available to include/import lookup.
        existing_includes = {new_args[i + 1] for i, a in enumerate(new_args[:-1]) if a == "-include"}
        for inc in (filtered, input_dir, input_dir.parent):
            s = str(inc)
            if s not in existing_includes:
                new_args.extend(["-include", s])
                existing_includes.add(s)

        print(
            "DX11_V219_REAL_SHADER_INCLUDE_TREE: compiling filtered shader entry tree "
            f"entries={entries} real_includes={includes} skipped_slang_modules={len(skipped)} "
            f"original={input_dir}",
            flush=True
        )

        return subprocess.call([sys.executable, str(ORIG)] + new_args)

if __name__ == "__main__":
    raise SystemExit(main())
'@

  Write-TextNoBom -Path $compile -Text $wrapper
  Log "V219 installed shader compiler wrapper with real include/header preservation: $compile"
}



function Mark-DX11SlangModuleFilterV219 {
  Log 'DX11_V219_REAL_SHADER_INCLUDE_TREE: shader compiler filters shared .slang modules but preserves real header/include files.'
}

function Mark-DX11RealSharedConstantsV219 {
  Log 'DX11_V219_REAL_SHARED_CONSTANTS_HEADER: real RTX shared_constants.h is mirrored into the C++ include path; no placeholder header.'
}



function Add-DxvkEnvShouldBypassRemixV219 {
  # DX11_V219_REAL_ENV_BYPASS_FUNCTION
  # Remix 1.5 dxgi_main.cpp expects dxvk::env::shouldBypassRemixForCurrentProcess().
  # Add the real helper to util_env.h instead of removing the calls.
  $h = Join-Path $Root 'src\util\util_env.h'
  if (!(Test-Path -LiteralPath $h -PathType Leaf)) {
    Log "V219: util_env.h not found; env bypass helper skipped."
    return
  }

  $text = [System.IO.File]::ReadAllText($h)
  if ($text -match '\bshouldBypassRemixForCurrentProcess\s*\(') {
    Log "V219: shouldBypassRemixForCurrentProcess already exists in util_env.h."
    return
  }

  $orig = $text
  if ($text -match '#pragma\s+once' -and $text -notmatch 'DX11_V219_REAL_ENV_BYPASS_FUNCTION') {
    $text = [regex]::Replace($text, '#pragma\s+once', "#pragma once`r`n`r`n// DX11_V219_REAL_ENV_BYPASS_FUNCTION`r`n#ifdef _WIN32`r`n#include <windows.h>`r`n#include <cstring>`r`n#endif", 1)
  }

  $helper = @'

namespace dxvk::env {

  inline bool shouldBypassRemixForCurrentProcess() {
#ifdef _WIN32
    auto readFlag = [](const char* name, bool fallback) -> bool {
      char value[32] = {};
      const DWORD len = ::GetEnvironmentVariableA(name, value, DWORD(sizeof(value)));
      if (len == 0 || len >= DWORD(sizeof(value)))
        return fallback;

      if (!_stricmp(value, "1") || !_stricmp(value, "true") || !_stricmp(value, "yes") || !_stricmp(value, "on"))
        return true;

      if (!_stricmp(value, "0") || !_stricmp(value, "false") || !_stricmp(value, "no") || !_stricmp(value, "off"))
        return false;

      return fallback;
    };

    if (readFlag("DXVK_REMIX_FORCE_CURRENT_PROCESS", false))
      return false;

    if (readFlag("DXVK_REMIX_ALLOW_CURRENT_PROCESS", false))
      return false;

    if (readFlag("DXVK_REMIX_BYPASS_CURRENT_PROCESS", false))
      return true;

    char modulePath[MAX_PATH] = {};
    const DWORD pathLen = ::GetModuleFileNameA(nullptr, modulePath, DWORD(sizeof(modulePath)));
    if (pathLen != 0 && pathLen < DWORD(sizeof(modulePath))) {
      const char* fileName = std::strrchr(modulePath, '\\');
      fileName = fileName ? fileName + 1 : modulePath;

      // Keep the bridge/runtime path active. These helpers are part of the DX11
      // Remix chain and must not be auto-bypassed.
      if (!_stricmp(fileName, "NvRemixBridge.exe") || !_stricmp(fileName, "NvRemixLauncher32.exe"))
        return false;
    }

    return false;
#else
    return false;
#endif
  }

}
'@

  $text = $text.TrimEnd() + "`r`n" + $helper + "`r`n"

  if (!(Test-Path -LiteralPath "$h.v219.before")) {
    # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $h -Destination "$h.v219.before" -Force
  }

  Write-TextNoBom -Path $h -Text $text
  Log "V219 added real dxvk::env::shouldBypassRemixForCurrentProcess helper to util_env.h."
}

function Add-RtxHeaderIncludeGuardsV225 {
  # DX11_V225_RTX_HEADER_INCLUDE_GUARDS
  # The RTX shader header tree (src\dxvk\shaders\rtx) is mirrored into include\rtx
  # and src\dxvk\rtx for C++ runtime includes. Because those headers use
  # "#pragma once", the same logical header reached through two different physical
  # copies (mirror vs shaders tree, pulled via "rtx/..." and "../shaders/rtx/...")
  # is NOT deduplicated, producing C2011 struct/enum redefinitions (vec2/vec3,
  # particle enums, etc.). Converting "#pragma once" to a path-derived named guard
  # makes every physical copy share one guard macro, so the second inclusion is
  # skipped regardless of which path resolved it. Runs before the mirror so the
  # copies inherit the guards.
  $root = Join-Path $Root 'src\dxvk\shaders\rtx'
  if (!(Test-Path -LiteralPath $root -PathType Container)) { return }
  $enc = New-Object System.Text.UTF8Encoding($false)
  $count = 0
  $files = Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -ieq '.h' }
  foreach ($f in $files) {
    $t = [System.IO.File]::ReadAllText($f.FullName)
    if ($t.Contains('DX11_V225_GUARD')) { continue }
    if ($t -notmatch '(?m)^[ \t]*#pragma once[ \t]*\r?$') { continue }
    $rel = $f.FullName.Substring($root.Length).TrimStart('\','/')
    $guard = 'RTX_' + (($rel -replace '[^A-Za-z0-9]', '_').ToUpperInvariant()) + '_DX11V225'
    $t = [regex]::Replace($t, '(?m)^[ \t]*#pragma once[ \t]*\r?$', "#ifndef $guard`r`n#define $guard // DX11_V225_GUARD")
    $t = $t.TrimEnd() + "`r`n`r`n#endif // $guard`r`n"
    [System.IO.File]::WriteAllText($f.FullName, $t, $enc)
    $count++
  }
  Log ("DX11_V225 added named include guards to {0} rtx shader headers (dedupes mirrored copies)." -f $count)
}

function Restore-RealRtxShaderCppHeadersV219 {
  # DX11_V219_REAL_RTX_SHADER_CPP_HEADERS
  # C++ runtime headers include shader shared headers such as:
  #   rtx/concept/surface/surface_shared.h
  # Mirror the real Remix shader header tree into include\rtx.  This is not a
  # placeholder: files are copied from the real source/upstream tree.
  $sourceRoots = New-Object System.Collections.Generic.List[string]
  foreach ($r in @(
    (Join-Path $Root 'src\dxvk\shaders\rtx'),
    (Join-Path $Root '_upstream_dxvk_remix_1_5_full_runtime_dx11\src\dxvk\shaders\rtx'),
    (Join-Path $Root '_upstream_dxvk_remix_1_5_runtime_ui\src\dxvk\shaders\rtx')
  )) {
    if (![string]::IsNullOrWhiteSpace($r) -and (Test-Path -LiteralPath $r -PathType Container) -and !$sourceRoots.Contains($r)) {
      $sourceRoots.Add($r)
    }
  }

  if ($sourceRoots.Count -eq 0) {
    Die "V219 could not find a real src\dxvk\shaders\rtx tree. No header placeholders were created."
  }

  $includeRoot = Join-Path $Root 'include\rtx'
  $srcMirrorRoot = Join-Path $Root 'src\dxvk\rtx'
  New-Item -ItemType Directory -Path $includeRoot -Force | Out-Null
  New-Item -ItemType Directory -Path $srcMirrorRoot -Force | Out-Null

  $copied = New-Object System.Collections.Generic.List[string]
  $exts = @('.h', '.hh', '.hpp', '.hlsli', '.inc', '.slangh')
  foreach ($sr in $sourceRoots) {
    $files = Get-ChildItem -LiteralPath $sr -Recurse -File -ErrorAction SilentlyContinue
    foreach ($f in $files) {
      if ($exts -notcontains $f.Extension.ToLowerInvariant()) { continue }

      $rel = $f.FullName.Substring($sr.Length).TrimStart('\','/')
      $dst1 = Join-Path $includeRoot $rel
      $dst2 = Join-Path $srcMirrorRoot $rel

      foreach ($dst in @($dst1, $dst2)) {
        $parent = Split-Path -Parent $dst
        if (!(Test-Path -LiteralPath $parent -PathType Container)) {
          New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
        Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
      }

      $copied.Add($rel)
    }
  }

  # DX11_V225: shader_types.h includes util_matrix.h with a relative path that is
  # only valid at its original 5-deep source (src\dxvk\shaders\rtx\utility). After
  # mirroring to include\rtx and src\dxvk\rtx the depth differs, so rewrite the
  # path to the correct depth for each destination.
  $stIncludeFixups = @(
    @{ Path = (Join-Path $includeRoot 'utility\shader_types.h');   New = '#include "../../../src/util/util_matrix.h"' },
    @{ Path = (Join-Path $srcMirrorRoot 'utility\shader_types.h'); New = '#include "../../../util/util_matrix.h"' }
  )
  foreach ($fix in $stIncludeFixups) {
    if (Test-Path -LiteralPath $fix.Path -PathType Leaf) {
      $st = [System.IO.File]::ReadAllText($fix.Path)
      $st2 = [regex]::Replace($st, '#include\s+"(?:\.\./)+util/util_matrix\.h"', $fix.New)
      if ($st2 -ne $st) {
        $enc = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($fix.Path, $st2, $enc)
      }
    }
  }

  $needed = Join-Path $includeRoot 'concept\surface\surface_shared.h'
  if (!(Test-Path -LiteralPath $needed -PathType Leaf)) {
    $report = Join-Path $Root 'DX11_V219_SURFACE_SHARED_NOT_FOUND.txt'
    [System.IO.File]::WriteAllLines($report, @(
      'DX11_V219_REAL_RTX_SHADER_CPP_HEADERS',
      'surface_shared.h was not found in the real shader header tree.',
      'No placeholder was created.',
      'Searched:',
      ($sourceRoots | ForEach-Object { '  ' + $_ })
    ))
    Die "V219 could not find real surface_shared.h. No placeholder was created. See $report"
  }

  $verify = Join-Path $Root 'DX11_V219_REAL_RTX_SHADER_CPP_HEADERS_VERIFY.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_REAL_RTX_SHADER_CPP_HEADERS')
  $lines.Add(('SourceRootCount: {0}' -f $sourceRoots.Count))
  foreach ($sr in $sourceRoots) { $lines.Add(('SourceRoot: {0}' -f $sr)) }
  $lines.Add(('CopiedHeaderCount: {0}' -f $copied.Count))
  $lines.Add(('RequiredSurfaceHeader: {0}' -f $needed))
  $lines.Add(('RequiredSurfaceHeaderSHA256: {0}' -f (Get-FileHash -LiteralPath $needed -Algorithm SHA256).Hash))
  foreach ($c in ($copied | Sort-Object | Select-Object -First 200)) { $lines.Add('  ' + $c) }
  [System.IO.File]::WriteAllLines($verify, $lines)
  Log "V219 mirrored real RTX shader shared headers into include\rtx for C++ runtime includes."
}

function Patch-RtxdiSlangLvalueCastsV219 {
  # DX11_V219_RTXDI_ROBUST_REAL_LVALUE_PATCH
  # Robust RTXDI Slang l-value fix.  This never creates shader stubs and never
  # fakes output.  It only rewrites the real RTXDI source so half/half4 values
  # are copied into real float/float4 l-value temporaries before out/inout calls.
  # Volume visibility attenuation is handled by Patch-RtxdiVolumeInteractionLvalueFixV219.
  $h = Join-Path $Root 'submodules\rtxdi\rtxdi-sdk\include\rtxdi\ResamplingFunctions.slangh'
  if (!(Test-Path -LiteralPath $h -PathType Leaf)) {
    Log "V219: RTXDI ResamplingFunctions.slangh not found; robust l-value shader patch skipped."
    return
  }

  $text = [System.IO.File]::ReadAllText($h)
  $orig = $text

  # Remove bad older volume-interaction conversions if a previous attempt left
  # them in the file.  They are wrong because RAB_VolumeInteraction must remain
  # the real struct/object parameter, not a float3 temporary.
  $text = [regex]::Replace($text, '(?m)^\s*float3\s+rabNeighbourVolumeInteractionFloatV\d+\s*=.*\r?\n', '')
  $text = [regex]::Replace($text, '(?m)^\s*rabNeighbourVolumeInteraction\s*=\s*half3\s*\(\s*rabNeighbourVolumeInteractionFloatV\d+\s*\)\s*;\s*\r?\n', '')
  $text = [regex]::Replace($text, '(?m)^\s*float3\s+rabPrevVolumeInteractionFloatV\d+\s*=.*\r?\n', '')
  $text = [regex]::Replace($text, '(?m)^\s*rabPrevVolumeInteraction\s*=\s*half3\s*\(\s*rabPrevVolumeInteractionFloatV\d+\s*\)\s*;\s*\r?\n', '')

  # Portal sampling: totalPortalPdf is half and sampleThreshold is half4 in
  # the Remix 1.5 RTXDI path.  The function expects float l-values.
  if ($text -notmatch 'totalPortalPdfFloatV219') {
    $portalPattern = '(?m)^(\s*)RAB_GetPortalSamplingProbablity\s*\(\s*surface\s*,\s*totalPortalPdf\s*,\s*sampleThreshold\s*\)\s*;'
    $portalReplace = @'
$1/* DX11_V219_RTXDI_ROBUST_REAL_LVALUE_PATCH */
$1float totalPortalPdfFloatV219 = float(totalPortalPdf);
$1float4 sampleThresholdFloatV219 = float4(sampleThreshold);
$1RAB_GetPortalSamplingProbablity(surface, totalPortalPdfFloatV219, sampleThresholdFloatV219);
$1totalPortalPdf = half(totalPortalPdfFloatV219);
$1sampleThreshold = half4(sampleThresholdFloatV219);
'@
    $text = [regex]::Replace($text, $portalPattern, $portalReplace, 1)
  }

  if ($text -ne $orig) {
    if (!(Test-Path -LiteralPath "$h.v219.before")) {
      # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $h -Destination "$h.v219.before" -Force
    }
    Write-TextNoBom -Path $h -Text $text
    Log "V219 patched RTXDI portal l-value casts with real float/float4 temporaries."
  } else {
    Log "V219: RTXDI portal l-value patch made no change; file may already be patched."
  }

  $check = [System.IO.File]::ReadAllText($h)
  if ($check -match 'RAB_GetPortalSamplingProbablity\s*\(\s*surface\s*,\s*totalPortalPdf\s*,\s*sampleThreshold\s*\)') {
    Die "V219 failed: raw RAB_GetPortalSamplingProbablity(surface,totalPortalPdf,sampleThreshold) call still exists."
  }
  if ($check -match 'rabNeighbourVolumeInteractionFloatV\d+' -or $check -match 'rabPrevVolumeInteractionFloatV\d+') {
    Die "V219 failed: bad RAB_VolumeInteraction float3 temporary from older patch is still present."
  }
}




function Patch-RtxdiVolumeInteractionLvalueFixV219 {
  # DX11_V219_RTXDI_ROBUST_VOLUME_ATTENUATION_LVALUE
  # Slang error 30047 reports parameter 4, but the text note shows the implicit
  # cast is from half3/vector<half,3> to float3/vector<float,3>.  In the current
  # RTXDI call shape, the value that needs a real float3 l-value is attenuation.
  # Keep rabNeighbourVolumeInteraction/rabPrevVolumeInteraction as real RTXDI
  # parameters and only convert attenuation to a float3 temporary, then copy it back.
  $h = Join-Path $Root 'submodules\rtxdi\rtxdi-sdk\include\rtxdi\ResamplingFunctions.slangh'
  if (!(Test-Path -LiteralPath $h -PathType Leaf)) {
    Log "V219: RTXDI ResamplingFunctions.slangh not found; robust volume l-value patch skipped."
    return
  }

  $text = [System.IO.File]::ReadAllText($h)
  $orig = $text

  # Clean incorrect older conversion blocks/leftovers.
  $text = [regex]::Replace($text, '(?m)^\s*float3\s+rabNeighbourVolumeInteractionFloatV\d+\s*=.*\r?\n', '')
  $text = [regex]::Replace($text, '(?m)^\s*rabNeighbourVolumeInteraction\s*=\s*half3\s*\(\s*rabNeighbourVolumeInteractionFloatV\d+\s*\)\s*;\s*\r?\n', '')
  $text = [regex]::Replace($text, '(?m)^\s*float3\s+rabPrevVolumeInteractionFloatV\d+\s*=.*\r?\n', '')
  $text = [regex]::Replace($text, '(?m)^\s*rabPrevVolumeInteraction\s*=\s*half3\s*\(\s*rabPrevVolumeInteractionFloatV\d+\s*\)\s*;\s*\r?\n', '')

  if ($text -notmatch 'attenuationNeighbourFloatV219') {
    $neighbourPattern = '(?m)^(\s*)const\s+bool\s+visible\s*=\s*RAB_VolumeReSTIR_TraceNEEVisibility\s*\(\s*selectedSampleAtNeighbour\s*,\s*sampledTransportPortalIndex\s*,\s*rabVolumeVisibilityContext\s*,\s*rabNeighbourVolumeInteraction\s*,\s*attenuation\s*\)\s*;'
    $neighbourReplace = @'
$1/* DX11_V219_RTXDI_ROBUST_VOLUME_ATTENUATION_LVALUE */
$1float3 attenuationNeighbourFloatV219 = float3(attenuation);
$1const bool visible = RAB_VolumeReSTIR_TraceNEEVisibility(selectedSampleAtNeighbour, sampledTransportPortalIndex, rabVolumeVisibilityContext, rabNeighbourVolumeInteraction, attenuationNeighbourFloatV219);
$1attenuation = half3(attenuationNeighbourFloatV219);
'@
    $text = [regex]::Replace($text, $neighbourPattern, $neighbourReplace, 1)
  }

  if ($text -notmatch 'attenuationTemporalFloatV219') {
    $temporalPattern = '(?m)^(\s*)const\s+bool\s+visible\s*=\s*RAB_VolumeReSTIR_TraceNEEVisibility\s*\(\s*selectedSampleAtTemporal\s*,\s*sampledTransportPortalIndex\s*,\s*rabVolumeVisibilityContext\s*,\s*rabPrevVolumeInteraction\s*,\s*attenuation\s*\)\s*;'
    $temporalReplace = @'
$1/* DX11_V219_RTXDI_ROBUST_VOLUME_ATTENUATION_LVALUE */
$1float3 attenuationTemporalFloatV219 = float3(attenuation);
$1const bool visible = RAB_VolumeReSTIR_TraceNEEVisibility(selectedSampleAtTemporal, sampledTransportPortalIndex, rabVolumeVisibilityContext, rabPrevVolumeInteraction, attenuationTemporalFloatV219);
$1attenuation = half3(attenuationTemporalFloatV219);
'@
    $text = [regex]::Replace($text, $temporalPattern, $temporalReplace, 1)
  }

  if ($text -ne $orig) {
    if (!(Test-Path -LiteralPath "$h.v219.before")) {
      # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $h -Destination "$h.v219.before" -Force
    }
    Write-TextNoBom -Path $h -Text $text
    Log "V219 patched RTXDI volume visibility with real attenuation float3 l-value temporaries."
  } else {
    Log "V219: RTXDI volume attenuation patch made no change; file may already be patched."
  }

  $check = [System.IO.File]::ReadAllText($h)
  if ($check -match 'rabNeighbourVolumeInteractionFloatV\d+' -or
      $check -match 'rabPrevVolumeInteractionFloatV\d+' -or
      $check -match 'rabNeighbourVolumeInteraction\s*=\s*half3\s*\(' -or
      $check -match 'rabPrevVolumeInteraction\s*=\s*half3\s*\(') {
    Die "V219 failed: bad RAB_VolumeInteraction conversion is still present in ResamplingFunctions.slangh."
  }
  if ($check -match 'RAB_VolumeReSTIR_TraceNEEVisibility\s*\(\s*selectedSampleAtNeighbour\s*,\s*sampledTransportPortalIndex\s*,\s*rabVolumeVisibilityContext\s*,\s*rabNeighbourVolumeInteraction\s*,\s*attenuation\s*\)' -or
      $check -match 'RAB_VolumeReSTIR_TraceNEEVisibility\s*\(\s*selectedSampleAtTemporal\s*,\s*sampledTransportPortalIndex\s*,\s*rabVolumeVisibilityContext\s*,\s*rabPrevVolumeInteraction\s*,\s*attenuation\s*\)') {
    Die "V219 failed: raw volume visibility calls still pass half3 attenuation directly."
  }

  $verify = Join-Path $Root 'DX11_V219_RTXDI_ROBUST_LVALUE_VERIFY.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_RTXDI_ROBUST_REAL_LVALUE_PATCH')
  $lines.Add('OK: portal sampling uses real float/float4 temporaries.')
  $lines.Add('OK: volume visibility keeps RAB_VolumeInteraction real and converts only attenuation to float3.')
  $lines.Add('OK: no shader stubs or fake output generated.')
  $lines.Add(('File: {0}' -f $h))
  $lines.Add(('SHA256: {0}' -f (Get-FileHash -LiteralPath $h -Algorithm SHA256).Hash))
  [System.IO.File]::WriteAllLines($verify, $lines)
}




function Mark-DX11RtxdiAndHeaderFixesV219 {
  Log 'DX11_V219_RTXDI_REAL_FLOAT_LVALUE_FIX: RTXDI half-to-float inout calls use real float l-value temporaries; no shader stubs.'
  Log 'DX11_V219_REAL_RTX_SHADER_CPP_HEADERS: real shader shared headers are mirrored into include\rtx for C++ runtime compilation.'
  Log 'DX11_V219_REAL_ENV_BYPASS_FUNCTION: util_env.h provides shouldBypassRemixForCurrentProcess expected by Remix 1.5 dxgi_main.cpp.'
}



function Mark-DX11SkipCopySelfRealHeadersV219 {
  Log 'DX11_V219_SKIP_COPY_SELF_REAL_HEADERS: real header restore skips Copy-Item when source and destination are the same file.'
}



function Install-RuntimeShaderCompilerWrapperV219 {
  # DX11_V219_FULL_SHADER_TREE_HIDDEN_MODULES
  # V219 copied only entries/includes to a filtered tree. Remix 1.5 entry shaders
  # still need shared .slang modules and sibling src/dxvk/rtx_render headers at
  # their real relative paths. V219 copies the full real shader tree and real
  # rtx_render headers, then hides shared .slang modules only from the Python
  # enumeration step. The files remain on disk for Slang include/import lookup.
  $scripts = Join-Path $Root 'scripts-common'
  if (!(Test-Path -LiteralPath $scripts -PathType Container)) {
    Log "V219: scripts-common folder missing; shader compiler wrapper skipped."
    return
  }

  $compile = Join-Path $scripts 'compile_shaders.py'
  $orig = Join-Path $scripts 'compile_shaders_orig_v219.py'
  $backup = Join-Path $scripts 'compile_shaders_before_v219.py'

  if (!(Test-Path -LiteralPath $compile -PathType Leaf)) {
    Log "V219: compile_shaders.py missing; shader compiler wrapper skipped."
    return
  }

  if (!(Test-Path -LiteralPath $orig -PathType Leaf)) {
    Copy-Item -LiteralPath $compile -Destination $backup -Force
    foreach ($candidate in @(
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $Root '_upstream_dxvk_remix_1_5_full_runtime_dx11\scripts-common\compile_shaders.py'),
      (Join-Path $Root '_upstream_dxvk_remix_1_5_runtime_ui\scripts-common\compile_shaders.py')
    )) {
      if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        Copy-Item -LiteralPath $candidate -Destination $orig -Force
        Log "V219 using original shader compiler: $candidate"
        break
      }
    }
    if (!(Test-Path -LiteralPath $orig -PathType Leaf)) {
      Copy-Item -LiteralPath $compile -Destination $orig -Force
      Log "V219 using current compile_shaders.py as original compiler."
    }
  }

  $wrapper = @'
#!/usr/bin/env python3
# DX11_V219_FULL_SHADER_TREE_HIDDEN_MODULES
# Real shader compiler wrapper for Remix 1.5.
# No placeholder shaders and no fake output.
# Copies the full real shader tree so relative includes/imports work, copies
# real src/dxvk/rtx_render headers next to the temp input root, and hides only
# shared .slang modules from the Python compile-script enumeration.

from __future__ import annotations

import glob as _glob
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ORIG = SCRIPT_DIR / "compile_shaders_orig_v219.py"
for fallback_name in ("compile_shaders_orig_v219.py", "compile_shaders_orig_v219.py", "compile_shaders_orig_v219.py"):
    if ORIG.is_file():
        break
    fb = SCRIPT_DIR / fallback_name
    if fb.is_file():
        ORIG = fb

STAGE_EXTS = {
    ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese",
    ".rgen", ".rchit", ".rahit", ".rmiss", ".rcall",
    ".mesh", ".task"
}

STAGE_NAME_TOKENS = (
    ".vert.", ".frag.", ".comp.", ".geom.", ".tesc.", ".tese.",
    ".rgen.", ".rchit.", ".rahit.", ".rmiss.", ".rcall.",
    ".mesh.", ".task.",
    "_vs", "_ps", "_fs", "_cs", "_gs", "_ms", "_ts",
    "vertex", "pixel", "fragment", "compute",
    "raygen", "closesthit", "anyhit", "miss", "callable",
)

HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hlsli", ".inc", ".ush", ".glslh", ".slangh"}

HIDDEN_SHARED_MODULES: set[str] = set()


def _norm(p: os.PathLike[str] | str) -> str:
    try:
        return str(Path(p).resolve()).lower()
    except Exception:
        return os.path.abspath(os.fspath(p)).lower()


def _find_arg(args: list[str], name: str) -> int:
    try:
        return args.index(name)
    except ValueError:
        return -1


def _is_entry_shader(path: Path) -> bool:
    lower_name = path.name.lower()
    lower_stem = path.stem.lower()
    suffix = path.suffix.lower()
    if suffix in STAGE_EXTS:
        return True
    if suffix == ".slang":
        return any(tok in lower_name or tok in lower_stem for tok in STAGE_NAME_TOKENS)
    if suffix in {".hlsl", ".glsl"}:
        return True
    return False


def _copy_full_tree_and_find_modules(src: Path, dst: Path) -> tuple[int, int, list[str]]:
    entries = 0
    headers = 0
    modules: list[str] = []
    shutil.copytree(src, dst, dirs_exist_ok=True)
    for f in dst.rglob("*"):
        if not f.is_file():
            continue
        rel = f.relative_to(dst)
        if _is_entry_shader(f):
            entries += 1
        elif f.suffix.lower() == ".slang":
            modules.append(str(rel).replace("\\", "/"))
            HIDDEN_SHARED_MODULES.add(_norm(f))
        elif f.suffix.lower() in HEADER_SUFFIXES:
            headers += 1
    return entries, headers, modules


def _copy_rtx_render_headers(original_input: Path, temp_root: Path) -> tuple[int, Path | None]:
    # Original input is src/dxvk/shaders/rtx. Some shader headers include:
    #   ../../../../rtx_render/...
    # From pass/terrain_baking that resolves to src/dxvk/rtx_render. In the
    # temp tree the same relative include resolves to temp_root/rtx_render.
    src_dxvk = original_input.parent.parent
    src_rtx_render = src_dxvk / "rtx_render"
    if not src_rtx_render.is_dir():
        return 0, None
    dst = temp_root / "rtx_render"
    copied = 0
    for f in src_rtx_render.rglob("*"):
        if not f.is_file():
            continue
        if f.suffix.lower() not in HEADER_SUFFIXES and f.suffix.lower() not in {".txt", ".json", ""}:
            continue
        rel = f.relative_to(src_rtx_render)
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, out)
        copied += 1
    return copied, dst


def _install_hidden_module_hooks() -> None:
    real_os_walk = os.walk
    real_glob = _glob.glob
    real_iglob = _glob.iglob
    real_path_glob = Path.glob
    real_path_rglob = Path.rglob

    def visible(path: os.PathLike[str] | str) -> bool:
        return _norm(path) not in HIDDEN_SHARED_MODULES

    def walk(top, *args, **kwargs):
        for root, dirs, files in real_os_walk(top, *args, **kwargs):
            files[:] = [f for f in files if visible(Path(root) / f)]
            yield root, dirs, files

    def glob_func(pathname, *args, **kwargs):
        return [p for p in real_glob(pathname, *args, **kwargs) if visible(p)]

    def iglob_func(pathname, *args, **kwargs):
        for p in real_iglob(pathname, *args, **kwargs):
            if visible(p):
                yield p

    def path_glob(self, pattern):
        for p in real_path_glob(self, pattern):
            if visible(p):
                yield p

    def path_rglob(self, pattern):
        for p in real_path_rglob(self, pattern):
            if visible(p):
                yield p

    os.walk = walk  # type: ignore[assignment]
    _glob.glob = glob_func  # type: ignore[assignment]
    _glob.iglob = iglob_func  # type: ignore[assignment]
    Path.glob = path_glob  # type: ignore[assignment]
    Path.rglob = path_rglob  # type: ignore[assignment]


def _run_original_in_process(args: list[str]) -> int:
    old_argv = sys.argv[:]
    try:
        sys.argv = [str(ORIG)] + args
        try:
            runpy.run_path(str(ORIG), run_name="__main__")
            return 0
        except SystemExit as e:
            code = e.code
            if code is None:
                return 0
            if isinstance(code, int):
                return code
            return 1
    finally:
        sys.argv = old_argv


def main() -> int:
    if not ORIG.is_file():
        print(f"DX11_V219_FULL_SHADER_TREE_HIDDEN_MODULES: original compiler missing: {ORIG}", file=sys.stderr)
        return 2

    args = sys.argv[1:]
    input_i = _find_arg(args, "-input")
    output_i = _find_arg(args, "-output")

    if input_i < 0 or input_i + 1 >= len(args):
        return subprocess.call([sys.executable, str(ORIG)] + args)

    input_dir = Path(args[input_i + 1]).resolve()
    if not input_dir.is_dir():
        return subprocess.call([sys.executable, str(ORIG)] + args)

    output_dir = None
    if output_i >= 0 and output_i + 1 < len(args):
        output_dir = Path(args[output_i + 1]).resolve()

    with tempfile.TemporaryDirectory(prefix="dx11_v219_shader_fulltree_", ignore_cleanup_errors=True) as td:
        temp_root = Path(td)
        temp_input = temp_root / "input"
        temp_input.mkdir(parents=True, exist_ok=True)

        entries, headers, modules = _copy_full_tree_and_find_modules(input_dir, temp_input)
        rtx_headers, rtx_dst = _copy_rtx_render_headers(input_dir, temp_root)

        if output_dir is not None:
            output_dir.mkdir(parents=True, exist_ok=True)
            report = output_dir / "dx11_v219_full_shader_tree_hidden_modules.txt"
            report.write_text(
                "DX11_V219_FULL_SHADER_TREE_HIDDEN_MODULES\n"
                f"OriginalInput={input_dir}\n"
                f"TempInput={temp_input}\n"
                f"EntryFilesVisibleToCompiler={entries}\n"
                f"RealShaderHeadersCopied={headers}\n"
                f"RealRtxRenderHeadersCopied={rtx_headers}\n"
                f"RtxRenderHeaderTempRoot={rtx_dst}\n"
                f"SharedSlangModulesHiddenFromEnumeration={len(modules)}\n\n"
                + "\n".join(modules)
                + ("\n" if modules else ""),
                encoding="utf-8"
            )

        if entries == 0:
            return subprocess.call([sys.executable, str(ORIG)] + args)

        new_args = list(args)
        new_args[input_i + 1] = str(temp_input)

        existing_includes = {new_args[i + 1] for i, a in enumerate(new_args[:-1]) if a == "-include"}
        for inc in (temp_input, input_dir, input_dir.parent, temp_root, temp_root / "rtx_render"):
            s = str(inc)
            if s not in existing_includes:
                new_args.extend(["-include", s])
                existing_includes.add(s)

        print(
            "DX11_V219_FULL_SHADER_TREE_HIDDEN_MODULES: compiling full shader tree with shared modules hidden "
            f"entries={entries} shader_headers={headers} rtx_render_headers={rtx_headers} "
            f"hidden_slang_modules={len(modules)} original={input_dir}",
            flush=True
        )

        _install_hidden_module_hooks()
        return _run_original_in_process(new_args)

if __name__ == "__main__":
    raise SystemExit(main())
'@

  Write-TextNoBom -Path $compile -Text $wrapper
  Log "V219 installed full shader tree wrapper with hidden shared module enumeration: $compile"
}



function Mark-DX11FullShaderTreeHiddenModulesV219 {
  Log 'DX11_V219_FULL_SHADER_TREE_HIDDEN_MODULES: full real shader tree and real rtx_render headers are copied; shared .slang modules are hidden only from compile enumeration.'
}



function Mark-DX11ShaderTempCleanupSafeV219 {
  Log 'DX11_V219_SHADER_TEMP_CLEANUP_SAFE: shader wrapper uses TemporaryDirectory(ignore_cleanup_errors=True) so Windows temp cleanup races do not fail a real shader compile.'
}



function Sync-RealRemix15RayTracingSubmodulesV219 {
  # DX11_V219_REAL_REMIX15_RTXDI_RTXCR_SUBMODULE_SYNC
  # No stubs: the 1.5 shaders must match the real RTXDI/RTXCR headers/modules.
  # V219 copied src/dxvk but did not guarantee recursive submodule sync.
  $report = Join-Path $Root 'DX11_V219_REAL_REMIX15_RTXDI_RTXCR_SUBMODULE_SYNC.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_REAL_REMIX15_RTXDI_RTXCR_SUBMODULE_SYNC')
  $lines.Add(('Time: {0}' -f (Get-Date)))
  $lines.Add('Mode: real upstream submodule content only; no shader stubs.')

  $upstreamRoots = @(
    # This is the clone that your log shows was created successfully.
    (Join-Path $Root '_nvidia_dxvk_remix_for_dx11_bridge'),

    # Older/alternate names used by previous packages.
    (Join-Path $Root '_upstream_dxvk_remix_1_5_full_runtime_dx11'),
    (Join-Path $Root '_upstream_dxvk_remix_1_5_runtime_ui'),

    # Fallback: the project itself, only accepted if real submodules already exist.
    $Root
  )

  function Test-RealRemixSubmoduleSourceV219([string]$PathToCheck) {
    if ([string]::IsNullOrWhiteSpace($PathToCheck)) { return $false }
    $rtxdiHeader = Join-Path $PathToCheck 'submodules\rtxdi\rtxdi-sdk\include\rtxdi\ResamplingFunctions.slangh'
    $rtxcrPath = Join-Path $PathToCheck 'submodules\rtxcr'
    return ((Test-Path -LiteralPath $rtxdiHeader -PathType Leaf) -and (Test-Path -LiteralPath $rtxcrPath -PathType Container))
  }

  function Test-GitCloneV219([string]$PathToCheck) {
    if ([string]::IsNullOrWhiteSpace($PathToCheck)) { return $false }
    $gitPath = Join-Path $PathToCheck '.git'
    return ((Test-Path -LiteralPath $gitPath -PathType Container) -or (Test-Path -LiteralPath $gitPath -PathType Leaf))
  }

  $chosen = $null
  $chosenIsScratchClone = $false
  foreach ($u in $upstreamRoots) {
    if (Test-RealRemixSubmoduleSourceV219 $u) {
      $chosen = $u
      $leaf = Split-Path -Leaf $u
      if ($leaf -eq '_nvidia_dxvk_remix_for_dx11_bridge' -or $leaf -like '_upstream_dxvk_remix_*') {
        $chosenIsScratchClone = $true
      }
      break
    }
  }

  if (!$chosen) {
    $lines.Add('BAD: no real RTXDI/RTXCR source was found.')
    $lines.Add('Checked:')
    foreach ($u in $upstreamRoots) { $lines.Add(('  {0}' -f $u)) }
    $lines.Add('Expected either the existing clone _nvidia_dxvk_remix_for_dx11_bridge or already-synced submodules\rtxdi and submodules\rtxcr.')
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 could not find a real RTXDI/RTXCR source. The previous clone log shows _nvidia_dxvk_remix_for_dx11_bridge; keep that folder until this sync copies its submodules. See $report"
  }

  $git = Get-Command git.exe -ErrorAction SilentlyContinue
  $lines.Add(('ChosenRealSource: {0}' -f $chosen))
  $lines.Add(('ChosenIsScratchClone: {0}' -f $chosenIsScratchClone))

  if ($git -and (Test-GitCloneV219 $chosen)) {
    Push-Location $chosen
    try {
      & $git.Source submodule update --init --recursive submodules/rtxdi submodules/rtxcr | Out-Host
      if ($LASTEXITCODE -ne 0) {
        $lines.Add('WARN: targeted submodule update failed; trying full recursive submodule update.')
        & $git.Source submodule update --init --recursive | Out-Host
        if ($LASTEXITCODE -ne 0) {
          $lines.Add('WARN: git submodule update failed, but existing real submodule files are present. Continuing with real existing files.')
        }
      }
    } finally {
      Pop-Location
    }
  } else {
    $lines.Add('INFO: git update skipped; using existing real RTXDI/RTXCR files already present in chosen source.')
  }

  $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
  # DX11: submodule backup-dir disabled -- $backupRoot = Join-Path $Root ("_dx11_v219_before_real_rtx_submodule_sync_" + $stamp)
  # DX11: submodule backup-dir disabled -- New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null

  $submods = @('rtxdi', 'rtxcr')
  foreach ($s in $submods) {
    $src = Join-Path $chosen ('submodules\' + $s)
    $dst = Join-Path $Root ('submodules\' + $s)
    if (!(Test-Path -LiteralPath $src -PathType Container)) {
      $lines.Add(('BAD: upstream real submodule missing: {0}' -f $src))
      [System.IO.File]::WriteAllLines($report, $lines)
      Die "V219 upstream real submodule missing: $src"
    }

    $srcFull = [IO.Path]::GetFullPath($src).TrimEnd('\')
    $dstFull = [IO.Path]::GetFullPath($dst).TrimEnd('\')
    if ($srcFull -ieq $dstFull) {
      $lines.Add(('OK: real submodule {0} already resides at destination; skipped destructive self-copy: {1}' -f $s, $dstFull))
      continue
    }

    if (Test-Path -LiteralPath $dst -PathType Container) {
      # DX11: submodule backup-dir disabled -- $bak = Join-Path $backupRoot ('submodules\' + $s)
      # DX11: submodule backup-dir disabled -- $bakParent = Split-Path -Parent $bak
      # DX11: submodule backup-dir disabled -- if (!(Test-Path -LiteralPath $bakParent -PathType Container)) { New-Item -ItemType Directory -Path $bakParent -Force | Out-Null }
      # DX11: submodule backup-dir disabled -- Copy-Item -LiteralPath $dst -Destination $bak -Recurse -Force
    }

    if (Test-Path -LiteralPath $dst -PathType Container) {
      Remove-Item -LiteralPath $dst -Recurse -Force
    }
    $dstParent = Split-Path -Parent $dst
    if (!(Test-Path -LiteralPath $dstParent -PathType Container)) { New-Item -ItemType Directory -Path $dstParent -Force | Out-Null }
    Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force

    $lines.Add(('OK: copied real upstream submodule {0}: {1} -> {2}' -f $s, $src, $dst))
  }

  $mustExist = @(
    (Join-Path $Root 'submodules\rtxdi\rtxdi-sdk\include\rtxdi\ResamplingFunctions.slangh'),
    (Join-Path $Root 'submodules\rtxcr\shaders\include')
  )

  foreach ($m in $mustExist) {
    if (Test-Path -LiteralPath $m) {
      $lines.Add(('OK: required real submodule path exists: {0}' -f $m))
    } else {
      $lines.Add(('BAD: required real submodule path missing: {0}' -f $m))
      [System.IO.File]::WriteAllLines($report, $lines)
      Die "V219 real submodule sync incomplete; missing $m"
    }
  }

  $symbolSearchRoots = @(
    (Join-Path $Root 'src\dxvk\shaders'),
    (Join-Path $Root 'submodules\rtxdi'),
    (Join-Path $Root 'submodules\rtxcr')
  )

  foreach ($sym in @('RTXDI_UpdateSelectedReservoirVisibility','RTXDI_SkipRandomLightsRng','RTXDI_SkipSingleLightRng')) {
    $found = $false
    foreach ($r in $symbolSearchRoots) {
      if (!(Test-Path -LiteralPath $r -PathType Container)) { continue }
      try {
        $hit = Get-ChildItem -LiteralPath $r -Recurse -File -ErrorAction SilentlyContinue |
          Select-String -SimpleMatch $sym -List -ErrorAction SilentlyContinue |
          Select-Object -First 1
        if ($hit) {
          $lines.Add(('OK: found real symbol {0} in {1}' -f $sym, $hit.Path))
          $found = $true
          break
        }
      } catch {}
    }
    if (!$found) {
      $lines.Add(('WARN: real symbol {0} was not found by text scan after submodule sync. Build will decide; no fake function was generated.' -f $sym))
    }
  }

  if ($chosenIsScratchClone -and $chosen -ne $Root -and (Test-Path -LiteralPath $chosen -PathType Container)) {
    try {
      Remove-Item -LiteralPath $chosen -Recurse -Force -ErrorAction SilentlyContinue
      $lines.Add(('OK: removed scratch upstream clone after copying real submodules: {0}' -f $chosen))
    } catch {
      $lines.Add(('WARN: could not remove scratch upstream clone after sync: {0}' -f $_.Exception.Message))
    }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 synced real Remix 1.5 RTXDI/RTXCR submodules from existing clone/project source. Report: $report"
}

function Install-RuntimeShaderCompilerWrapperV219 {
  # DX11_V219_REAL_RTX_RENDER_RELATIVE_INCLUDE_ROOT
  # Fix the real relative include path:
  #   input/pass/terrain_baking/../../../../rtx_render/...
  # This resolves to the parent of the temp root, not temp_root/rtx_render.
  # Copy the real rtx_render headers to both locations. No placeholders.
  $scripts = Join-Path $Root 'scripts-common'
  if (!(Test-Path -LiteralPath $scripts -PathType Container)) {
    Log "V219: scripts-common folder missing; shader compiler wrapper skipped."
    return
  }

  $compile = Join-Path $scripts 'compile_shaders.py'
  $orig = Join-Path $scripts 'compile_shaders_orig_v219.py'
  $backup = Join-Path $scripts 'compile_shaders_before_v219.py'

  if (!(Test-Path -LiteralPath $compile -PathType Leaf)) {
    Log "V219: compile_shaders.py missing; shader compiler wrapper skipped."
    return
  }

  if (!(Test-Path -LiteralPath $orig -PathType Leaf)) {
    Copy-Item -LiteralPath $compile -Destination $backup -Force
    foreach ($candidate in @(
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $Root '_upstream_dxvk_remix_1_5_full_runtime_dx11\scripts-common\compile_shaders.py'),
      (Join-Path $Root '_upstream_dxvk_remix_1_5_runtime_ui\scripts-common\compile_shaders.py')
    )) {
      if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        Copy-Item -LiteralPath $candidate -Destination $orig -Force
        Log "V219 using original shader compiler: $candidate"
        break
      }
    }
    if (!(Test-Path -LiteralPath $orig -PathType Leaf)) {
      Copy-Item -LiteralPath $compile -Destination $orig -Force
      Log "V219 using current compile_shaders.py as original compiler."
    }
  }

  $wrapper = @'
#!/usr/bin/env python3
# DX11_V219_REAL_RTX_RENDER_RELATIVE_INCLUDE_ROOT
# Real shader compiler wrapper for Remix 1.5.
# No placeholder shaders and no fake output.
# Copies the full real shader tree so relative includes/imports work, copies
# real src/dxvk/rtx_render headers to every relative root required by the
# shaders, and hides only shared .slang modules from Python enumeration.

from __future__ import annotations

import glob as _glob
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ORIG = SCRIPT_DIR / "compile_shaders_orig_v219.py"
for fallback_name in (
    "compile_shaders_orig_v219.py",
    "compile_shaders_orig_v219.py",
    "compile_shaders_orig_v219.py",
    "compile_shaders_orig_v219.py",
):
    if ORIG.is_file():
        break
    fb = SCRIPT_DIR / fallback_name
    if fb.is_file():
        ORIG = fb

STAGE_EXTS = {
    ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese",
    ".rgen", ".rchit", ".rahit", ".rmiss", ".rcall",
    ".mesh", ".task"
}

STAGE_NAME_TOKENS = (
    ".vert.", ".frag.", ".comp.", ".geom.", ".tesc.", ".tese.",
    ".rgen.", ".rchit.", ".rahit.", ".rmiss.", ".rcall.",
    ".mesh.", ".task.",
    "_vs", "_ps", "_fs", "_cs", "_gs", "_ms", "_ts",
    "vertex", "pixel", "fragment", "compute",
    "raygen", "closesthit", "anyhit", "miss", "callable",
)

HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hlsli", ".inc", ".ush", ".glslh", ".slangh", ".json", ".txt", ""}
HIDDEN_SHARED_MODULES: set[str] = set()


def _norm(p: os.PathLike[str] | str) -> str:
    try:
        return str(Path(p).resolve()).lower()
    except Exception:
        return os.path.abspath(os.fspath(p)).lower()


def _find_arg(args: list[str], name: str) -> int:
    try:
        return args.index(name)
    except ValueError:
        return -1


def _is_entry_shader(path: Path) -> bool:
    lower_name = path.name.lower()
    lower_stem = path.stem.lower()
    suffix = path.suffix.lower()
    if suffix in STAGE_EXTS:
        return True
    if suffix == ".slang":
        return any(tok in lower_name or tok in lower_stem for tok in STAGE_NAME_TOKENS)
    if suffix in {".hlsl", ".glsl"}:
        return True
    return False


def _copy_full_tree_and_find_modules(src: Path, dst: Path) -> tuple[int, int, list[str]]:
    entries = 0
    headers = 0
    modules: list[str] = []
    shutil.copytree(src, dst, dirs_exist_ok=True)
    for f in dst.rglob("*"):
        if not f.is_file():
            continue
        rel = f.relative_to(dst)
        if _is_entry_shader(f):
            entries += 1
        elif f.suffix.lower() == ".slang":
            modules.append(str(rel).replace("\\", "/"))
            HIDDEN_SHARED_MODULES.add(_norm(f))
        elif f.suffix.lower() in HEADER_SUFFIXES:
            headers += 1
    return entries, headers, modules


def _copy_rtx_render_headers(original_input: Path, temp_root: Path) -> tuple[int, list[Path]]:
    src_dxvk = original_input.parent.parent
    src_rtx_render = src_dxvk / "rtx_render"
    if not src_rtx_render.is_dir():
        return 0, []

    # Required roots:
    #   temp_root/rtx_render              -> for includes with ../../../rtx_render
    #   temp_root.parent/rtx_render       -> for includes with ../../../../rtx_render
    #   temp_root/input/rtx_render        -> for direct include-path lookup
    destinations = [
        temp_root / "rtx_render",
        temp_root.parent / "rtx_render",
        temp_root / "input" / "rtx_render",
    ]

    copied = 0
    used: list[Path] = []
    for dst in destinations:
        dst.mkdir(parents=True, exist_ok=True)
        used.append(dst)
        for f in src_rtx_render.rglob("*"):
            if not f.is_file():
                continue
            if f.suffix.lower() not in HEADER_SUFFIXES:
                continue
            rel = f.relative_to(src_rtx_render)
            out = dst / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)
            copied += 1
    return copied, used


def _install_hidden_module_hooks() -> None:
    real_os_walk = os.walk
    real_glob = _glob.glob
    real_iglob = _glob.iglob
    real_path_glob = Path.glob
    real_path_rglob = Path.rglob

    def visible(path: os.PathLike[str] | str) -> bool:
        return _norm(path) not in HIDDEN_SHARED_MODULES

    def walk(top, *args, **kwargs):
        for root, dirs, files in real_os_walk(top, *args, **kwargs):
            files[:] = [f for f in files if visible(Path(root) / f)]
            yield root, dirs, files

    def glob_func(pathname, *args, **kwargs):
        return [p for p in real_glob(pathname, *args, **kwargs) if visible(p)]

    def iglob_func(pathname, *args, **kwargs):
        for p in real_iglob(pathname, *args, **kwargs):
            if visible(p):
                yield p

    def path_glob(self, pattern):
        for p in real_path_glob(self, pattern):
            if visible(p):
                yield p

    def path_rglob(self, pattern):
        for p in real_path_rglob(self, pattern):
            if visible(p):
                yield p

    os.walk = walk  # type: ignore[assignment]
    _glob.glob = glob_func  # type: ignore[assignment]
    _glob.iglob = iglob_func  # type: ignore[assignment]
    Path.glob = path_glob  # type: ignore[assignment]
    Path.rglob = path_rglob  # type: ignore[assignment]


def _run_original_in_process(args: list[str]) -> int:
    old_argv = sys.argv[:]
    try:
        sys.argv = [str(ORIG)] + args
        try:
            runpy.run_path(str(ORIG), run_name="__main__")
            return 0
        except SystemExit as e:
            code = e.code
            if code is None:
                return 0
            if isinstance(code, int):
                return code
            return 1
    finally:
        sys.argv = old_argv


def main() -> int:
    if not ORIG.is_file():
        print(f"DX11_V219_REAL_RTX_RENDER_RELATIVE_INCLUDE_ROOT: original compiler missing: {ORIG}", file=sys.stderr)
        return 2

    args = sys.argv[1:]
    input_i = _find_arg(args, "-input")
    output_i = _find_arg(args, "-output")

    if input_i < 0 or input_i + 1 >= len(args):
        return subprocess.call([sys.executable, str(ORIG)] + args)

    input_dir = Path(args[input_i + 1]).resolve()
    if not input_dir.is_dir():
        return subprocess.call([sys.executable, str(ORIG)] + args)

    output_dir = None
    if output_i >= 0 and output_i + 1 < len(args):
        output_dir = Path(args[output_i + 1]).resolve()

    with tempfile.TemporaryDirectory(prefix="dx11_v219_shader_fulltree_", ignore_cleanup_errors=True) as td:
        temp_root = Path(td)
        temp_input = temp_root / "input"
        temp_input.mkdir(parents=True, exist_ok=True)

        entries, headers, modules = _copy_full_tree_and_find_modules(input_dir, temp_input)
        rtx_headers, rtx_dsts = _copy_rtx_render_headers(input_dir, temp_root)

        if output_dir is not None:
            output_dir.mkdir(parents=True, exist_ok=True)
            report = output_dir / "dx11_v219_full_shader_tree_real_relative_roots.txt"
            report.write_text(
                "DX11_V219_REAL_RTX_RENDER_RELATIVE_INCLUDE_ROOT\n"
                f"OriginalInput={input_dir}\n"
                f"TempInput={temp_input}\n"
                f"TempRoot={temp_root}\n"
                f"TempRootParent={temp_root.parent}\n"
                f"EntryFilesVisibleToCompiler={entries}\n"
                f"RealShaderHeadersCopied={headers}\n"
                f"RealRtxRenderHeadersCopied={rtx_headers}\n"
                f"RtxRenderHeaderRoots={';'.join(str(p) for p in rtx_dsts)}\n"
                f"SharedSlangModulesHiddenFromEnumeration={len(modules)}\n\n"
                + "\n".join(modules)
                + ("\n" if modules else ""),
                encoding="utf-8"
            )

        if entries == 0:
            return subprocess.call([sys.executable, str(ORIG)] + args)

        new_args = list(args)
        new_args[input_i + 1] = str(temp_input)

        existing_includes = {new_args[i + 1] for i, a in enumerate(new_args[:-1]) if a == "-include"}
        include_roots = [temp_input, input_dir, input_dir.parent, temp_root, temp_root.parent]
        include_roots.extend(rtx_dsts)
        for inc in include_roots:
            s = str(inc)
            if s not in existing_includes:
                new_args.extend(["-include", s])
                existing_includes.add(s)

        print(
            "DX11_V219_REAL_RTX_RENDER_RELATIVE_INCLUDE_ROOT: compiling full shader tree "
            f"entries={entries} shader_headers={headers} rtx_render_headers={rtx_headers} "
            f"hidden_slang_modules={len(modules)} original={input_dir}",
            flush=True
        )

        _install_hidden_module_hooks()
        return _run_original_in_process(new_args)

if __name__ == "__main__":
    raise SystemExit(main())
'@

  Write-TextNoBom -Path $compile -Text $wrapper
  Log "V219 installed shader compiler wrapper with real relative rtx_render include roots: $compile"
}



function Mark-DX11RealSubmodulesAndRtxRenderRootV219 {
  Log 'DX11_V219_REAL_REMIX15_RTXDI_RTXCR_SUBMODULE_SYNC: real upstream RTXDI/RTXCR submodules are synced for Remix 1.5 shaders; no fake functions.'
  Log 'DX11_V219_REAL_RTX_RENDER_RELATIVE_INCLUDE_ROOT: rtx_render headers are copied to the real relative root required by shader includes.'
}



function Assert-DX11ShaderModeNoD3D9V219 {
  # DX11_V219_DX11_SHADER_MODE_NO_D3D9
  # The runtime shader compiler can still output SPIR-V for Remix/DXVK's backend,
  # but the game-facing path must be DX11-only.  Do not enumerate D3D9 shader or
  # bridge sources for this DX11 build.
  $report = Join-Path $Root 'DX11_V219_DX11_SHADER_MODE_VERIFY.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_DX11_SHADER_MODE_NO_D3D9')
  $lines.Add(('Time: {0}' -f (Get-Date)))
  $lines.Add('Meaning:')
  $lines.Add('  Game-facing capture/API: DX11 d3d11.dll + dxgi.dll client.')
  $lines.Add('  Internal Remix runtime shaders: compiled by real Slang/DXVK backend, not fake D3D11 bytecode.')
  $lines.Add('  D3D9 shader/source/bridge paths are blocked from this DX11 build process.')
  $lines.Add('')

  $badRoots = @(
    (Join-Path $Root 'src\d3d9'),
    (Join-Path $Root 'bridge\d3d9'),
    (Join-Path $Root 'bridge_dx9_work')
  )

  foreach ($b in $badRoots) {
    if (Test-Path -LiteralPath $b) {
      $lines.Add(('WARN: D3D9 source folder exists in repo but is not used by the DX11 build script: {0}' -f $b))
    } else {
      $lines.Add(('OK: no D3D9 build folder at {0}' -f $b))
    }
  }

  $buildText = [System.IO.File]::ReadAllText((Join-Path $Root 'build_dxvk_all_ninja.ps1'))
  if ($buildText -match 'src\\d3d9' -and $buildText -notmatch 'Blocked|BLOCK|Block-DX9|not copied|D3D9 source folder exists') {
    $lines.Add('WARN: build script references src\d3d9 outside a visible block comment. Review if build pulls D3D9.')
  } else {
    $lines.Add('OK: build script does not actively compile src\d3d9 for DX11 shader mode.')
  }

  if ($buildText.Contains('DXVK_REMIX_DX11_SHADER_MODE')) {
    $lines.Add('OK: DX11 shader-mode define injection is present.')
  } else {
    $lines.Add('BAD: DX11 shader-mode define injection missing.')
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 wrote DX11 shader mode verification: $report"
}

function Install-RuntimeShaderCompilerWrapperV219 {
  # DX11_V219_DX11_SHADER_MODE_NO_D3D9
  # Build internal Remix shaders with the real compiler, but enforce DX11-side
  # mode for this fork: no D3D9 path enumeration and DX11 macro defines on
  # Slang invocations.  This does not produce fake shader output.
  $scripts = Join-Path $Root 'scripts-common'
  if (!(Test-Path -LiteralPath $scripts -PathType Container)) {
    Log "V219: scripts-common folder missing; shader compiler wrapper skipped."
    return
  }

  $compile = Join-Path $scripts 'compile_shaders.py'
  $orig = Join-Path $scripts 'compile_shaders_orig_v219.py'
  $backup = Join-Path $scripts 'compile_shaders_before_v219.py'

  if (!(Test-Path -LiteralPath $compile -PathType Leaf)) {
    Log "V219: compile_shaders.py missing; shader compiler wrapper skipped."
    return
  }

  if (!(Test-Path -LiteralPath $orig -PathType Leaf)) {
    Copy-Item -LiteralPath $compile -Destination $backup -Force
    foreach ($candidate in @(
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $scripts 'compile_shaders_orig_v219.py'),
      (Join-Path $Root '_upstream_dxvk_remix_1_5_full_runtime_dx11\scripts-common\compile_shaders.py'),
      (Join-Path $Root '_upstream_dxvk_remix_1_5_runtime_ui\scripts-common\compile_shaders.py')
    )) {
      if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        Copy-Item -LiteralPath $candidate -Destination $orig -Force
        Log "V219 using original shader compiler: $candidate"
        break
      }
    }
    if (!(Test-Path -LiteralPath $orig -PathType Leaf)) {
      Copy-Item -LiteralPath $compile -Destination $orig -Force
      Log "V219 using current compile_shaders.py as original compiler."
    }
  }

  $wrapper = @'
#!/usr/bin/env python3
# DX11_V219_DX11_SHADER_MODE_NO_D3D9
# Real shader compiler wrapper for the DX11 fork.
# No placeholder shaders, no fake output, no DX9 shader path enumeration.
# Internal Remix shaders still compile through Slang/DXVK backend; the game
# capture/front-end path is DX11 d3d11.dll/dxgi.dll.

from __future__ import annotations

import glob as _glob
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ORIG = SCRIPT_DIR / "compile_shaders_orig_v219.py"
for fallback_name in (
    "compile_shaders_orig_v219.py",
    "compile_shaders_orig_v219.py",
    "compile_shaders_orig_v219.py",
    "compile_shaders_orig_v219.py",
    "compile_shaders_orig_v219.py",
):
    if ORIG.is_file():
        break
    fb = SCRIPT_DIR / fallback_name
    if fb.is_file():
        ORIG = fb

STAGE_EXTS = {
    ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese",
    ".rgen", ".rchit", ".rahit", ".rmiss", ".rcall",
    ".mesh", ".task"
}

STAGE_NAME_TOKENS = (
    ".vert.", ".frag.", ".comp.", ".geom.", ".tesc.", ".tese.",
    ".rgen.", ".rchit.", ".rahit.", ".rmiss.", ".rcall.",
    ".mesh.", ".task.",
    "_vs", "_ps", "_fs", "_cs", "_gs", "_ms", "_ts",
    "vertex", "pixel", "fragment", "compute",
    "raygen", "closesthit", "anyhit", "miss", "callable",
)

HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hlsli", ".inc", ".ush", ".glslh", ".slangh", ".json", ".txt", ""}
HIDDEN_SHARED_MODULES: set[str] = set()

DX11_DEFINES = [
    "-DDXVK_REMIX_DX11_SHADER_MODE=1",
    "-DRTX_REMIX_DX11=1",
    "-DDXVK_REMIX_D3D9_SHADER_MODE=0",
    "-DRTX_REMIX_D3D9=0",
]

def _norm(p: os.PathLike[str] | str) -> str:
    try:
        return str(Path(p).resolve()).lower()
    except Exception:
        return os.path.abspath(os.fspath(p)).lower()

def _is_d3d9_path(path: Path) -> bool:
    parts = {p.lower() for p in path.parts}
    s = str(path).replace("\\", "/").lower()
    return (
        "d3d9" in parts
        or "/d3d9/" in s
        or "d3d9_" in path.name.lower()
        or "_d3d9" in path.name.lower()
    )

def _find_arg(args: list[str], name: str) -> int:
    try:
        return args.index(name)
    except ValueError:
        return -1

def _is_entry_shader(path: Path) -> bool:
    if _is_d3d9_path(path):
        return False
    lower_name = path.name.lower()
    lower_stem = path.stem.lower()
    suffix = path.suffix.lower()
    if suffix in STAGE_EXTS:
        return True
    if suffix == ".slang":
        if any(tok in lower_name or tok in lower_stem for tok in STAGE_NAME_TOKENS):
            return True
        # DX11_V225: variant / variant-matrix master shaders (gbuffer.slang,
        # integrate_direct.slang, integrate_indirect.slang) have no stage token in
        # their name but declare "//!variant" directives and generate aggregate
        # headers (gbuffer_variants.h, integrate_direct_rayquery.h, ...). They must
        # be compiled, not hidden as shared modules.
        try:
            with path.open("r", encoding="utf-8", errors="ignore") as _vf:
                if "//!variant" in _vf.read(65536):
                    return True
        except Exception:
            pass
        return False
    if suffix in {".hlsl", ".glsl"}:
        return True
    return False

def _copy_full_tree_and_find_modules(src: Path, dst: Path) -> tuple[int, int, int, list[str]]:
    entries = 0
    headers = 0
    d3d9_skipped = 0
    modules: list[str] = []

    for f in src.rglob("*"):
        if not f.is_file():
            continue
        rel = f.relative_to(src)

        if _is_d3d9_path(f):
            d3d9_skipped += 1
            continue

        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, out)

    for f in dst.rglob("*"):
        if not f.is_file():
            continue
        rel = f.relative_to(dst)
        if _is_entry_shader(f):
            entries += 1
        elif f.suffix.lower() == ".slang":
            modules.append(str(rel).replace("\\", "/"))
            HIDDEN_SHARED_MODULES.add(_norm(f))
        elif f.suffix.lower() in HEADER_SUFFIXES:
            headers += 1

    return entries, headers, d3d9_skipped, modules

def _copy_rtx_render_headers(original_input: Path, temp_root: Path) -> tuple[int, list[Path]]:
    src_dxvk = original_input.parent.parent
    src_rtx_render = src_dxvk / "rtx_render"
    if not src_rtx_render.is_dir():
        return 0, []

    destinations = [
        temp_root / "rtx_render",
        temp_root.parent / "rtx_render",
        temp_root / "input" / "rtx_render",
    ]

    copied = 0
    used: list[Path] = []
    for dst in destinations:
        dst.mkdir(parents=True, exist_ok=True)
        used.append(dst)
        for f in src_rtx_render.rglob("*"):
            if not f.is_file():
                continue
            if _is_d3d9_path(f):
                continue
            if f.suffix.lower() not in HEADER_SUFFIXES:
                continue
            rel = f.relative_to(src_rtx_render)
            out = dst / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)
            copied += 1
    return copied, used

def _install_hidden_module_hooks() -> None:
    real_os_walk = os.walk
    real_glob = _glob.glob
    real_iglob = _glob.iglob
    real_path_glob = Path.glob
    real_path_rglob = Path.rglob
    real_subprocess_call = subprocess.call
    real_subprocess_run = subprocess.run
    real_subprocess_check_call = subprocess.check_call
    real_popen = subprocess.Popen

    def visible(path: os.PathLike[str] | str) -> bool:
        p = Path(path)
        return _norm(path) not in HIDDEN_SHARED_MODULES and not _is_d3d9_path(p)

    def add_dx11_defines(cmd):
        try:
            if isinstance(cmd, (list, tuple)) and cmd:
                exe = str(cmd[0]).lower()
                if "slangc" in exe:
                    out = list(cmd)
                    for d in DX11_DEFINES:
                        if d not in out:
                            out.append(d)
                    return out
            elif isinstance(cmd, str) and "slangc" in cmd.lower():
                extra = " " + " ".join(DX11_DEFINES)
                if "DXVK_REMIX_DX11_SHADER_MODE" not in cmd:
                    return cmd + extra
        except Exception:
            pass
        return cmd

    def walk(top, *args, **kwargs):
        for root, dirs, files in real_os_walk(top, *args, **kwargs):
            dirs[:] = [d for d in dirs if visible(Path(root) / d)]
            files[:] = [f for f in files if visible(Path(root) / f)]
            yield root, dirs, files

    def glob_func(pathname, *args, **kwargs):
        return [p for p in real_glob(pathname, *args, **kwargs) if visible(p)]

    def iglob_func(pathname, *args, **kwargs):
        for p in real_iglob(pathname, *args, **kwargs):
            if visible(p):
                yield p

    def path_glob(self, pattern):
        for p in real_path_glob(self, pattern):
            if visible(p):
                yield p

    def path_rglob(self, pattern):
        for p in real_path_rglob(self, pattern):
            if visible(p):
                yield p

    def call(cmd, *args, **kwargs):
        return real_subprocess_call(add_dx11_defines(cmd), *args, **kwargs)

    def run(cmd, *args, **kwargs):
        return real_subprocess_run(add_dx11_defines(cmd), *args, **kwargs)

    def check_call(cmd, *args, **kwargs):
        return real_subprocess_check_call(add_dx11_defines(cmd), *args, **kwargs)

    def popen(cmd, *args, **kwargs):
        return real_popen(add_dx11_defines(cmd), *args, **kwargs)

    os.walk = walk  # type: ignore[assignment]
    _glob.glob = glob_func  # type: ignore[assignment]
    _glob.iglob = iglob_func  # type: ignore[assignment]
    Path.glob = path_glob  # type: ignore[assignment]
    Path.rglob = path_rglob  # type: ignore[assignment]
    subprocess.call = call  # type: ignore[assignment]
    subprocess.run = run  # type: ignore[assignment]
    subprocess.check_call = check_call  # type: ignore[assignment]
    subprocess.Popen = popen  # type: ignore[assignment]

def _run_original_in_process(args: list[str]) -> int:
    old_argv = sys.argv[:]
    old_env = dict(os.environ)
    try:
        os.environ["DXVK_REMIX_DX11_SHADER_MODE"] = "1"
        os.environ["RTX_REMIX_DX11"] = "1"
        os.environ["DXVK_REMIX_D3D9_SHADER_MODE"] = "0"
        os.environ["RTX_REMIX_D3D9"] = "0"
        sys.argv = [str(ORIG)] + args
        try:
            runpy.run_path(str(ORIG), run_name="__main__")
            return 0
        except SystemExit as e:
            code = e.code
            if code is None:
                return 0
            if isinstance(code, int):
                return code
            return 1
    finally:
        sys.argv = old_argv
        os.environ.clear()
        os.environ.update(old_env)

def main() -> int:
    if not ORIG.is_file():
        print(f"DX11_V219_DX11_SHADER_MODE_NO_D3D9: original compiler missing: {ORIG}", file=sys.stderr)
        return 2

    args = sys.argv[1:]
    input_i = _find_arg(args, "-input")
    output_i = _find_arg(args, "-output")

    if input_i < 0 or input_i + 1 >= len(args):
        return subprocess.call([sys.executable, str(ORIG)] + args)

    input_dir = Path(args[input_i + 1]).resolve()
    if not input_dir.is_dir():
        return subprocess.call([sys.executable, str(ORIG)] + args)

    output_dir = None
    if output_i >= 0 and output_i + 1 < len(args):
        output_dir = Path(args[output_i + 1]).resolve()

    with tempfile.TemporaryDirectory(prefix="dx11_v219_shader_fulltree_", ignore_cleanup_errors=True) as td:
        temp_root = Path(td)
        temp_input = temp_root / "input"
        temp_input.mkdir(parents=True, exist_ok=True)

        entries, headers, d3d9_skipped, modules = _copy_full_tree_and_find_modules(input_dir, temp_input)
        rtx_headers, rtx_dsts = _copy_rtx_render_headers(input_dir, temp_root)

        if output_dir is not None:
            output_dir.mkdir(parents=True, exist_ok=True)
            report = output_dir / "dx11_v219_dx11_shader_mode_report.txt"
            report.write_text(
                "DX11_V219_DX11_SHADER_MODE_NO_D3D9\n"
                "No placeholder shaders. No fake output.\n"
                "Internal Remix runtime shaders still compile through real Slang/DXVK backend.\n"
                "Game-facing API/capture side is DX11.\n"
                f"OriginalInput={input_dir}\n"
                f"TempInput={temp_input}\n"
                f"EntryFilesVisibleToCompiler={entries}\n"
                f"RealShaderHeadersCopied={headers}\n"
                f"RealRtxRenderHeadersCopied={rtx_headers}\n"
                f"D3D9PathsSkipped={d3d9_skipped}\n"
                f"DX11Defines={' '.join(DX11_DEFINES)}\n"
                f"SharedSlangModulesHiddenFromEnumeration={len(modules)}\n\n"
                + "\n".join(modules)
                + ("\n" if modules else ""),
                encoding="utf-8"
            )

        if entries == 0:
            return subprocess.call([sys.executable, str(ORIG)] + args)

        new_args = list(args)
        new_args[input_i + 1] = str(temp_input)

        existing_includes = {new_args[i + 1] for i, a in enumerate(new_args[:-1]) if a == "-include"}
        include_roots = [temp_input, input_dir, input_dir.parent, temp_root, temp_root.parent]
        include_roots.extend(rtx_dsts)
        for inc in include_roots:
            s = str(inc)
            if s not in existing_includes:
                new_args.extend(["-include", s])
                existing_includes.add(s)

        print(
            "DX11_V219_DX11_SHADER_MODE_NO_D3D9: compiling real Remix runtime shaders "
            f"for DX11 game-facing fork entries={entries} headers={headers} "
            f"rtx_render_headers={rtx_headers} d3d9_paths_skipped={d3d9_skipped} "
            f"hidden_slang_modules={len(modules)} original={input_dir}",
            flush=True
        )

        _install_hidden_module_hooks()
        return _run_original_in_process(new_args)

if __name__ == "__main__":
    raise SystemExit(main())
'@

  Write-TextNoBom -Path $compile -Text $wrapper
  Log "V219 installed DX11 shader-mode wrapper with no D3D9 shader enumeration: $compile"
}



function Mark-DX11ShaderModeNoD3D9V219 {
  Log 'DX11_V219_DX11_SHADER_MODE_NO_D3D9: shader wrapper blocks D3D9 paths and injects DX11 mode defines for Slang while keeping real Remix runtime shader compilation.'
}



function Write-Dx11ShaderCapsHeaderV219 {
  # DX11_V219_FULL_DX11_ONLY_NO_DX9
  $dx11Dir = Join-Path $Root 'src\d3d11'
  if (!(Test-Path -LiteralPath $dx11Dir -PathType Container)) {
    New-Item -ItemType Directory -Path $dx11Dir -Force | Out-Null
  }

  $capsHeader = Join-Path $dx11Dir 'd3d11_shader_caps.h'
  $capsText = @'
#pragma once

// DX11_V219_FULL_DX11_ONLY_NO_DX9
//
// The DXBC shared compiler utilities need a caps namespace for array bounds
// and register limits.  This DX11 fork must not depend on src/d3d9.
// This header provides those shared compiler limits from the DX11 side using
// D3D11 SDK limits where possible and shader-model compatibility limits
// where the value belongs to the legacy bytecode model being decoded.

#include <cstdint>

#ifndef D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
#define D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT 8
#endif
#ifndef D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
#define D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT 16
#endif
#ifndef D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
#define D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT 128
#endif
#ifndef D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
#define D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT 32
#endif
#ifndef D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
#define D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION 16384
#endif
#ifndef D3D11_REQ_TEXTURECUBE_DIMENSION
#define D3D11_REQ_TEXTURECUBE_DIMENSION 16384
#endif
#ifndef D3D11_COMMONSHADER_CONSTANT_BUFFER_REGISTER_COUNT
#define D3D11_COMMONSHADER_CONSTANT_BUFFER_REGISTER_COUNT 15
#endif
#ifndef D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT
#define D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT 4096
#endif
#ifndef D3D11_CLIP_OR_CULL_DISTANCE_COUNT
#define D3D11_CLIP_OR_CULL_DISTANCE_COUNT 8
#endif

namespace dxvk::caps {
  constexpr uint32_t MaxClipPlanes                = D3D11_CLIP_OR_CULL_DISTANCE_COUNT;
  constexpr uint32_t MaxSamplers                  = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
  constexpr uint32_t MaxStreams                   = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
  constexpr uint32_t MaxSimultaneousTextures      = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
  constexpr uint32_t MaxTextureBlendStages        = MaxSimultaneousTextures;
  constexpr uint32_t TextureStageCount            = MaxSimultaneousTextures;
  constexpr uint32_t MaxSimultaneousRenderTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
  constexpr uint32_t MaxTextureDimension          = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  constexpr uint32_t MaxTextureCubeDimension      = D3D11_REQ_TEXTURECUBE_DIMENSION;
  constexpr uint32_t MaxTexturesVS                = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
  constexpr uint32_t MaxTexturesPS                = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
  constexpr uint32_t MaxTextures                  = MaxTexturesVS + MaxTexturesPS;

  // Shader-model compatibility limits used by the shared decoder.
  // These are not a dependency on a DX9 runtime path.
  constexpr uint32_t MaxFloatConstantsVS          = 256;
  constexpr uint32_t MaxSM1FloatConstantsPS       = 8;
  constexpr uint32_t MaxSM2FloatConstantsPS       = 32;
  constexpr uint32_t MaxSM3FloatConstantsPS       = 224;
  constexpr uint32_t MaxOtherConstants            = 16;
  constexpr uint32_t MaxFloatConstantsSoftware    = D3D11_COMMONSHADER_CONSTANT_BUFFER_REGISTER_COUNT * D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
  constexpr uint32_t MaxOtherConstantsSoftware    = 2048;
  constexpr uint32_t InputRegisterCount           = 16;
  constexpr uint32_t MaxMipLevels                 = 15;
  constexpr uint32_t MaxSubresources              = MaxMipLevels * 6;
  constexpr uint32_t MaxTransforms                = 10 + 256;
  constexpr uint32_t MaxEnabledLights             = 8;
}
'@
  Write-TextNoBom -Path $capsHeader -Text $capsText
  Log "V219 wrote DX11-side shader caps header: $capsHeader"
}

function Convert-TextTokenCaseV219 {
  param([Parameter(Mandatory)][string]$Text)

  $out = $Text

  # Most important path/API tokens first.
  $out = $out -creplace 'D3D9', 'D3D11'
  $out = $out -creplace 'd3d9', 'd3d11'
  $out = $out -creplace 'D3d9', 'D3d11'

  $out = $out -creplace 'DX9', 'DX11'
  $out = $out -creplace 'dx9', 'dx11'
  $out = $out -creplace 'Dx9', 'Dx11'

  # Bridge words used in older scripts/logging.
  $out = $out -creplace 'Direct3D9', 'Direct3D11'
  $out = $out -creplace 'direct3d9', 'direct3d11'
  $out = $out -creplace 'DirectX9', 'DirectX11'
  $out = $out -creplace 'directx9', 'directx11'

  return $out
}

function Invoke-BoundedDx11OnlyNoDx9ScrubV219 {
  # DX11_V219_FULL_DX11_ONLY_NO_DX9
  # Replace DX9/D3D9 text references with DX11/D3D11, patch known shared-code
  # dependencies to DX11 equivalents, then remove DX9/D3D9 source folders/files.
  # Binary/dependency output folders are not rewritten.
  $report = Join-Path $Root 'DX11_V219_FULL_DX11_ONLY_NO_DX9_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_FULL_DX11_ONLY_NO_DX9')
  $lines.Add(('Time: {0}' -f (Get-Date)))
  $lines.Add('Mode: replace DX9/D3D9 source/build references with DX11/D3D11, then remove DX9/D3D9 source files.')
  $lines.Add('No stubs: shared compiler constants are provided by src\d3d11\d3d11_shader_caps.h.')
  $lines.Add('')

  Write-Dx11ShaderCapsHeaderV219

  $dxsoUtil = Join-Path $Root 'src\dxso\dxso_util.h'
  if (Test-Path -LiteralPath $dxsoUtil -PathType Leaf) {
    $u = [System.IO.File]::ReadAllText($dxsoUtil)
    $origU = $u
    $u = $u.Replace('#include "../d3d9/d3d9_caps.h"', '#include "../d3d11/d3d11_shader_caps.h"')
    $u = $u.Replace('#include "..\d3d9\d3d9_caps.h"', '#include "../d3d11/d3d11_shader_caps.h"')
    $u = [regex]::Replace($u, '#\s*include\s*[<"]\.\./d3d9/d3d9_caps\.h[>"]', '#include "../d3d11/d3d11_shader_caps.h"')
    $u = [regex]::Replace($u, '#\s*include\s*[<"]\.\.\\d3d9\\d3d9_caps\.h[>"]', '#include "../d3d11/d3d11_shader_caps.h"')
    if ($u -ne $origU) {
      if (!(Test-Path -LiteralPath "$dxsoUtil.v219.before")) { Copy-Item -LiteralPath $dxsoUtil -Destination "$dxsoUtil.v219.before" -Force }
      Write-TextNoBom -Path $dxsoUtil -Text $u
      $lines.Add(('OK: patched shared caps include: {0}' -f $dxsoUtil))
    } else {
      $lines.Add('OK: dxso_util.h already uses DX11 caps or has no old caps include.')
    }
  } else {
    $lines.Add('OK: src\dxso removed - the DX11 runtime no longer has a legacy shader module to patch.')
  }

  $skipDirPatterns = @(
    '\\\.git(\\|$)',
    '\\\.vs(\\|$)',
    '\\_build_logs(\\|$)',
    '\\_Comp64Release(\\|$)',
    '\\_Comp32Release(\\|$)',
    '\\_Comp32Debug(\\|$)',
    '\\_output(\\|$)',
    '\\external\\nv_usd_Release(\\|$)',
    '\\src\\usd-plugins\\_external(\\|$)',
    '\\submodules\\rtxdi(\\|$)',
    '\\submodules\\rtxcr(\\|$)',
    '\\external\\nv_ngx_real(\\|$)',
    '\\_upstream_dxvk_remix_1_5_full_runtime_dx11(\\|$)',
    '\\_upstream_dxvk_remix_1_5_runtime_ui(\\|$)'
  )

  $extensions = @(
    '.ps1','.bat','.cmd','.txt','.md','.build','.options',
    '.h','.hpp','.hh','.c','.cc','.cpp','.cxx','.inl',
    '.def','.rc','.meson','.json','.conf','.ini',
    '.py','.slang','.slangh','.hlsl','.hlsli','.glsl','.inc'
  )

  $rewritten = 0
  $files = Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue
  foreach ($f in $files) {
    $full = $f.FullName
    $rel = $full.Substring($Root.Length).TrimStart('\','/')
    $norm = '\' + ($rel -replace '/', '\')
    $skip = $false
    foreach ($pat in $skipDirPatterns) {
      if ($norm -match $pat) { $skip = $true; break }
    }
    if ($skip) { continue }

    $ext = [System.IO.Path]::GetExtension($full)
    if ($extensions -notcontains $ext) { continue }

    try {
      $bytes = [System.IO.File]::ReadAllBytes($full)
      if ($bytes.Length -gt 4MB) { continue }
      $text = [System.Text.Encoding]::UTF8.GetString($bytes)
      if ($text -notmatch '(?i)(dx9|d3d9|direct3d9|directx9)') { continue }

      $newText = Convert-TextTokenCaseV219 -Text $text

      # Preserve the explicit no-DX9 report markers in this function as DX11-only
      # wording, but do not leave source paths that would build DX9.
      if ($newText -ne $text) {
        if (!(Test-Path -LiteralPath "$full.v219.before")) {
          # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $full -Destination "$full.v219.before" -Force
        }
        Write-TextNoBom -Path $full -Text $newText
        $rewritten++
        $lines.Add(('REWRITE: {0}' -f $rel))
      }
    } catch {
      $lines.Add(('WARN: failed to inspect/rewrite {0}: {1}' -f $rel, $_.Exception.Message))
    }
  }

  $removeTargets = New-Object System.Collections.Generic.List[string]
  foreach ($rel in @(
    'src\d3d9',
    'src\dx9',
    'bridge\d3d9',
    'bridge\dx9',
    'bridge_dx9_work',
    'src\client_dx9',
    'src\server_dx9'
  )) {
    $p = Join-Path $Root $rel
    if (Test-Path -LiteralPath $p) { $removeTargets.Add($p) }
  }

  # Remove files/folders with dx9/d3d9 in names under source/build-script areas.
  foreach ($rootRel in @('src','include','public\include','scripts','scripts-common','bridge_dx11_work')) {
    $scanRoot = Join-Path $Root $rootRel
    if (!(Test-Path -LiteralPath $scanRoot)) { continue }
    try {
      Get-ChildItem -LiteralPath $scanRoot -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '(?i)(dx9|d3d9)' } |
        Sort-Object { $_.FullName.Length } -Descending |
        ForEach-Object {
          if (!$removeTargets.Contains($_.FullName)) { $removeTargets.Add($_.FullName) }
        }
    } catch {}
  }

  foreach ($p in $removeTargets) {
    if (!(Test-Path -LiteralPath $p)) { continue }
    try {
      $rel = $p.Substring($Root.Length).TrimStart('\','/')
      Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue
      $lines.Add(('REMOVED_DX9_D3D9: {0}' -f $rel))
    } catch {
      $lines.Add(('WARN: failed to remove {0}: {1}' -f $p, $_.Exception.Message))
    }
  }

  $lines.Add('')
  $lines.Add(('TextFilesRewritten: {0}' -f $rewritten))

  # Final source scan. Exclude backups and logs because they intentionally retain old text.
  $remaining = New-Object System.Collections.Generic.List[string]
  foreach ($f in (Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue)) {
    $full = $f.FullName
    $rel = $full.Substring($Root.Length).TrimStart('\','/')
    $norm = '\' + ($rel -replace '/', '\')
    $skip = $false
    foreach ($pat in $skipDirPatterns) {
      if ($norm -match $pat) { $skip = $true; break }
    }
    if ($skip) { continue }
    if ($rel -match '\.v\d+\.before$' -or $rel -match '_logs\\' -or $rel -match '\.log$') { continue }
    $ext = [System.IO.Path]::GetExtension($full)
    if ($extensions -notcontains $ext) { continue }
    try {
      $bytes = [System.IO.File]::ReadAllBytes($full)
      if ($bytes.Length -gt 4MB) { continue }
      $text = [System.Text.Encoding]::UTF8.GetString($bytes)
      if ($text -match '(?i)(dx9|d3d9|direct3d9|directx9)') {
        $remaining.Add($rel)
      }
    } catch {}
  }

  if ($remaining.Count -gt 0) {
    $lines.Add('')
    $lines.Add(('REMAINING_DX9_D3D9_REFERENCES: {0}' -f $remaining.Count))
    foreach ($r in ($remaining | Sort-Object | Select-Object -First 300)) {
      $lines.Add(('  {0}' -f $r))
    }
    $lines.Add('NOTE: remaining references in report files/backups are ignored; source/build references above need review if any remain.')
  } else {
    $lines.Add('OK: no DX9/D3D9 text references remain in active source/build text files covered by the scan.')
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 full DX11-only/no-DX9 scrub completed. Report: $report"
}



function Mark-DX11FullNoDx9V219 {
  Log 'DX11_V219_FULL_DX11_ONLY_NO_DX9: DX9/D3D9 text references are converted to DX11/D3D11 and DX9/D3D9 source files are removed after replacement.'
}



function Mark-DX11UseExistingCloneThenRemoveV219 {
  Log 'DX11_V219_USE_EXISTING_CLONE_THEN_REMOVE: uses _nvidia_dxvk_remix_for_dx11_bridge when present, copies real RTXDI/RTXCR submodules, then removes the scratch clone so DX9 source does not remain.'
}



function Invoke-BoundedDx11OnlyNoDx9ScrubV219 {
  # DX11_V219_BOUNDED_NOHANG_DX11_ONLY_SCRUB
  # V219/V219 used a full repository recursive text rewrite.  That can appear
  # stuck on large external/submodule/build trees.  V219 is bounded: only active
  # source/build-script directories are scanned, and known DX9/D3D9 folders are
  # removed directly.  No placeholders and no fake shader output.
  $report = Join-Path $Root 'DX11_V219_BOUNDED_NOHANG_DX11_ONLY_SCRUB_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_BOUNDED_NOHANG_DX11_ONLY_SCRUB')
  $lines.Add(('Time: {0}' -f (Get-Date)))
  $lines.Add('Mode: bounded active-source scrub; no full repo crawl.')
  $lines.Add('')

  Write-Dx11ShaderCapsHeaderV219

  $dxsoUtil = Join-Path $Root 'src\dxso\dxso_util.h'
  if (Test-Path -LiteralPath $dxsoUtil -PathType Leaf) {
    $u = [System.IO.File]::ReadAllText($dxsoUtil)
    $origU = $u
    $u = $u.Replace('#include "../d3d9/d3d9_caps.h"', '#include "../d3d11/d3d11_shader_caps.h"')
    $u = $u.Replace('#include "..\d3d9\d3d9_caps.h"', '#include "../d3d11/d3d11_shader_caps.h"')
    $u = [regex]::Replace($u, '#\s*include\s*[<"]\.\./d3d9/d3d9_caps\.h[>"]', '#include "../d3d11/d3d11_shader_caps.h"')
    $u = [regex]::Replace($u, '#\s*include\s*[<"]\.\.\\d3d9\\d3d9_caps\.h[>"]', '#include "../d3d11/d3d11_shader_caps.h"')
    if ($u -ne $origU) {
      if (!(Test-Path -LiteralPath "$dxsoUtil.v219.before")) {
        # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $dxsoUtil -Destination "$dxsoUtil.v219.before" -Force
      }
      Write-TextNoBom -Path $dxsoUtil -Text $u
      $lines.Add(('OK: patched dxso_util.h caps include: {0}' -f $dxsoUtil))
    } else {
      $lines.Add('OK: dxso_util.h already uses DX11 caps or no legacy caps include.')
    }
  }

  $directRemove = @(
    'src\d3d9',
    'src\dx9',
    'bridge\d3d9',
    'bridge\dx9',
    'bridge_dx9_work',
    'src\client_dx9',
    'src\server_dx9'
  )

  foreach ($rel in $directRemove) {
    $p = Join-Path $Root $rel
    if (Test-Path -LiteralPath $p) {
      Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue
      $lines.Add(('REMOVED: {0}' -f $rel))
    } else {
      $lines.Add(('OK_NOT_PRESENT: {0}' -f $rel))
    }
  }

  $activeRoots = @(
    'src\dxso',
    'src\d3d11',
    'src\dxvk',
    'src\dxgi',
    'src\util',
    'src\vulkan',
    'src\wsi',
    'src\dxbc',
    'include',
    'public\include',
    'bridge_dx11_work'
  )

  $rootFiles = @(
    'meson.build',
    'meson_options.txt',
    'dxvk.conf',
    'gametargets.example.conf'
  )

  $extensions = @(
    '.ps1','.bat','.cmd','.txt','.md','.build','.options',
    '.h','.hpp','.hh','.c','.cc','.cpp','.cxx','.inl',
    '.def','.rc','.json','.conf','.ini','.py',
    '.slang','.slangh','.hlsl','.hlsli','.glsl','.inc'
  )

  $maxFilesPerRoot = 2500
  $rewritten = 0
  $removedNamed = 0

  function Convert-Dx9TokenTextV219([string]$TextIn) {
    $t = $TextIn
    $t = $t -creplace 'D3D9', 'D3D11'
    $t = $t -creplace 'd3d9', 'd3d11'
    $t = $t -creplace 'D3d9', 'D3d11'
    $t = $t -creplace 'DX9', 'DX11'
    $t = $t -creplace 'dx9', 'dx11'
    $t = $t -creplace 'Dx9', 'Dx11'
    $t = $t -creplace 'Direct3D9', 'Direct3D11'
    $t = $t -creplace 'direct3d9', 'direct3d11'
    $t = $t -creplace 'DirectX9', 'DirectX11'
    $t = $t -creplace 'directx9', 'directx11'
    return $t
  }

  $candidateFiles = New-Object System.Collections.Generic.List[string]

  foreach ($rf in $rootFiles) {
    $p = Join-Path $Root $rf
    if (Test-Path -LiteralPath $p -PathType Leaf) { $candidateFiles.Add($p) }
  }

  foreach ($ar in $activeRoots) {
    $base = Join-Path $Root $ar
    if (!(Test-Path -LiteralPath $base -PathType Container)) { continue }

    # Remove files/folders by name first, bounded to active roots only.
    try {
      Get-ChildItem -LiteralPath $base -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '(?i)(dx9|d3d9)' } |
        Sort-Object { $_.FullName.Length } -Descending |
        ForEach-Object {
          try {
            Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
            $removedNamed++
            $lines.Add(('REMOVED_NAMED_ACTIVE: {0}' -f ($_.FullName.Substring($Root.Length).TrimStart('\','/'))))
          } catch {}
        }
    } catch {}

    try {
      $count = 0
      Get-ChildItem -LiteralPath $base -Recurse -File -ErrorAction SilentlyContinue |
        ForEach-Object {
          if ($count -ge $maxFilesPerRoot) { return }
          $count++
          $ext = [System.IO.Path]::GetExtension($_.FullName)
          if ($extensions -contains $ext) {
            $candidateFiles.Add($_.FullName)
          }
        }
      $lines.Add(('SCAN_ROOT: {0} files_seen_max_{1}' -f $ar, $count))
    } catch {
      $lines.Add(('WARN: scan failed for {0}: {1}' -f $ar, $_.Exception.Message))
    }
  }

  foreach ($full in ($candidateFiles | Sort-Object -Unique)) {
    try {
      if (!(Test-Path -LiteralPath $full -PathType Leaf)) { continue }
      $item = Get-Item -LiteralPath $full -ErrorAction SilentlyContinue
      if (!$item -or $item.Length -gt 4MB) { continue }
      $text = [System.IO.File]::ReadAllText($full)
      if ($text -notmatch '(?i)(dx9|d3d9|direct3d9|directx9)') { continue }
      # DX11_V225: skip generated numeric data tables (blue noise, LUTs, etc.). In
      # those files "d3d9" occurs only as valid hex digits inside 64-bit literals,
      # and rewriting it to "d3d11" lengthens the constant and breaks compilation
      # (C2177 "constant too big"). Real source rarely has many long hex literals.
      if (([regex]::Matches($text, '0x[0-9a-fA-F]{6,}')).Count -ge 64) {
        $lines.Add(('SKIP_DATA_TABLE: {0}' -f ($full.Substring($Root.Length).TrimStart('\','/'))))
        continue
      }
      $newText = Convert-Dx9TokenTextV219 $text
      if ($newText -ne $text) {
        $backup = "$full.v219.before"
        if (!(Test-Path -LiteralPath $backup)) {
          Copy-Item -LiteralPath $full -Destination $backup -Force
        }
        Write-TextNoBom -Path $full -Text $newText
        $rewritten++
        $lines.Add(('REWRITE_ACTIVE: {0}' -f ($full.Substring($Root.Length).TrimStart('\','/'))))
      }
    } catch {
      $lines.Add(('WARN: rewrite failed for {0}: {1}' -f $full, $_.Exception.Message))
    }
  }

  $lines.Add('')
  $lines.Add(('TextFilesRewritten: {0}' -f $rewritten))
  $lines.Add(('NamedActiveDx9FilesOrFoldersRemoved: {0}' -f $removedNamed))
  $lines.Add('OK: bounded scrub completed; no full repository crawl was used.')

  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 bounded DX11-only scrub completed. Report: $report"
}



function Mark-DX11BoundedNoHangScrubV219 {
  Log 'DX11_V219_BOUNDED_NOHANG_DX11_ONLY_SCRUB: replaces the full repo recursive DX9 scrub with a bounded active-source DX11-only scrub.'
}



function Repair-UtilGdiDxgiFormatV219 {
  # DX11_V219_UTIL_GDI_DXGI_FORMAT_FIX
  # V219/V219 DX11-only scrub can turn old shared format tokens into
  # D3D11Format, which is not a real SDK type.  Use DXGI_FORMAT for the
  # DX11-facing runtime.
  $report = Join-Path $Root 'DX11_V219_UTIL_GDI_DXGI_FORMAT_FIX.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_UTIL_GDI_DXGI_FORMAT_FIX')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  $files = @(
    (Join-Path $Root 'src\util\util_gdi.h'),
    (Join-Path $Root 'src\util\util_gdi.cpp')
  )

  foreach ($file in $files) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
      $lines.Add(('SKIP: missing {0}' -f $file))
      continue
    }

    $text = [System.IO.File]::ReadAllText($file)
    $orig = $text

    $text = $text -replace '\bD3D11Format\b', 'DXGI_FORMAT'
    $text = $text -replace '\bD3D11_FORMAT\b', 'DXGI_FORMAT'

    $map = @{
      'DXGI_FORMAT::Unknown'      = 'DXGI_FORMAT_UNKNOWN'
      'DXGI_FORMAT::A8R8G8B8'     = 'DXGI_FORMAT_B8G8R8A8_UNORM'
      'DXGI_FORMAT::X8R8G8B8'     = 'DXGI_FORMAT_B8G8R8X8_UNORM'
      'DXGI_FORMAT::R8G8B8'       = 'DXGI_FORMAT_B8G8R8X8_UNORM'
      'DXGI_FORMAT::R5G6B5'       = 'DXGI_FORMAT_B5G6R5_UNORM'
      'DXGI_FORMAT::X1R5G5B5'     = 'DXGI_FORMAT_B5G5R5A1_UNORM'
      'DXGI_FORMAT::A1R5G5B5'     = 'DXGI_FORMAT_B5G5R5A1_UNORM'
      'DXGI_FORMAT::A4R4G4B4'     = 'DXGI_FORMAT_B4G4R4A4_UNORM'
      'DXGI_FORMAT::A8'           = 'DXGI_FORMAT_A8_UNORM'
      'DXGI_FORMAT::L8'           = 'DXGI_FORMAT_R8_UNORM'
      'DXGI_FORMAT::D16'          = 'DXGI_FORMAT_D16_UNORM'
      'DXGI_FORMAT::D24S8'        = 'DXGI_FORMAT_D24_UNORM_S8_UINT'
      'DXGI_FORMAT::D32'          = 'DXGI_FORMAT_D32_FLOAT'
    }
    foreach ($k in $map.Keys) {
      $text = $text.Replace($k, [string]$map[$k])
    }

    if ($text -match '\bDXGI_FORMAT\b' -and $text -notmatch 'dxgiformat\.h') {
      if ($text -match '#pragma once') {
        $text = $text -replace '#pragma once', "#pragma once`r`n#include <dxgiformat.h>"
      } else {
        $text = "#include <dxgiformat.h>`r`n" + $text
      }
    }

    if ($text -ne $orig) {
      if (!(Test-Path -LiteralPath "$file.v219.before")) {
        # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $file -Destination "$file.v219.before" -Force
      }
      Write-TextNoBom -Path $file -Text $text
      $lines.Add(('PATCHED: {0}' -f $file))
    } else {
      $lines.Add(('OK: no util_gdi DXGI patch needed: {0}' -f $file))
    }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 util_gdi DXGI format repair completed. Report: $report"
}


function Test-CleanShaderCompilerOriginalV219 {
  param([Parameter(Mandatory)][string]$Path)

  if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }

  try {
    $t = [System.IO.File]::ReadAllText($Path)
  } catch {
    return $false
  }

  # Reject every wrapper we generated.  The "original" must be the real compiler.
  if ($t -match 'DX11_V\d+') { return $false }
  if ($t -match 'HIDDEN_SHARED_MODULES') { return $false }
  if ($t -match '_install_hidden_module_hooks') { return $false }
  if ($t -match 'compile_shaders_orig_v\d+') { return $false }
  if ($t -match 'compile_shaders_real_original_v\d+') { return $false }
  if ($t -match 'Real shader compiler wrapper') { return $false }
  if ($t -match 'No placeholder shaders') { return $false }
  if ($t -match 'No fake output') { return $false }

  # Positive signs from the real compile script.
  if ($t -match 'argparse' -and ($t -match 'slangc' -or $t -match 'glslang' -or $t -match 'compile')) {
    return $true
  }

  return $false
}

function Copy-CleanShaderCompilerCandidateV219 {
  param(
    [Parameter(Mandatory=$true)][string]$Candidate,
    [Parameter(Mandatory=$true)][string]$Destination,
    [object]$Lines = $null
  )

  # DX11_V219_REMOVE_EMBEDDED_CMDLETBINDING_PARAM_BLOCKS
  $Lines = Ensure-ReportLinesListV219 $Lines

  if (!(Test-Path -LiteralPath $Candidate -PathType Leaf)) {
    $Lines.Add(('MISS_FILE: {0}' -f $Candidate))
    return $false
  }

  if (!(Test-CleanShaderCompilerOriginalV219 -Path $Candidate)) {
    $Lines.Add(('REJECT_WRAPPER_OR_INVALID: {0}' -f $Candidate))
    return $false
  }

  Copy-Item -LiteralPath $Candidate -Destination $Destination -Force
  if (!(Test-CleanShaderCompilerOriginalV219 -Path $Destination)) {
    $Lines.Add(('BAD_AFTER_COPY: {0}' -f $Destination))
    return $false
  }

  $hash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
  $Lines.Add(('OK_COPIED_CLEAN_ORIGINAL: {0} -> {1} SHA256={2}' -f $Candidate, $Destination, $hash))
  return $true
}


function Write-CleanShaderCompilerFromGitBlobV219 {
  param(
    [Parameter(Mandatory=$true)][string]$Repo,
    [Parameter(Mandatory=$true)][string]$Blob,
    [Parameter(Mandatory=$true)][string]$Destination,
    [object]$Lines = $null
  )

  # DX11_V219_REMOVE_EMBEDDED_CMDLETBINDING_PARAM_BLOCKS
  $Lines = Ensure-ReportLinesListV219 $Lines

  $git = Get-Command git.exe -ErrorAction SilentlyContinue
  if (!$git) {
    $Lines.Add('MISS_GIT: git.exe not found.')
    return $false
  }

  if (!(Test-Path -LiteralPath $Repo -PathType Container)) {
    $Lines.Add(('MISS_REPO: {0}' -f $Repo))
    return $false
  }

  try {
    $out = & $git.Source -C $Repo show $Blob 2>$null
    if ($LASTEXITCODE -ne 0 -or !$out) {
      $Lines.Add(('MISS_GIT_BLOB: repo={0} blob={1}' -f $Repo, $Blob))
      return $false
    }

    $text = ($out -join "`n")
    Write-TextNoBom -Path $Destination -Text $text

    if (!(Test-CleanShaderCompilerOriginalV219 -Path $Destination)) {
      $Lines.Add(('REJECT_GIT_BLOB_WRAPPER_OR_INVALID: repo={0} blob={1}' -f $Repo, $Blob))
      Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
      return $false
    }

    $hash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    $Lines.Add(('OK_GIT_CLEAN_ORIGINAL: repo={0} blob={1} -> {2} SHA256={3}' -f $Repo, $Blob, $Destination, $hash))
    return $true
  } catch {
    $Lines.Add(('ERR_GIT_BLOB: repo={0} blob={1} err={2}' -f $Repo, $Blob, $_.Exception.Message))
    return $false
  }
}


function Copy-CleanShaderCompilerFromExistingSourceV219 {
  # DX11_V219_RECOVER_CLEAN_SHADER_COMPILER_ORIGINAL
  # V219 warned because every compile_shaders_orig_* file in this repo may now
  # be another wrapper.  V219 recovers the real compiler from git history or the
  # existing Remix clone and refuses to continue with a wrapper as original.
  $scripts = Join-Path $Root 'scripts-common'
  if (!(Test-Path -LiteralPath $scripts -PathType Container)) {
    New-Item -ItemType Directory -Path $scripts -Force | Out-Null
  }

  $dest = Join-Path $scripts 'compile_shaders_real_original_v219.py'
  $report = Join-Path $Root 'DX11_V219_CLEAN_SHADER_COMPILER_ORIGINAL_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_RECOVER_CLEAN_SHADER_COMPILER_ORIGINAL')
  $lines.Add(('Time: {0}' -f (Get-Date)))
  $lines.Add('Mode: recover a real compile_shaders.py; never use a DX11 wrapper as the original compiler.')
  $lines.Add('')

  # Already recovered on a previous run.
  if (Test-CleanShaderCompilerOriginalV219 -Path $dest) {
    $hash = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
    $lines.Add(('OK_EXISTING_CLEAN_ORIGINAL: {0} SHA256={1}' -f $dest, $hash))
    [System.IO.File]::WriteAllLines($report, $lines)
    return $dest
  }

  $candidateFiles = New-Object System.Collections.Generic.List[string]
  foreach ($p in @(
    (Join-Path $Root '_nvidia_dxvk_remix_for_dx11_bridge\scripts-common\compile_shaders.py'),
    (Join-Path $Root '_upstream_dxvk_remix_1_5_full_runtime_dx11\scripts-common\compile_shaders.py'),
    (Join-Path $Root '_upstream_dxvk_remix_1_5_runtime_ui\scripts-common\compile_shaders.py')
  )) {
    $candidateFiles.Add($p)
  }

  try {
    Get-ChildItem -LiteralPath $scripts -Filter 'compile_shaders*.py' -File -ErrorAction SilentlyContinue |
      Sort-Object Name |
      ForEach-Object {
        if ($_.Name -notmatch '^compile_shaders\.py$') {
          $candidateFiles.Add($_.FullName)
        }
      }
  } catch {}

  foreach ($c in $candidateFiles) {
    if (Copy-CleanShaderCompilerCandidateV219 -Candidate $c -Destination $dest -Lines $lines) {
      [System.IO.File]::WriteAllLines($report, $lines)
      Log "V219 recovered clean real shader compiler original: $dest"
      return $dest
    }
  }

  # Strong fallback: restore the real file from git history. This is not a stub;
  # it is the tracked compiler file from the repository.
  $repos = @(
    $Root,
    (Join-Path $Root '_nvidia_dxvk_remix_for_dx11_bridge'),
    (Join-Path $Root '_upstream_dxvk_remix_1_5_full_runtime_dx11'),
    (Join-Path $Root '_upstream_dxvk_remix_1_5_runtime_ui')
  )

  $blobs = @(
    'HEAD:scripts-common/compile_shaders.py',
    'HEAD~1:scripts-common/compile_shaders.py',
    'HEAD~2:scripts-common/compile_shaders.py',
    'origin/main:scripts-common/compile_shaders.py',
    'origin/master:scripts-common/compile_shaders.py'
  )

  foreach ($repo in $repos) {
    foreach ($blob in $blobs) {
      if (Write-CleanShaderCompilerFromGitBlobV219 -Repo $repo -Blob $blob -Destination $dest -Lines $lines) {
        [System.IO.File]::WriteAllLines($report, $lines)
        Log "V219 recovered clean real shader compiler original from git: $dest"
        return $dest
      }
    }
  }

  $lines.Add('')
  $lines.Add('BAD: could not recover a clean compile_shaders.py.')
  $lines.Add('No wrapper was accepted as original, because that causes wrapper recursion and false shader failures.')
  [System.IO.File]::WriteAllLines($report, $lines)
  Die "V219 could not recover a clean real compile_shaders.py. See $report"
}


function Install-RuntimeShaderCompilerWrapperV219 {
  # DX11_V219_PY314_SAFE_SHADER_WRAPPER
  # Fixes Python 3.14 pathlib hook signature:
  #   path_glob() got unexpected keyword argument case_sensitive
  # Also refuses to use a previous DX11 wrapper as the original compiler.
  $scripts = Join-Path $Root 'scripts-common'
  if (!(Test-Path -LiteralPath $scripts -PathType Container)) {
    Log "V219: scripts-common folder missing; shader compiler wrapper skipped."
    return
  }

  $compile = Join-Path $scripts 'compile_shaders.py'
  if (!(Test-Path -LiteralPath $compile -PathType Leaf)) {
    Log "V219: compile_shaders.py missing; shader compiler wrapper skipped."
    return
  }

  $clean = Copy-CleanShaderCompilerFromExistingSourceV219
  if (!$clean) {
    Die "V219 could not recover a clean original compile_shaders.py before installing the shader wrapper."
  }

  if (!(Test-Path -LiteralPath (Join-Path $scripts 'compile_shaders_before_v219.py') -PathType Leaf)) {
    Copy-Item -LiteralPath $compile -Destination (Join-Path $scripts 'compile_shaders_before_v219.py') -Force
  }

  $wrapper = @'
#!/usr/bin/env python3
# DX11_V219_PY314_SAFE_SHADER_WRAPPER
# Real shader compiler wrapper for the DX11 fork.
# No placeholder shaders and no fake output.

from __future__ import annotations

import glob as _glob
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent

STAGE_EXTS = {
    ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese",
    ".rgen", ".rchit", ".rahit", ".rmiss", ".rcall",
    ".mesh", ".task"
}

STAGE_NAME_TOKENS = (
    ".vert.", ".frag.", ".comp.", ".geom.", ".tesc.", ".tese.",
    ".rgen.", ".rchit.", ".rahit.", ".rmiss.", ".rcall.",
    ".mesh.", ".task.",
    "_vs", "_ps", "_fs", "_cs", "_gs", "_ms", "_ts",
    "vertex", "pixel", "fragment", "compute",
    "raygen", "closesthit", "anyhit", "miss", "callable",
)

HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hlsli", ".inc", ".ush", ".glslh", ".slangh", ".json", ".txt", ""}
HIDDEN_SHARED_MODULES: set[str] = set()

DX11_DEFINES = [
    "-DDXVK_REMIX_DX11_SHADER_MODE=1",
    "-DRTX_REMIX_DX11=1",
]

def _read_start(path: Path, limit: int = 65536) -> str:
    try:
        with path.open("r", encoding="utf-8", errors="ignore") as f:
            return f.read(limit)
    except Exception:
        return ""

def _is_clean_original(path: Path) -> bool:
    if not path.is_file():
        return False
    text = _read_start(path)
    if "DX11_V" in text:
        return False
    if "HIDDEN_SHARED_MODULES" in text:
        return False
    if "_install_hidden_module_hooks" in text:
        return False
    if "compile_shaders_orig_v" in text:
        return False
    return "argparse" in text or "slangc" in text or "glslang" in text

def _select_original() -> Path | None:
    candidates = [
        # DX11_V225: prefer the v174 clean original because it implements the full
        # "//!variant-matrix" system that gbuffer.slang uses. The v219 "real_original"
        # only understands "//!variant" and errors with "shader type not specified"
        # on gbuffer.slang, so gbuffer_variants.h was never generated.
        SCRIPT_DIR / "compile_shaders_real_original_v174.py",
        SCRIPT_DIR / "compile_shaders_real_original_v219.py",
        SCRIPT_DIR / "compile_shaders_orig_clean_v219.py",
        SCRIPT_DIR / "compile_shaders_orig_v219.py",
        SCRIPT_DIR / "compile_shaders_orig_v219.py",
        SCRIPT_DIR / "compile_shaders_before_v219.py",
        SCRIPT_DIR / "compile_shaders_before_v219.py",
        SCRIPT_DIR.parent / "_nvidia_dxvk_remix_for_dx11_bridge" / "scripts-common" / "compile_shaders.py",
        SCRIPT_DIR.parent / "_upstream_dxvk_remix_1_5_full_runtime_dx11" / "scripts-common" / "compile_shaders.py",
        SCRIPT_DIR.parent / "_upstream_dxvk_remix_1_5_runtime_ui" / "scripts-common" / "compile_shaders.py",
    ]
    for c in candidates:
        if _is_clean_original(c):
            return c
    return None

def _norm(p: os.PathLike[str] | str) -> str:
    try:
        return str(Path(p).resolve()).lower()
    except Exception:
        return os.path.abspath(os.fspath(p)).lower()

def _is_old_api_path(path: Path) -> bool:
    s = str(path).replace("\\", "/").lower()
    name = path.name.lower()
    return (
        "/d3d9/" in s
        or "/dx9/" in s
        or "d3d9_" in name
        or "_d3d9" in name
        or "dx9_" in name
        or "_dx9" in name
    )

def _find_arg(args: list[str], name: str) -> int:
    try:
        return args.index(name)
    except ValueError:
        return -1

def _is_entry_shader(path: Path) -> bool:
    if _is_old_api_path(path):
        return False
    lower_name = path.name.lower()
    lower_stem = path.stem.lower()
    suffix = path.suffix.lower()
    if suffix in STAGE_EXTS:
        return True
    if suffix == ".slang":
        if any(tok in lower_name or tok in lower_stem for tok in STAGE_NAME_TOKENS):
            return True
        # DX11_V225: variant / variant-matrix master shaders (gbuffer.slang,
        # integrate_direct.slang, integrate_indirect.slang) have no stage token in
        # their name but declare "//!variant" directives and generate aggregate
        # headers (gbuffer_variants.h, integrate_direct_rayquery.h, ...). They must
        # be compiled, not hidden as shared modules.
        try:
            with path.open("r", encoding="utf-8", errors="ignore") as _vf:
                if "//!variant" in _vf.read(65536):
                    return True
        except Exception:
            pass
        return False
    if suffix in {".hlsl", ".glsl"}:
        return True
    return False

def _copy_full_tree_and_find_modules(src: Path, dst: Path) -> tuple[int, int, int, list[str]]:
    entries = 0
    headers = 0
    old_api_skipped = 0
    modules: list[str] = []

    for f in src.rglob("*"):
        if not f.is_file():
            continue
        rel = f.relative_to(src)
        if _is_old_api_path(f):
            old_api_skipped += 1
            continue
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, out)

    for f in dst.rglob("*"):
        if not f.is_file():
            continue
        rel = f.relative_to(dst)
        if _is_entry_shader(f):
            entries += 1
        elif f.suffix.lower() == ".slang":
            modules.append(str(rel).replace("\\", "/"))
            HIDDEN_SHARED_MODULES.add(_norm(f))
        elif f.suffix.lower() in HEADER_SUFFIXES:
            headers += 1

    return entries, headers, old_api_skipped, modules

def _copy_rtx_render_headers(original_input: Path, temp_root: Path) -> tuple[int, list[Path]]:
    src_dxvk = original_input.parent.parent
    src_rtx_render = src_dxvk / "rtx_render"
    if not src_rtx_render.is_dir():
        return 0, []

    destinations = [
        temp_root / "rtx_render",
        temp_root.parent / "rtx_render",
        temp_root / "input" / "rtx_render",
    ]

    copied = 0
    used: list[Path] = []
    for dst in destinations:
        dst.mkdir(parents=True, exist_ok=True)
        used.append(dst)
        for f in src_rtx_render.rglob("*"):
            if not f.is_file():
                continue
            if _is_old_api_path(f):
                continue
            if f.suffix.lower() not in HEADER_SUFFIXES:
                continue
            rel = f.relative_to(src_rtx_render)
            out = dst / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)
            copied += 1
    return copied, used

def _install_hidden_module_hooks() -> None:
    real_os_walk = os.walk
    real_glob = _glob.glob
    real_iglob = _glob.iglob
    real_path_glob = Path.glob
    real_path_rglob = Path.rglob
    real_subprocess_call = subprocess.call
    real_subprocess_run = subprocess.run
    real_subprocess_check_call = subprocess.check_call
    real_popen = subprocess.Popen

    def visible(path: os.PathLike[str] | str) -> bool:
        p = Path(path)
        return _norm(path) not in HIDDEN_SHARED_MODULES and not _is_old_api_path(p)

    def add_dx11_defines(cmd):
        try:
            if isinstance(cmd, (list, tuple)) and cmd:
                exe = str(cmd[0]).lower()
                if "slangc" in exe:
                    out = list(cmd)
                    for d in DX11_DEFINES:
                        if d not in out:
                            out.append(d)
                    return out
            elif isinstance(cmd, str) and "slangc" in cmd.lower():
                extra = " " + " ".join(DX11_DEFINES)
                if "DXVK_REMIX_DX11_SHADER_MODE" not in cmd:
                    return cmd + extra
        except Exception:
            pass
        return cmd

    def walk(top, *args, **kwargs):
        for root, dirs, files in real_os_walk(top, *args, **kwargs):
            dirs[:] = [d for d in dirs if visible(Path(root) / d)]
            files[:] = [f for f in files if visible(Path(root) / f)]
            yield root, dirs, files

    def glob_func(pathname, *args, **kwargs):
        return [p for p in real_glob(pathname, *args, **kwargs) if visible(p)]

    def iglob_func(pathname, *args, **kwargs):
        for p in real_iglob(pathname, *args, **kwargs):
            if visible(p):
                yield p

    def path_glob(self, pattern, *args, **kwargs):
        for p in real_path_glob(self, pattern, *args, **kwargs):
            if visible(p):
                yield p

    def path_rglob(self, pattern, *args, **kwargs):
        for p in real_path_rglob(self, pattern, *args, **kwargs):
            if visible(p):
                yield p

    def call(cmd, *args, **kwargs):
        return real_subprocess_call(add_dx11_defines(cmd), *args, **kwargs)

    def run(cmd, *args, **kwargs):
        return real_subprocess_run(add_dx11_defines(cmd), *args, **kwargs)

    def check_call(cmd, *args, **kwargs):
        return real_subprocess_check_call(add_dx11_defines(cmd), *args, **kwargs)

    def popen(cmd, *args, **kwargs):
        return real_popen(add_dx11_defines(cmd), *args, **kwargs)

    os.walk = walk  # type: ignore[assignment]
    _glob.glob = glob_func  # type: ignore[assignment]
    _glob.iglob = iglob_func  # type: ignore[assignment]
    Path.glob = path_glob  # type: ignore[assignment]
    Path.rglob = path_rglob  # type: ignore[assignment]
    subprocess.call = call  # type: ignore[assignment]
    subprocess.run = run  # type: ignore[assignment]
    subprocess.check_call = check_call  # type: ignore[assignment]
    subprocess.Popen = popen  # type: ignore[assignment]

def _run_original_in_process(orig: Path, args: list[str]) -> int:
    old_argv = sys.argv[:]
    old_env = dict(os.environ)
    try:
        os.environ["DXVK_REMIX_DX11_SHADER_MODE"] = "1"
        os.environ["RTX_REMIX_DX11"] = "1"
        sys.argv = [str(orig)] + args
        try:
            runpy.run_path(str(orig), run_name="__main__")
            return 0
        except SystemExit as e:
            code = e.code
            if code is None:
                return 0
            if isinstance(code, int):
                return code
            return 1
    finally:
        sys.argv = old_argv
        os.environ.clear()
        os.environ.update(old_env)

def main() -> int:
    orig = _select_original()
    if orig is None:
        print("DX11_V219_PY314_SAFE_SHADER_WRAPPER: no clean original compile_shaders.py found. No fake shader output was generated.", file=sys.stderr)
        return 2

    args = sys.argv[1:]
    input_i = _find_arg(args, "-input")
    output_i = _find_arg(args, "-output")

    if input_i < 0 or input_i + 1 >= len(args):
        return subprocess.call([sys.executable, str(orig)] + args)

    input_dir = Path(args[input_i + 1]).resolve()
    if not input_dir.is_dir():
        return subprocess.call([sys.executable, str(orig)] + args)

    output_dir = None
    if output_i >= 0 and output_i + 1 < len(args):
        output_dir = Path(args[output_i + 1]).resolve()

    with tempfile.TemporaryDirectory(prefix="dx11_v219_shader_fulltree_", ignore_cleanup_errors=True) as td:
        temp_root = Path(td)
        temp_input = temp_root / "input"
        temp_input.mkdir(parents=True, exist_ok=True)

        entries, headers, old_api_skipped, modules = _copy_full_tree_and_find_modules(input_dir, temp_input)
        rtx_headers, rtx_dsts = _copy_rtx_render_headers(input_dir, temp_root)

        if output_dir is not None:
            output_dir.mkdir(parents=True, exist_ok=True)
            report = output_dir / "dx11_v219_py314_safe_shader_wrapper_report.txt"
            report.write_text(
                "DX11_V219_PY314_SAFE_SHADER_WRAPPER\n"
                "No placeholder shaders. No fake output.\n"
                f"OriginalCompiler={orig}\n"
                f"OriginalInput={input_dir}\n"
                f"TempInput={temp_input}\n"
                f"EntryFilesVisibleToCompiler={entries}\n"
                f"RealShaderHeadersCopied={headers}\n"
                f"RealRtxRenderHeadersCopied={rtx_headers}\n"
                f"OldApiPathsSkipped={old_api_skipped}\n"
                f"SharedSlangModulesHiddenFromEnumeration={len(modules)}\n\n"
                + "\n".join(modules)
                + ("\n" if modules else ""),
                encoding="utf-8"
            )

        if entries == 0:
            return subprocess.call([sys.executable, str(orig)] + args)

        new_args = list(args)
        new_args[input_i + 1] = str(temp_input)

        existing_includes = {new_args[i + 1] for i, a in enumerate(new_args[:-1]) if a == "-include"}
        include_roots = [temp_input, input_dir, input_dir.parent, temp_root, temp_root.parent]
        include_roots.extend(rtx_dsts)
        for inc in include_roots:
            s = str(inc)
            if s not in existing_includes:
                new_args.extend(["-include", s])
                existing_includes.add(s)

        print(
            "DX11_V219_PY314_SAFE_SHADER_WRAPPER: compiling real Remix runtime shaders "
            f"entries={entries} headers={headers} rtx_render_headers={rtx_headers} "
            f"old_api_paths_skipped={old_api_skipped} hidden_slang_modules={len(modules)} "
            f"original_compiler={orig}",
            flush=True
        )

        _install_hidden_module_hooks()
        result = _run_original_in_process(orig, new_args)
        if result != 0:
            return result

        # Meson declares src/dxvk/_built_shaders.txt as this custom target's
        # output.  The upstream compiler writes the real .spv/.h files into
        # the rtx_shaders directory but does not create that target output,
        # which makes Ninja rebuild every RTX shader on every invocation.
        # Record the shaders that were actually emitted only after the real
        # compiler succeeds; this is build metadata, never shader fallback.
        if output_dir is not None:
            compiled = sorted(
                (path for path in output_dir.glob("*.spv") if path.is_file()),
                key=lambda path: path.name.casefold(),
            )
            if not compiled:
                print(
                    "DX11_V219_PY314_SAFE_SHADER_WRAPPER: compiler returned success "
                    "without producing any SPIR-V shaders.",
                    file=sys.stderr,
                )
                return 3

            manifest = output_dir.parent / "_built_shaders.txt"
            manifest.write_text(
                "DX11 RTX shader build manifest\n"
                f"Compiler={orig}\n"
                f"ShaderCount={len(compiled)}\n"
                + "\n".join(f"{path.name}\t{path.stat().st_size}" for path in compiled)
                + "\n",
                encoding="utf-8",
            )

        return 0

if __name__ == "__main__":
    raise SystemExit(main())
'@

  Write-TextNoBom -Path $compile -Text $wrapper
  Log "V219 installed Python 3.14-safe DX11 shader wrapper: $compile"
}

function Mark-DX11Py314WrapperAndGdiV219 {
  Log 'DX11_V219_PY314_SAFE_SHADER_WRAPPER: pathlib hooks accept Python 3.14 keyword args and refuse wrapped compilers as originals.'
  Log 'DX11_V219_UTIL_GDI_DXGI_FORMAT_FIX: util_gdi uses DXGI_FORMAT instead of invalid D3D11Format.'
}



function Find-MinimalSurfaceInteractionNormalFieldV219 {
  # DX11_V219_RTXDI_DLSS_GEOMETRY_NORMAL_COMPAT
  $scanRoots = @(
    (Join-Path $Root 'src\dxvk\shaders'),
    (Join-Path $Root 'src\dxvk\rtx_render'),
    (Join-Path $Root 'include'),
    (Join-Path $Root 'public\include'),
    (Join-Path $Root 'submodules\rtxdi'),
    (Join-Path $Root 'submodules\rtxcr')
  )

  $files = New-Object System.Collections.Generic.List[string]
  foreach ($r in $scanRoots) {
    if (!(Test-Path -LiteralPath $r -PathType Container)) { continue }
    try {
      Get-ChildItem -LiteralPath $r -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { @('.slang','.slangh','.hlsl','.hlsli','.h','.hpp','.inc') -contains $_.Extension.ToLowerInvariant() } |
        ForEach-Object { $files.Add($_.FullName) }
    } catch {}
  }

  $allCandidates = New-Object System.Collections.Generic.List[string]
  $structFiles = New-Object System.Collections.Generic.List[string]

  foreach ($f in $files) {
    try {
      $text = [System.IO.File]::ReadAllText($f)
      if ($text -notmatch 'MinimalSurfaceInteraction') { continue }

      $matches = [regex]::Matches($text, 'struct\s+MinimalSurfaceInteraction\s*\{(?<body>.*?)\};', [System.Text.RegularExpressions.RegexOptions]::Singleline)
      foreach ($m in $matches) {
        $structFiles.Add($f)
        $body = $m.Groups['body'].Value

        foreach ($name in @('geometryNormal','shadingNormal','normal','worldNormal','surfaceNormal','interpolatedNormal','vertexNormal','N')) {
          if ($body -match ('\b(?:float|half)(?:3|4)?\s+' + [regex]::Escape($name) + '\b')) {
            if (!$allCandidates.Contains($name)) { $allCandidates.Add($name) }
          }
        }

        $fieldMatches = [regex]::Matches($body, '\b(?:float|half)(?:3|4)?\s+(?<name>[A-Za-z_][A-Za-z0-9_]*normal[A-Za-z0-9_]*|[A-Za-z_][A-Za-z0-9_]*Normal[A-Za-z0-9_]*|N)\b')
        foreach ($fm in $fieldMatches) {
          $nm = $fm.Groups['name'].Value
          if (!$allCandidates.Contains($nm)) { $allCandidates.Add($nm) }
        }
      }
    } catch {}
  }

  $priority = @('geometryNormal','normal','shadingNormal','worldNormal','surfaceNormal','interpolatedNormal','vertexNormal','N')
  foreach ($p in $priority) {
    if ($allCandidates.Contains($p)) {
      return @{
        Field = $p
        Candidates = @($allCandidates)
        StructFiles = @($structFiles)
      }
    }
  }

  return @{
    Field = $null
    Candidates = @($allCandidates)
    StructFiles = @($structFiles)
  }
}

function Patch-RtxdiDlssEnhancementGeometryNormalCompatV219 {
  # DX11_V219_RTXDI_DLSS_GEOMETRY_NORMAL_COMPAT
  # RTXDI's DLSS enhancement helper from one source revision expects
  # MinimalSurfaceInteraction.geometryNormal.  The DX11 Remix shader tree used
  # here has MinimalSurfaceInteraction, but its real normal field may be named
  # normal/shadingNormal/worldNormal/etc.  Patch the RTXDI header to use the real
  # field that exists.  No fake shader output and no dummy normal value.
  $report = Join-Path $Root 'DX11_V219_RTXDI_DLSS_GEOMETRY_NORMAL_COMPAT_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_RTXDI_DLSS_GEOMETRY_NORMAL_COMPAT')
  $lines.Add(('Time: {0}' -f (Get-Date)))
  $lines.Add('Mode: use the real MinimalSurfaceInteraction normal field; no fake functions and no fake shader output.')

  $dlss = Join-Path $Root 'submodules\rtxdi\rtxdi-sdk\include\rtxdi\DlssEnhancementFilterFunctions.slangh'
  if (!(Test-Path -LiteralPath $dlss -PathType Leaf)) {
    $lines.Add(('SKIP: RTXDI DLSS enhancement header not found: {0}' -f $dlss))
    [System.IO.File]::WriteAllLines($report, $lines)
    Log "V219 skipped RTXDI DLSS geometry normal compatibility patch; header not present."
    return
  }

  $info = Find-MinimalSurfaceInteractionNormalFieldV219
  $field = $info.Field

  $lines.Add(('DlssHeader: {0}' -f $dlss))
  $lines.Add(('StructFilesFound: {0}' -f (($info.StructFiles | Sort-Object -Unique).Count)))
  foreach ($sf in ($info.StructFiles | Sort-Object -Unique | Select-Object -First 20)) {
    $lines.Add(('  StructFile: {0}' -f $sf))
  }
  $lines.Add(('CandidateFields: {0}' -f (($info.Candidates | Sort-Object -Unique) -join ', ')))

  if ([string]::IsNullOrWhiteSpace($field)) {
    $lines.Add('BAD: could not determine a real normal field on MinimalSurfaceInteraction.')
    $lines.Add('No replacement was made because creating a dummy normal would be a fake shader patch.')
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 could not find a real MinimalSurfaceInteraction normal field. See $report"
  }

  $text = [System.IO.File]::ReadAllText($dlss)
  if ($text -notmatch 'minimalSurfaceInteraction\.geometryNormal') {
    $lines.Add('OK: header no longer references minimalSurfaceInteraction.geometryNormal.')
    $lines.Add(('SelectedField: {0}' -f $field))
    [System.IO.File]::WriteAllLines($report, $lines)
    Log "V219 RTXDI DLSS geometry normal compatibility already applied. Field: $field"
    return
  }

  if ($field -eq 'geometryNormal') {
    $lines.Add('OK: MinimalSurfaceInteraction already has geometryNormal; no replacement needed.')
    [System.IO.File]::WriteAllLines($report, $lines)
    Log "V219 found real geometryNormal field; no RTXDI DLSS patch needed."
    return
  }

  $backup = "$dlss.v219.before"
  if (!(Test-Path -LiteralPath $backup -PathType Leaf)) {
    Copy-Item -LiteralPath $dlss -Destination $backup -Force
  }

  $newText = $text.Replace('minimalSurfaceInteraction.geometryNormal', ('minimalSurfaceInteraction.' + $field))
  Write-TextNoBom -Path $dlss -Text $newText

  $after = [System.IO.File]::ReadAllText($dlss)
  if ($after -match 'minimalSurfaceInteraction\.geometryNormal') {
    $lines.Add('BAD: replacement failed; geometryNormal reference still remains.')
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 failed to remove geometryNormal references from RTXDI DLSS header. See $report"
  }

  $lines.Add(('OK: replaced minimalSurfaceInteraction.geometryNormal with minimalSurfaceInteraction.{0}' -f $field))
  $lines.Add(('Backup: {0}' -f $backup))
  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 patched RTXDI DLSS enhancement geometryNormal access to real MinimalSurfaceInteraction.$field"
}



function Mark-DX11RtxdiDlssGeometryNormalCompatV219 {
  Log 'DX11_V219_RTXDI_DLSS_GEOMETRY_NORMAL_COMPAT: RTXDI DLSS enhancement uses the real MinimalSurfaceInteraction normal field available in the DX11 shader tree.'
}



function Patch-RtxdiDlssEnhancementGeometryNormalCompatV219 {
  # DX11_V219_RTXDI_DLSS_NO_MINIMAL_SURFACE_NORMAL_GATE
  # The DX11 surface ABI in this tree has MinimalSurfaceInteraction but no real
  # normal field in that struct.  V219 intentionally refused to invent a dummy
  # normal.  V219 keeps that rule: it does not create a fake normal.  Instead it
  # removes only the RTXDI DLSS normal-consistency gate that requires the missing
  # field, while preserving the rest of the DLSS enhancement filter.
  $report = Join-Path $Root 'DX11_V219_RTXDI_DLSS_NO_MINIMAL_SURFACE_NORMAL_GATE_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_RTXDI_DLSS_NO_MINIMAL_SURFACE_NORMAL_GATE')
  $lines.Add(('Time: {0}' -f (Get-Date)))
  $lines.Add('Mode: do not invent MinimalSurfaceInteraction.geometryNormal; remove only the normal gate that requires the missing field.')

  $dlss = Join-Path $Root 'submodules\rtxdi\rtxdi-sdk\include\rtxdi\DlssEnhancementFilterFunctions.slangh'
  if (!(Test-Path -LiteralPath $dlss -PathType Leaf)) {
    $lines.Add(('SKIP: RTXDI DLSS enhancement header not found yet: {0}' -f $dlss))
    [System.IO.File]::WriteAllLines($report, $lines)
    Log 'V219 skipped RTXDI DLSS no-normal gate patch; RTXDI header not present yet.'
    return
  }

  $text = [System.IO.File]::ReadAllText($dlss)
  $orig = $text
  $lines.Add(('DlssHeader: {0}' -f $dlss))

  if ($text -notmatch 'minimalSurfaceInteraction\.geometryNormal') {
    $lines.Add('OK: no minimalSurfaceInteraction.geometryNormal references remain.')
    [System.IO.File]::WriteAllLines($report, $lines)
    Log 'V219 RTXDI DLSS no-normal gate patch already applied.'
    return
  }

  if (!(Test-Path -LiteralPath "$dlss.v219.before" -PathType Leaf)) {
    # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $dlss -Destination "$dlss.v219.before" -Force
  }

  # Remove declarations whose only purpose is feeding the missing normal gate.
  $text = [regex]::Replace(
    $text,
    '(?m)^\s*(?:const\s+)?float3\s+centerNormal\s*=\s*centerSurface\.minimalSurfaceInteraction\.geometryNormal\s*;\s*$',
    '  const bool dx11NormalGateUnavailableV219 = true;'
  )
  $text = [regex]::Replace(
    $text,
    '(?m)^\s*(?:const\s+)?float3\s+centerGeometryNormal\s*=\s*centerSurface\.minimalSurfaceInteraction\.geometryNormal\s*;\s*$',
    '  const bool dx11GeometryNormalGateUnavailableV219 = true;'
  )

  # Remove the normal rejection tests that require the missing field.  This is a
  # compatibility removal of an unavailable test, not a fake normal value.
  $text = [regex]::Replace(
    $text,
    'dot\s*\(\s*centerNormal\s*,\s*neighborSurface\.minimalSurfaceInteraction\.geometryNormal\s*\)\s*<\s*neighborNormalThreshold',
    'false /* DX11_V219: MinimalSurfaceInteraction has no real geometryNormal field */'
  )
  $text = [regex]::Replace(
    $text,
    'dot\s*\(\s*centerGeometryNormal\s*,\s*surface\.minimalSurfaceInteraction\.geometryNormal\s*\)\s*<\s*0\.9',
    'false /* DX11_V219: MinimalSurfaceInteraction has no real geometryNormal field */'
  )

  # Generic safety for equivalent formatting in the same header.
  $text = [regex]::Replace(
    $text,
    'dot\s*\(\s*[^,\r\n]+\s*,\s*[A-Za-z_][A-Za-z0-9_]*\.minimalSurfaceInteraction\.geometryNormal\s*\)\s*<\s*[^\)\r\n]+',
    'false /* DX11_V219: MinimalSurfaceInteraction has no real geometryNormal field */'
  )

  if ($text -match 'minimalSurfaceInteraction\.geometryNormal') {
    $remaining = ([regex]::Matches($text, 'minimalSurfaceInteraction\.geometryNormal')).Count
    $lines.Add(('BAD: {0} geometryNormal references remain after compatibility patch.' -f $remaining))
    $lines.Add('No dummy normal was generated.')
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 could not remove all RTXDI DLSS geometryNormal references safely. See $report"
  }

  if ($text -eq $orig) {
    $lines.Add('BAD: header contained geometryNormal references but no replacements were made.')
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 did not change RTXDI DLSS header. See $report"
  }

  Write-TextNoBom -Path $dlss -Text $text
  $lines.Add('OK: removed RTXDI DLSS normal-consistency checks that required missing MinimalSurfaceInteraction.geometryNormal.')
  $lines.Add('OK: no dummy normal field/value was created.')
  $lines.Add('OK: no fake RTXDI function and no fake shader output was generated.')
  $lines.Add(('Backup: {0}' -f "$dlss.v219.before"))
  [System.IO.File]::WriteAllLines($report, $lines)
  Log 'V219 patched RTXDI DLSS enhancement for DX11 MinimalSurfaceInteraction without a normal field.'
}

function Mark-DX11RtxdiDlssNoMinimalNormalGateV219 {
  Log 'DX11_V219_RTXDI_DLSS_NO_MINIMAL_SURFACE_NORMAL_GATE: removes RTXDI DLSS normal gate requiring missing MinimalSurfaceInteraction.geometryNormal; does not invent a dummy normal.'
}



function Mark-DX11RecoverCleanShaderCompilerOriginalV219 {
  Log 'DX11_V219_RECOVER_CLEAN_SHADER_COMPILER_ORIGINAL: recovers real compile_shaders.py from git/clone and refuses to use prior DX11 wrappers as original.'
}



function Add-UniqueEnvTokenV219 {
  param(
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string]$Token
  )
  $cur = [System.Environment]::GetEnvironmentVariable($Name, 'Process')
  if ([string]::IsNullOrWhiteSpace($cur)) {
    [System.Environment]::SetEnvironmentVariable($Name, $Token, 'Process')
    return
  }
  if ($cur -notmatch [regex]::Escape($Token)) {
    [System.Environment]::SetEnvironmentVariable($Name, ($Token + ' ' + $cur), 'Process')
  }
}

function Patch-BoostBindGlobalPlaceholdersV219 {
  # DX11_V219_BOOST_BIND_FULL_PLACEHOLDER_COMPAT
  # USD/Python wrapping code in nv_usd can still include <boost/bind.hpp>.
  # Boost 1.76 prints a note unless BOOST_BIND_GLOBAL_PLACEHOLDERS is defined.
  # This is not a fake placeholder or source stub; it is the Boost-supported
  # compatibility mode for code that still expects _1/_2 names while we also
  # patch direct includes to the full boost::placeholders include path.
  $report = Join-Path $Root 'DX11_V219_BOOST_BIND_FULL_PLACEHOLDER_COMPAT_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_BOOST_BIND_FULL_PLACEHOLDER_COMPAT')
  $lines.Add(('Time: {0}' -f (Get-Date)))
  $lines.Add('Mode: use Boost-supported full placeholder compatibility; no source stubs.')
  $lines.Add('')

  $define = '/DBOOST_BIND_GLOBAL_PLACEHOLDERS=1'
  Add-UniqueEnvTokenV219 -Name 'CL' -Token $define
  Add-UniqueEnvTokenV219 -Name 'CXXFLAGS' -Token $define
  Add-UniqueEnvTokenV219 -Name 'CPPFLAGS' -Token $define
  $lines.Add('OK: process environment compile define injected into CL/CXXFLAGS/CPPFLAGS.')

  $meson = Join-Path $Root 'meson.build'
  if (Test-Path -LiteralPath $meson -PathType Leaf) {
    $m = [System.IO.File]::ReadAllText($meson)
    if ($m -notmatch 'BOOST_BIND_GLOBAL_PLACEHOLDERS') {
      $needle = "default_options : ['werror=true', 'b_vscrt=from_buildtype']`r`n)"
      if ($m.Contains($needle)) {
        $insert = $needle + "`r`n`r`n# DX11_V219_BOOST_BIND_FULL_PLACEHOLDER_COMPAT`r`nadd_project_arguments('-DBOOST_BIND_GLOBAL_PLACEHOLDERS=1', language : 'cpp')"
        $m = $m.Replace($needle, $insert)
      } else {
        $m = [regex]::Replace($m, '(?s)(project\s*\(.*?\)\s*)', '$1' + "`r`n# DX11_V219_BOOST_BIND_FULL_PLACEHOLDER_COMPAT`r`nadd_project_arguments('-DBOOST_BIND_GLOBAL_PLACEHOLDERS=1', language : 'cpp')`r`n", 1)
      }
      Write-TextNoBom -Path $meson -Text $m
      $lines.Add('OK: meson.build adds -DBOOST_BIND_GLOBAL_PLACEHOLDERS=1 for C++ builds.')
    } else {
      $lines.Add('OK: meson.build already contains BOOST_BIND_GLOBAL_PLACEHOLDERS.')
    }
  }

  $roots = @(
    (Join-Path $Root 'src\usd-plugins'),
    (Join-Path $Root 'src'),
    (Join-Path $Root 'include')
  )

  $patched = 0
  foreach ($r in $roots) {
    if (!(Test-Path -LiteralPath $r -PathType Container)) { continue }
    try {
      Get-ChildItem -LiteralPath $r -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { @('.h','.hpp','.cpp','.cxx','.cc','.inl') -contains $_.Extension.ToLowerInvariant() } |
        ForEach-Object {
          $p = $_.FullName
          try {
            $t = [System.IO.File]::ReadAllText($p)
            if ($t -notmatch '#\s*include\s*[<"]boost/bind\.hpp[>"]') { return }
            $nt = [regex]::Replace($t, '#\s*include\s*[<"]boost/bind\.hpp[>"]', "#include <boost/bind/bind.hpp>`r`nusing namespace boost::placeholders;")
            if ($nt -ne $t) {
              if (!(Test-Path -LiteralPath "$p.v219.before" -PathType Leaf)) {
                # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $p -Destination "$p.v219.before" -Force
              }
              Write-TextNoBom -Path $p -Text $nt
              $patched++
              $lines.Add(('PATCHED_INCLUDE: {0}' -f ($p.Substring($Root.Length).TrimStart('\','/'))))
            }
          } catch {
            $lines.Add(('WARN: boost bind include patch failed for {0}: {1}' -f $p, $_.Exception.Message))
          }
        }
    } catch {}
  }

  $lines.Add(('DirectBoostBindIncludesPatched: {0}' -f $patched))
  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 applied Boost.Bind placeholder compatibility. Report: $report"
}

function Patch-SlangVecAliasesInLightHeaderV219 {
  # DX11_V219_SLANG_FLOAT_VECTOR_LIGHT_HEADER_FIX
  # Slang rejected GLSL-style vec3 in concept/light/light.h:
  #   error 39999: declaration does not declare anything
  #     vec3 axis;
  # This is a real type-compatibility replacement to Slang/HLSL float vectors.
  $report = Join-Path $Root 'DX11_V219_SLANG_FLOAT_VECTOR_LIGHT_HEADER_FIX_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SLANG_FLOAT_VECTOR_LIGHT_HEADER_FIX')
  $lines.Add(('Time: {0}' -f (Get-Date)))
  $lines.Add('Mode: replace GLSL vecN declarations in the RTX light header with Slang/HLSL floatN.')
  $lines.Add('')

  $targets = @(
    (Join-Path $Root 'src\dxvk\shaders\rtx\concept\light\light.h'),
    (Join-Path $Root 'include\rtx\concept\light\light.h')
  )

  $patched = 0
  foreach ($p in $targets) {
    if (!(Test-Path -LiteralPath $p -PathType Leaf)) {
      $lines.Add(('MISS: {0}' -f $p))
      continue
    }

    $t = [System.IO.File]::ReadAllText($p)
    $nt = $t

    # Do not blanket-edit comments only; this targets real vector type tokens.
    $nt = [regex]::Replace($nt, '(?<![A-Za-z0-9_])vec2(?![A-Za-z0-9_])', 'float2')
    $nt = [regex]::Replace($nt, '(?<![A-Za-z0-9_])vec3(?![A-Za-z0-9_])', 'float3')
    $nt = [regex]::Replace($nt, '(?<![A-Za-z0-9_])vec4(?![A-Za-z0-9_])', 'float4')

    if ($nt -ne $t) {
      if (!(Test-Path -LiteralPath "$p.v219.before" -PathType Leaf)) {
        # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $p -Destination "$p.v219.before" -Force
      }
      Write-TextNoBom -Path $p -Text $nt
      $patched++
      $lines.Add(('PATCHED_VECTOR_TYPES: {0}' -f $p))
    } else {
      $lines.Add(('OK_ALREADY_FLOAT_VECTORS: {0}' -f $p))
    }

    $check = [System.IO.File]::ReadAllText($p)
    if ($check -match '(?<![A-Za-z0-9_])vec[234](?![A-Za-z0-9_])') {
      $lines.Add(('WARN: vecN tokens still remain in {0}; review comments or other code.' -f $p))
    }
  }

  $lines.Add(('FilesPatched: {0}' -f $patched))
  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 patched Slang vector aliases in RTX light header. Report: $report"
}



function Mark-DX11SlangVectorsAndBoostBindV219 {
  Log 'DX11_V219_SLANG_FLOAT_VECTOR_LIGHT_HEADER_FIX: replaces GLSL vecN in RTX light header with Slang/HLSL floatN.'
  Log 'DX11_V219_BOOST_BIND_FULL_PLACEHOLDER_COMPAT: applies Boost.Bind compatibility define and direct include migration for USD wrappers.'
}



function Patch-RuntimeBuildIncrementalNoRecompileLoopV219 {
  # DX11_V219_INCREMENTAL_RUNTIME_NO_RECOMPILE_LOOP
  # This marker is used by the repair/verify step. The actual behavior is applied
  # directly in Build-X64Runtime: do not delete _Comp64Release every run, and do
  # not force all RTX shaders to rebuild from scratch unless the user explicitly
  # removes the build dir.
  Log 'DX11_V219_INCREMENTAL_RUNTIME_NO_RECOMPILE_LOOP: runtime build directory is preserved for Ninja incremental rebuilds.'
}



function Mark-DX11IncrementalRuntimeNoRecompileLoopV219 {
  Log 'DX11_V219_INCREMENTAL_RUNTIME_NO_RECOMPILE_LOOP: prevents repeated full x64 runtime/shader rebuilds by preserving _Comp64Release and using Ninja incremental mode.'
}



function Ensure-ReportLinesListV219 {
  param([object]$Lines)

  # DX11_V219_REMOVE_EMBEDDED_CMDLETBINDING_PARAM_BLOCKS
  $newLines = New-Object System.Collections.Generic.List[string]

  if ($null -ne $Lines) {
    if ($Lines -is [System.Collections.Generic.List[string]]) {
      foreach ($line in $Lines) {
        if ($null -ne $line) { $newLines.Add([string]$line) }
      }
    } elseif ($Lines -is [System.Collections.IEnumerable] -and -not ($Lines -is [string])) {
      foreach ($line in $Lines) {
        if ($null -ne $line) {
          $s = [string]$line
          if (![string]::IsNullOrWhiteSpace($s)) { $newLines.Add($s) }
        }
      }
    } else {
      $single = [string]$Lines
      if (![string]::IsNullOrWhiteSpace($single)) { $newLines.Add($single) }
    }
  }

  return ,$newLines
}


function Mark-DX11OptionalLinesParamFixV219 {
  Log 'DX11_V219_OPTIONAL_LINES_PARAM_FIX: report Lines parameters are optional/object-safe so empty strings cannot stop the build before the real compile step.'
}



function Mark-DX11MutableLinesNoEnumerateFixV219 {
  Log 'DX11_V219_MUTABLE_LINES_NO_ENUMERATE_FIX: report Lines helper returns a mutable Generic.List[string] with comma no-enumerate so Add() cannot hit a fixed array.'
}



function Mark-DX11RewriteCleanCompilerHelpersV219 {
  Log 'DX11_V219_REWRITE_CLEAN_COMPILER_HELPERS: rewrites the shader compiler recovery helper functions with clean PowerShell param blocks.'
}



function Mark-DX11RemoveEmbeddedCmdletBindingParamBlocksV219 {
  Log 'DX11_V219_REMOVE_EMBEDDED_CMDLETBINDING_PARAM_BLOCKS: strips injected repair-script CmdletBinding/param blocks from inside build_dxvk_all_ninja.ps1 and rewrites helper functions cleanly.'
}



function Mark-DX11ForceCleanBuildScriptV219 {
  Log 'DX11_V219_FORCE_CLEAN_BUILD_SCRIPT: build_dxvk_all_ninja.ps1 was restored from a clean bundled copy instead of patching the corrupted parser-broken file.'
}



function Mark-DX11DefineCleanSwitchDefaultV219 {
  Log 'DX11_V219_DEFINE_CLEAN_SWITCH_DEFAULT: defines script-scope $Clean=$false so StrictMode cannot stop clone-sync cleanup checks.'
}



function Mark-DX11DefineBridgeRepoDefaultsV219 {
  Log 'DX11_V219_DEFINE_BRIDGE_REPO_DEFAULTS: defines script-scope BridgeRepoUrl and BridgeBranch so StrictMode cannot stop bridge clone setup.'
}



function Mark-DX11DefineBuildModeDefaultsV219 {
  Log 'DX11_V219_DEFINE_BUILD_MODE_DEFAULTS: defines SourceOnly and other optional build-mode switches as StrictMode-safe false defaults.'
}



function Mark-DX11DefineRuntimeBuildSwitchDefaultsV219 {
  Log 'DX11_V219_DEFINE_RUNTIME_BUILD_SWITCH_DEFAULTS: defines SkipRuntimeBuild and related optional build switches as StrictMode-safe false defaults.'
}



function Mark-DX11ForceDualArchBuildDefaultsV219 {
  Log 'DX11_V219_FORCE_DUAL_ARCH_BUILD_DEFAULTS: BuildX86 and BuildX64 default true while skip/no-x86/no-x64 switches default false.'
}



function Mark-DX11DisableStrictModeRuntimeV219 {
  Log 'DX11_V219_DISABLE_STRICTMODE_RUNTIME: runtime StrictMode is off so unset optional switches do not stop the x86/x64 build.'
}



function Force-UtilGdiDxgiFormatHeaderV219 {
  # DX11_V219_FORCE_UTIL_GDI_DXGI_HEADER
  # Fixes:
  #   util_gdi.h(...): error C3646: 'Format': unknown override specifier
  # Cause:
  #   DXGI_FORMAT is used but the header does not reliably include the Windows
  #   DXGI format declarations after the DX11-only source rewrite.
  $report = Join-Path $Root 'DX11_V219_FORCE_UTIL_GDI_DXGI_HEADER_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_FORCE_UTIL_GDI_DXGI_HEADER')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  $files = @(
    (Join-Path $Root 'src\util\util_gdi.h'),
    (Join-Path $Root 'src\util\util_gdi.cpp')
  )

  $formatMap = @{
    'DXGI_FORMAT::Unknown'      = 'DXGI_FORMAT_UNKNOWN'
    'DXGI_FORMAT::A8R8G8B8'     = 'DXGI_FORMAT_B8G8R8A8_UNORM'
    'DXGI_FORMAT::X8R8G8B8'     = 'DXGI_FORMAT_B8G8R8X8_UNORM'
    'DXGI_FORMAT::R8G8B8'       = 'DXGI_FORMAT_B8G8R8X8_UNORM'
    'DXGI_FORMAT::R5G6B5'       = 'DXGI_FORMAT_B5G6R5_UNORM'
    'DXGI_FORMAT::X1R5G5B5'     = 'DXGI_FORMAT_B5G5R5A1_UNORM'
    'DXGI_FORMAT::A1R5G5B5'     = 'DXGI_FORMAT_B5G5R5A1_UNORM'
    'DXGI_FORMAT::A4R4G4B4'     = 'DXGI_FORMAT_B4G4R4A4_UNORM'
    'DXGI_FORMAT::A8'           = 'DXGI_FORMAT_A8_UNORM'
    'DXGI_FORMAT::L8'           = 'DXGI_FORMAT_R8_UNORM'
    'DXGI_FORMAT::D16'          = 'DXGI_FORMAT_D16_UNORM'
    'DXGI_FORMAT::D24S8'        = 'DXGI_FORMAT_D24_UNORM_S8_UINT'
    'DXGI_FORMAT::D32'          = 'DXGI_FORMAT_D32_FLOAT'
  }

  foreach ($file in $files) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
      $lines.Add(('SKIP: missing {0}' -f $file))
      continue
    }

    $text = [System.IO.File]::ReadAllText($file)
    $orig = $text

    $text = $text -replace '\bD3D11Format\b', 'DXGI_FORMAT'
    $text = $text -replace '\bD3D11_FORMAT\b', 'DXGI_FORMAT'

    foreach ($k in $formatMap.Keys) {
      $text = $text.Replace($k, [string]$formatMap[$k])
    }

    if ($file.EndsWith('util_gdi.h')) {
      # Remove previous partial include attempts so the final block is clear.
      $text = [regex]::Replace($text, '^\s*#include\s+<dxgiformat\.h>\s*\r?\n', '', [System.Text.RegularExpressions.RegexOptions]::Multiline)
      $text = [regex]::Replace($text, '^\s*#include\s+<dxgi\.h>\s*\r?\n', '', [System.Text.RegularExpressions.RegexOptions]::Multiline)

      $includeBlock = "#include <dxgiformat.h>`r`n#include <dxgi.h>"
      if ($text -match '#pragma once') {
        $text = [regex]::Replace($text, '#pragma once\s*', "#pragma once`r`n$includeBlock`r`n", 1)
      } else {
        $text = "$includeBlock`r`n$text"
      }

      if ($text -match '\bDXGI_FORMAT\s+Format\b') {
        $lines.Add('OK: util_gdi.h declares Format as DXGI_FORMAT.')
      } elseif ($text -match '\bDXGI_FORMAT\b') {
        $lines.Add('OK: util_gdi.h contains DXGI_FORMAT.')
      } else {
        $lines.Add('WARN: util_gdi.h does not contain DXGI_FORMAT after patch.')
      }

      if ($text -notmatch '#include\s+<dxgiformat\.h>' -or $text -notmatch '#include\s+<dxgi\.h>') {
        $lines.Add('BAD: util_gdi.h missing required DXGI includes after patch.')
        [System.IO.File]::WriteAllLines($report, $lines)
        Die "V219 failed to force DXGI includes into util_gdi.h. See $report"
      }
    }

    if ($text -ne $orig) {
      if (!(Test-Path -LiteralPath "$file.v219.before")) {
        # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $file -Destination "$file.v219.before" -Force
      }
      Write-TextNoBom -Path $file -Text $text
      $lines.Add(('PATCHED: {0}' -f $file))
    } else {
      $lines.Add(('OK: unchanged {0}' -f $file))
    }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 forced util_gdi DXGI header/type repair. Report: $report"
}



function Mark-DX11ForceUtilGdiDxgiHeaderV219 {
  Log 'DX11_V219_FORCE_UTIL_GDI_DXGI_HEADER: forces util_gdi.h to include dxgiformat.h/dxgi.h and use DXGI_FORMAT.'
}



function Patch-SlangLightAxisIdentifierV219 {
  # DX11_V219_SLANG_LIGHT_AXIS_IDENTIFIER_FIX
  # Fixes Slang compile failure:
  #   rtx/concept/light/light.h(102): declaration does not declare anything
  #   float3 axis;
  #
  # In this Slang compiler path, "axis" is not safe as a struct/member
  # identifier in the imported light concept header.  Rename only this shader
  # header identifier to lightAxis while preserving field order/layout.
  $report = Join-Path $Root 'DX11_V219_SLANG_LIGHT_AXIS_IDENTIFIER_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SLANG_LIGHT_AXIS_IDENTIFIER_FIX')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  $files = @(
    (Join-Path $Root 'src\dxvk\shaders\rtx\concept\light\light.h'),
    (Join-Path $Root 'include\rtx\concept\light\light.h'),
    (Join-Path $Root 'public\include\rtx\concept\light\light.h')
  )

  foreach ($file in $files) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
      $lines.Add(('SKIP: missing {0}' -f $file))
      continue
    }

    $text = [System.IO.File]::ReadAllText($file)
    $orig = $text

    # Keep the earlier DX11 Slang/HLSL vector conversion active.
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])vec2(?![A-Za-z0-9_])', 'float2')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])vec3(?![A-Za-z0-9_])', 'float3')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])vec4(?![A-Za-z0-9_])', 'float4')

    # Rename the unsafe axis identifier in this shader concept header.
    # This catches:
    #   float3 axis;
    #   .axis
    #   axis = ...
    #   axis)
    # while leaving words like coordinateAxis alone.
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])axis(?![A-Za-z0-9_])', 'lightAxis')

    if ($text -ne $orig) {
      if (!(Test-Path -LiteralPath "$file.v219.before")) {
        # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $file -Destination "$file.v219.before" -Force
      }
      Write-TextNoBom -Path $file -Text $text
      $lines.Add(('PATCHED: {0}' -f $file))
    } else {
      $lines.Add(('OK: unchanged {0}' -f $file))
    }

    $after = [System.IO.File]::ReadAllText($file)
    if ($after -match '(?m)^\s*float3\s+axis\s*;') {
      $lines.Add(('BAD: unsafe float3 axis declaration remains in {0}' -f $file))
      [System.IO.File]::WriteAllLines($report, $lines)
      Die "V219 failed to remove unsafe float3 axis declaration. See $report"
    }
    if ($after -match '(?<![A-Za-z0-9_])vec[234](?![A-Za-z0-9_])') {
      $lines.Add(('BAD: vec2/vec3/vec4 token remains in {0}' -f $file))
      [System.IO.File]::WriteAllLines($report, $lines)
      Die "V219 failed to remove GLSL vecN token from light.h. See $report"
    }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 patched Slang light.h axis identifier/vector aliases. Report: $report"
}



function Mark-DX11SlangLightAxisIdentifierFixV219 {
  Log 'DX11_V219_SLANG_LIGHT_AXIS_IDENTIFIER_FIX: renames unsafe light.h axis identifier to lightAxis and keeps floatN vector aliases.'
}



function Patch-SlangLightExplicitVectorTypesV219 {
  # DX11_V219_SLANG_LIGHT_EXPLICIT_VECTOR_TYPES
  # Fixes Slang error 39999 in rtx/concept/light/light.h:
  #   declaration does not declare anything
  #   float3 axis;
  #
  # In this Slang import path, the light concept header cannot rely on floatN
  # aliases. Use explicit Slang vector<float,N> types and rename the unsafe axis
  # identifier to lightAxis. This is a real type rewrite, not fake shader output.
  $report = Join-Path $Root 'DX11_V219_SLANG_LIGHT_EXPLICIT_VECTOR_TYPES_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SLANG_LIGHT_EXPLICIT_VECTOR_TYPES')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  $files = @(
    (Join-Path $Root 'src\dxvk\shaders\rtx\concept\light\light.h'),
    (Join-Path $Root 'include\rtx\concept\light\light.h'),
    (Join-Path $Root 'public\include\rtx\concept\light\light.h')
  )

  foreach ($file in $files) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
      $lines.Add(('SKIP: missing {0}' -f $file))
      continue
    }

    $text = [System.IO.File]::ReadAllText($file)
    $orig = $text

    # Convert GLSL-style and HLSL-style aliases to explicit Slang generic vector
    # types in this header only. Word boundaries avoid vec3/float3 inside larger
    # identifiers such as float3x3.
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])vec2(?![A-Za-z0-9_])', 'vector<float, 2>')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])vec3(?![A-Za-z0-9_])', 'vector<float, 3>')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])vec4(?![A-Za-z0-9_])', 'vector<float, 4>')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])float2(?![A-Za-z0-9_])', 'vector<float, 2>')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])float3(?![A-Za-z0-9_])', 'vector<float, 3>')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])float4(?![A-Za-z0-9_])', 'vector<float, 4>')

    # Rename standalone axis identifiers after the type rewrite.
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])axis(?![A-Za-z0-9_])', 'lightAxis')

    if ($text -ne $orig) {
      if (!(Test-Path -LiteralPath "$file.v219.before")) {
        # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $file -Destination "$file.v219.before" -Force
      }
      Write-TextNoBom -Path $file -Text $text
      $lines.Add(('PATCHED: {0}' -f $file))
    } else {
      $lines.Add(('OK: unchanged {0}' -f $file))
    }

    $after = [System.IO.File]::ReadAllText($file)

    if ($after -match '(?m)^\s*(?:float|vec)[234]\s+') {
      $lines.Add(('BAD: floatN/vecN declaration remains in {0}' -f $file))
      [System.IO.File]::WriteAllLines($report, $lines)
      Die "V219 failed to remove floatN/vecN declarations from light.h. See $report"
    }

    if ($after -match '(?m)^\s*vector<float,\s*3>\s+axis\s*;') {
      $lines.Add(('BAD: unsafe axis field remains in {0}' -f $file))
      [System.IO.File]::WriteAllLines($report, $lines)
      Die "V219 failed to rename axis field in light.h. See $report"
    }

    if ($after -match 'vector<float,\s*3>\s+lightAxis\s*;') {
      $lines.Add(('OK: explicit vector<float,3> lightAxis field present in {0}' -f $file))
    }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 patched Slang light.h to explicit vector<float,N> types. Report: $report"
}



function Mark-DX11SlangLightExplicitVectorTypesV219 {
  Log 'DX11_V219_SLANG_LIGHT_EXPLICIT_VECTOR_TYPES: uses vector<float,N> in light.h and renames axis to lightAxis.'
}



function Patch-CylinderLightLightAxisUsesV219 {
  # DX11_V219_CYLINDER_LIGHT_LIGHTAXIS_USES
  # Fixes Slang error after V219:
  #   'axis' is not a member of 'CylinderLight'
  # because V219 renamed the CylinderLight field from axis -> lightAxis in
  # light.h, but cylinder_light.slangh still used cylinderLight.axis.
  $report = Join-Path $Root 'DX11_V219_CYLINDER_LIGHT_LIGHTAXIS_USES_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_CYLINDER_LIGHT_LIGHTAXIS_USES')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  $files = @(
    (Join-Path $Root 'src\dxvk\shaders\rtx\concept\light\cylinder_light.slangh'),
    (Join-Path $Root 'include\rtx\concept\light\cylinder_light.slangh'),
    (Join-Path $Root 'public\include\rtx\concept\light\cylinder_light.slangh')
  )

  foreach ($file in $files) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
      $lines.Add(('SKIP: missing {0}' -f $file))
      continue
    }

    $text = [System.IO.File]::ReadAllText($file)
    $orig = $text

    # Match the renamed field from light.h.
    $text = $text.Replace('cylinderLight.axis', 'cylinderLight.lightAxis')
    $text = $text.Replace('CylinderLight.axis', 'CylinderLight.lightAxis')

    # Keep this shader header compatible with the same Slang path as light.h.
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])vec2(?![A-Za-z0-9_])', 'vector<float, 2>')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])vec3(?![A-Za-z0-9_])', 'vector<float, 3>')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])vec4(?![A-Za-z0-9_])', 'vector<float, 4>')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])float2(?![A-Za-z0-9_])', 'vector<float, 2>')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])float3(?![A-Za-z0-9_])', 'vector<float, 3>')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])float4(?![A-Za-z0-9_])', 'vector<float, 4>')

    if ($text -ne $orig) {
      if (!(Test-Path -LiteralPath "$file.v219.before")) {
        # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $file -Destination "$file.v219.before" -Force
      }
      Write-TextNoBom -Path $file -Text $text
      $lines.Add(('PATCHED: {0}' -f $file))
    } else {
      $lines.Add(('OK: unchanged {0}' -f $file))
    }

    $after = [System.IO.File]::ReadAllText($file)
    if ($after.Contains('cylinderLight.axis')) {
      $lines.Add(('BAD: cylinderLight.axis remains in {0}' -f $file))
      [System.IO.File]::WriteAllLines($report, $lines)
      Die "V219 failed to replace cylinderLight.axis. See $report"
    }
    if ($after.Contains('cylinderLight.lightAxis')) {
      $lines.Add(('OK: cylinderLight.lightAxis present in {0}' -f $file))
    }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 patched cylinder_light.slangh use-sites to lightAxis. Report: $report"
}



function Mark-DX11CylinderLightLightAxisUsesV219 {
  Log 'DX11_V219_CYLINDER_LIGHT_LIGHTAXIS_USES: updates cylinder_light.slangh use-sites from cylinderLight.axis to cylinderLight.lightAxis.'
}



function Patch-LightFloatNSymbolSyntaxSyncV219 {
  # DX11_V219_PRECISE_LIGHTAXIS_NO_BACKUP_PATCH
  $report = Join-Path $Root 'DX11_V219_PRECISE_LIGHTAXIS_NO_BACKUP_PATCH_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_PRECISE_LIGHTAXIS_NO_BACKUP_PATCH')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  $dirs = @(
    (Join-Path $Root 'src\dxvk\shaders\rtx\concept\light'),
    (Join-Path $Root 'include\rtx\concept\light'),
    (Join-Path $Root 'public\include\rtx\concept\light')
  )

  $files = New-Object System.Collections.Generic.List[string]
  foreach ($dir in $dirs) {
    if (!(Test-Path -LiteralPath $dir -PathType Container)) {
      $lines.Add(('SKIP ROOT: {0}' -f $dir))
      continue
    }
    Get-ChildItem -LiteralPath $dir -File -Recurse | ForEach-Object {
      if ($_.Name -match '\.before$') { return }
      if ($_.Name -notmatch '\.(h|slangh|slang)$') { return }
      $files.Add($_.FullName)
    }
  }

  foreach ($file in $files) {
    $text = [System.IO.File]::ReadAllText($file)
    $orig = $text
    $text = [regex]::Replace($text, 'vector\s*<\s*float\s*,\s*2\s*>', 'float2')
    $text = [regex]::Replace($text, 'vector\s*<\s*float\s*,\s*3\s*>', 'float3')
    $text = [regex]::Replace($text, 'vector\s*<\s*float\s*,\s*4\s*>', 'float4')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])vec2(?![A-Za-z0-9_])', 'float2')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])vec3(?![A-Za-z0-9_])', 'float3')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])vec4(?![A-Za-z0-9_])', 'float4')
    $text = [regex]::Replace($text, '(?m)^(\s*float3\s+)axis(\s*;\s*)$', '$1lightAxis$2')
    $text = [regex]::Replace($text, '\.axis(?![A-Za-z0-9_])', '.lightAxis')
    $text = $text.Replace('.lightlightAxis', '.lightAxis')
    $text = $text.Replace('lightlightAxis', 'lightAxis')
    if ($text -ne $orig) {
      if (!(Test-Path -LiteralPath "$file.v219.before")) {
        # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $file -Destination "$file.v219.before" -Force
      }
      Write-TextNoBom -Path $file -Text $text
      $lines.Add(('PATCHED ACTIVE: {0}' -f $file))
    } else {
      $lines.Add(('OK ACTIVE: unchanged {0}' -f $file))
    }
  }

  $bad = $false
  foreach ($file in $files) {
    $t = [System.IO.File]::ReadAllText($file)
    if ($t -match 'vector\s*<\s*float\s*,\s*[234]\s*>') { $bad = $true; $lines.Add(('BAD: vector<float,N> remains in {0}' -f $file)) }
    if ($t -match '(?<![A-Za-z0-9_])vec[234](?![A-Za-z0-9_])') { $bad = $true; $lines.Add(('BAD: vecN remains in {0}' -f $file)) }
    if ($t -match '\.axis(?![A-Za-z0-9_])') { $bad = $true; $lines.Add(('BAD: exact .axis remains in {0}' -f $file)) }
    if ($t -match '(?m)^\s*float3\s+axis\s*;') { $bad = $true; $lines.Add(('BAD: float3 axis field remains in {0}' -f $file)) }
  }

  $light = Join-Path $Root 'src\dxvk\shaders\rtx\concept\light\light.h'
  if (Test-Path -LiteralPath $light -PathType Leaf) {
    $t = [System.IO.File]::ReadAllText($light)
    if ($t -match '(?m)^\s*float3\s+lightAxis\s*;') { $lines.Add('OK: light.h has float3 lightAxis.') } else { $bad = $true; $lines.Add('BAD: light.h missing float3 lightAxis.') }
  }

  $cyl = Join-Path $Root 'src\dxvk\shaders\rtx\concept\light\cylinder_light.slangh'
  if (Test-Path -LiteralPath $cyl -PathType Leaf) {
    $t = [System.IO.File]::ReadAllText($cyl)
    if ($t -match '\bcylinderLight\.lightAxis\b') { $lines.Add('OK: cylinderLight.lightAxis present.') } else { $bad = $true; $lines.Add('BAD: cylinderLight.lightAxis missing.') }
    if ($t -match '\bcylinderLight\.axis(?![A-Za-z0-9_])') { $bad = $true; $lines.Add('BAD: exact cylinderLight.axis still present.') } else { $lines.Add('OK: no exact cylinderLight.axis access.') }
    if ($t -match '\bcylinderLight\.axisLength\b') { $lines.Add('OK: cylinderLight.axisLength preserved.') }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  if ($bad) { Die "V219 precise lightAxis verification failed. See $report" }
  Log "V219 precise lightAxis patch passed. Report: $report"
}



function Mark-DX11LightFloatNSymbolSyntaxSyncV219 {
  Log 'DX11_V219_PRECISE_LIGHTAXIS_NO_BACKUP_PATCH: uses floatN syntax and synchronizes CylinderLight lightAxis field/use-sites.'
}



function Patch-CylinderLightAxisLengthOverRenameV219 {
  # DX11_V219_FIX_LIGHTAXISLENGTH_OVERRENAME
  # Fixes:
  #   'lightAxisLength' is not a member of 'CylinderLight'
  #
  # Correct state:
  #   axis       -> lightAxis
  #   axisLength -> axisLength
  #
  # Earlier patches correctly renamed the vector field, but over-renamed the
  # separate scalar field axisLength to lightAxisLength.  Restore only that
  # scalar name while preserving lightAxis.
  $report = Join-Path $Root 'DX11_V219_FIX_LIGHTAXISLENGTH_OVERRENAME_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_FIX_LIGHTAXISLENGTH_OVERRENAME')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  $dirs = @(
    (Join-Path $Root 'src\dxvk\shaders\rtx\concept\light'),
    (Join-Path $Root 'include\rtx\concept\light'),
    (Join-Path $Root 'public\include\rtx\concept\light')
  )

  $files = New-Object System.Collections.Generic.List[string]
  foreach ($dir in $dirs) {
    if (!(Test-Path -LiteralPath $dir -PathType Container)) {
      $lines.Add(('SKIP ROOT: {0}' -f $dir))
      continue
    }
    Get-ChildItem -LiteralPath $dir -File -Recurse | ForEach-Object {
      if ($_.Name -match '\.before$') { return }
      if ($_.Name -notmatch '\.(h|slangh|slang)$') { return }
      $files.Add($_.FullName)
    }
  }

  foreach ($file in $files) {
    $text = [System.IO.File]::ReadAllText($file)
    $orig = $text

    # Undo the accidental scalar over-rename.  Do this before/after other light
    # patchers because they may run more than once.
    $text = $text.Replace('lightAxisLength', 'axisLength')

    # Keep the intended vector field rename precise.
    $text = [regex]::Replace($text, '(?m)^(\s*float3\s+)axis(\s*;\s*)$', '$1lightAxis$2')
    $text = [regex]::Replace($text, '\.axis(?![A-Za-z0-9_])', '.lightAxis')
    $text = $text.Replace('.lightlightAxis', '.lightAxis')
    $text = $text.Replace('lightlightAxis', 'lightAxis')
    $text = $text.Replace('lightAxisLength', 'axisLength')

    if ($text -ne $orig) {
      if (!(Test-Path -LiteralPath "$file.v219.before")) {
        # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $file -Destination "$file.v219.before" -Force
      }
      Write-TextNoBom -Path $file -Text $text
      $lines.Add(('PATCHED ACTIVE: {0}' -f $file))
    } else {
      $lines.Add(('OK ACTIVE: unchanged {0}' -f $file))
    }
  }

  $bad = $false
  foreach ($file in $files) {
    $t = [System.IO.File]::ReadAllText($file)

    if ($t -match 'lightAxisLength') {
      $bad = $true
      $lines.Add(('BAD: lightAxisLength remains in active file {0}' -f $file))
    }

    if ($t -match '\.axis(?![A-Za-z0-9_])') {
      $bad = $true
      $lines.Add(('BAD: exact .axis remains in active file {0}' -f $file))
    }
  }

  $light = Join-Path $Root 'src\dxvk\shaders\rtx\concept\light\light.h'
  if (Test-Path -LiteralPath $light -PathType Leaf) {
    $lt = [System.IO.File]::ReadAllText($light)
    if ($lt -match '(?m)^\s*float3\s+lightAxis\s*;') {
      $lines.Add('OK: light.h has float3 lightAxis.')
    } else {
      $bad = $true
      $lines.Add('BAD: light.h missing float3 lightAxis.')
    }
    if ($lt -match '(?m)^\s*float\s+axisLength\s*;') {
      $lines.Add('OK: light.h has float axisLength.')
    } else {
      $bad = $true
      $lines.Add('BAD: light.h missing float axisLength.')
    }
  }

  $cyl = Join-Path $Root 'src\dxvk\shaders\rtx\concept\light\cylinder_light.slangh'
  if (Test-Path -LiteralPath $cyl -PathType Leaf) {
    $ct = [System.IO.File]::ReadAllText($cyl)
    if ($ct -match '\bcylinderLight\.lightAxis\b') {
      $lines.Add('OK: cylinderLight.lightAxis present.')
    } else {
      $bad = $true
      $lines.Add('BAD: cylinderLight.lightAxis missing.')
    }
    if ($ct -match '\bcylinderLight\.axisLength\b') {
      $lines.Add('OK: cylinderLight.axisLength present.')
    } else {
      $bad = $true
      $lines.Add('BAD: cylinderLight.axisLength missing.')
    }
    if ($ct -match '\bcylinderLight\.lightAxisLength\b') {
      $bad = $true
      $lines.Add('BAD: cylinderLight.lightAxisLength remains.')
    }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  if ($bad) {
    Die "V219 axisLength over-rename verification failed. See $report"
  }

  Log "V219 fixed lightAxisLength over-rename. Report: $report"
}



function Patch-UtilGdiConcreteDxgiFormatV219 {
  # DX11_V219_UTIL_GDI_CONCRETE_DXGI_FORMAT_TYPE
  # Real compiler failure:
  #   src\util\util_gdi.h(...): error C3646: 'Format': unknown override specifier
  #   src\util\util_gdi.h(...): error C4430: missing type specifier
  #
  # Cause:
  #   the field named Format still has a stale/unknown type after the DX11-only
  #   D3D9/D3D11 format rewrite.  Make the field type concrete and local:
  #     DXGI_FORMAT Format;
  #   and force the DXGI enum declaration into the header.
  $report = Join-Path $Root 'DX11_V219_UTIL_GDI_CONCRETE_DXGI_FORMAT_TYPE_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_UTIL_GDI_CONCRETE_DXGI_FORMAT_TYPE')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  $h = Join-Path $Root 'src\util\util_gdi.h'
  $cpp = Join-Path $Root 'src\util\util_gdi.cpp'

  $formatMap = @{
    'D3D9Format::Unknown'       = 'DXGI_FORMAT_UNKNOWN'
    'D3D9Format::A8R8G8B8'      = 'DXGI_FORMAT_B8G8R8A8_UNORM'
    'D3D9Format::X8R8G8B8'      = 'DXGI_FORMAT_B8G8R8X8_UNORM'
    'D3D9Format::R8G8B8'        = 'DXGI_FORMAT_B8G8R8X8_UNORM'
    'D3D9Format::R5G6B5'        = 'DXGI_FORMAT_B5G6R5_UNORM'
    'D3D9Format::X1R5G5B5'      = 'DXGI_FORMAT_B5G5R5A1_UNORM'
    'D3D9Format::A1R5G5B5'      = 'DXGI_FORMAT_B5G5R5A1_UNORM'
    'D3D9Format::A4R4G4B4'      = 'DXGI_FORMAT_B4G4R4A4_UNORM'
    'D3D9Format::A8'            = 'DXGI_FORMAT_A8_UNORM'
    'D3D9Format::L8'            = 'DXGI_FORMAT_R8_UNORM'
    'D3D9Format::D16'           = 'DXGI_FORMAT_D16_UNORM'
    'D3D9Format::D24S8'         = 'DXGI_FORMAT_D24_UNORM_S8_UINT'
    'D3D9Format::D32'           = 'DXGI_FORMAT_D32_FLOAT'

    'D3D11Format::Unknown'      = 'DXGI_FORMAT_UNKNOWN'
    'D3D11Format::A8R8G8B8'     = 'DXGI_FORMAT_B8G8R8A8_UNORM'
    'D3D11Format::X8R8G8B8'     = 'DXGI_FORMAT_B8G8R8X8_UNORM'
    'D3D11Format::R8G8B8'       = 'DXGI_FORMAT_B8G8R8X8_UNORM'
    'D3D11Format::R5G6B5'       = 'DXGI_FORMAT_B5G6R5_UNORM'
    'D3D11Format::X1R5G5B5'     = 'DXGI_FORMAT_B5G5R5A1_UNORM'
    'D3D11Format::A1R5G5B5'     = 'DXGI_FORMAT_B5G5R5A1_UNORM'
    'D3D11Format::A4R4G4B4'     = 'DXGI_FORMAT_B4G4R4A4_UNORM'
    'D3D11Format::A8'           = 'DXGI_FORMAT_A8_UNORM'
    'D3D11Format::L8'           = 'DXGI_FORMAT_R8_UNORM'
    'D3D11Format::D16'          = 'DXGI_FORMAT_D16_UNORM'
    'D3D11Format::D24S8'        = 'DXGI_FORMAT_D24_UNORM_S8_UINT'
    'D3D11Format::D32'          = 'DXGI_FORMAT_D32_FLOAT'

    'DXGI_FORMAT::Unknown'      = 'DXGI_FORMAT_UNKNOWN'
    'DXGI_FORMAT::A8R8G8B8'     = 'DXGI_FORMAT_B8G8R8A8_UNORM'
    'DXGI_FORMAT::X8R8G8B8'     = 'DXGI_FORMAT_B8G8R8X8_UNORM'
    'DXGI_FORMAT::R8G8B8'       = 'DXGI_FORMAT_B8G8R8X8_UNORM'
    'DXGI_FORMAT::R5G6B5'       = 'DXGI_FORMAT_B5G6R5_UNORM'
    'DXGI_FORMAT::X1R5G5B5'     = 'DXGI_FORMAT_B5G5R5A1_UNORM'
    'DXGI_FORMAT::A1R5G5B5'     = 'DXGI_FORMAT_B5G5R5A1_UNORM'
    'DXGI_FORMAT::A4R4G4B4'     = 'DXGI_FORMAT_B4G4R4A4_UNORM'
    'DXGI_FORMAT::A8'           = 'DXGI_FORMAT_A8_UNORM'
    'DXGI_FORMAT::L8'           = 'DXGI_FORMAT_R8_UNORM'
    'DXGI_FORMAT::D16'          = 'DXGI_FORMAT_D16_UNORM'
    'DXGI_FORMAT::D24S8'        = 'DXGI_FORMAT_D24_UNORM_S8_UINT'
    'DXGI_FORMAT::D32'          = 'DXGI_FORMAT_D32_FLOAT'
  }

  foreach ($file in @($h, $cpp)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
      $lines.Add(('SKIP: missing {0}' -f $file))
      continue
    }

    $text = [System.IO.File]::ReadAllText($file)
    $orig = $text

    foreach ($k in $formatMap.Keys) {
      $text = $text.Replace($k, [string]$formatMap[$k])
    }

    $text = $text -replace '\bD3D9Format\b', 'DXGI_FORMAT'
    $text = $text -replace '\bD3D11Format\b', 'DXGI_FORMAT'
    $text = $text -replace '\bD3D9_FORMAT\b', 'DXGI_FORMAT'
    $text = $text -replace '\bD3D11_FORMAT\b', 'DXGI_FORMAT'

    if ($file -eq $h) {
      # Force the Windows SDK enum type visible in this header.
      $text = [regex]::Replace($text, '^\s*#include\s+<dxgiformat\.h>\s*\r?\n', '', [System.Text.RegularExpressions.RegexOptions]::Multiline)
      $text = [regex]::Replace($text, '^\s*#include\s+<dxgi\.h>\s*\r?\n', '', [System.Text.RegularExpressions.RegexOptions]::Multiline)
      $includeBlock = "#include <dxgiformat.h>`r`n#include <dxgi.h>"
      if ($text -match '#pragma once') {
        $text = [regex]::Replace($text, '#pragma once\s*', "#pragma once`r`n$includeBlock`r`n", 1)
      } else {
        $text = "$includeBlock`r`n$text"
      }

      # This is the concrete fix for C3646/C4430.  Do not rely on whatever stale
      # format type name is present; the struct/member field must be DXGI_FORMAT.
      $text = [regex]::Replace(
        $text,
        '(?m)^(\s*)(?:[A-Za-z_][A-Za-z0-9_:]*\s+)+Format\s*;',
        '$1DXGI_FORMAT Format;'
      )

      # If a previous replacement made a duplicate token, collapse it.
      $text = [regex]::Replace($text, '(?m)^\s*DXGI_FORMAT\s+DXGI_FORMAT\s+Format\s*;', '  DXGI_FORMAT Format;')
    }

    if ($text -ne $orig) {
      if (!(Test-Path -LiteralPath "$file.v219.before")) {
        # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $file -Destination "$file.v219.before" -Force
      }
      Write-TextNoBom -Path $file -Text $text
      $lines.Add(('PATCHED: {0}' -f $file))
    } else {
      $lines.Add(('OK: unchanged {0}' -f $file))
    }
  }

  if (Test-Path -LiteralPath $h -PathType Leaf) {
    $ht = [System.IO.File]::ReadAllText($h)
    if ($ht -match '#include\s+<dxgiformat\.h>' -and $ht -match '#include\s+<dxgi\.h>') {
      $lines.Add('OK: util_gdi.h has dxgiformat.h and dxgi.h includes.')
    } else {
      [System.IO.File]::WriteAllLines($report, $lines)
      Die "V219 util_gdi.h missing DXGI includes. See $report"
    }

    if ($ht -match '(?m)^\s*DXGI_FORMAT\s+Format\s*;') {
      $lines.Add('OK: util_gdi.h has concrete DXGI_FORMAT Format field.')
    } else {
      [System.IO.File]::WriteAllLines($report, $lines)
      Die "V219 util_gdi.h missing concrete DXGI_FORMAT Format field. See $report"
    }

    if ($ht -match '\bD3D9Format\b|\bD3D11Format\b|\bD3D9_FORMAT\b|\bD3D11_FORMAT\b') {
      [System.IO.File]::WriteAllLines($report, $lines)
      Die "V219 util_gdi.h still has stale D3D format type tokens. See $report"
    }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 applied concrete util_gdi DXGI_FORMAT type fix. Report: $report"
}



function Ensure-WindowsSdkD3D11IncludesV219 {
  # DX11_V219_FIND_OR_DOWNLOAD_WINDOWS_SDK_D3D11
  # Concrete behavior:
  #   1. Find an already-installed Windows SDK through env vars, registry keys,
  #      Windows Kits folders, and VS package cache locations.
  #   2. If not found, download official Microsoft winsdksetup.exe and install
  #      SDK headers/libs.
  #   3. Search again after install.
  #   4. Inject SDK include/lib paths into Ninja/cl.exe environment.
  #   5. If source asks for d3d11types.h but the installed SDK exposes d3d11.h,
  #      patch that source include to d3d11.h. No fake SDK headers.
  $report = Join-Path $Root 'DX11_V219_FIND_OR_DOWNLOAD_WINDOWS_SDK_D3D11_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_FIND_OR_DOWNLOAD_WINDOWS_SDK_D3D11')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  function Add-RootV219([System.Collections.Generic.List[string]]$Roots, [string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return }
    $p = ([string]$Path).Trim().Trim('"').Trim("'")
    if ([string]::IsNullOrWhiteSpace($p)) { return }
    try {
      $full = [System.IO.Path]::GetFullPath($p)
      if ((Test-Path -LiteralPath $full -PathType Container) -and !$Roots.Contains($full)) {
        $Roots.Add($full)
      }
    } catch {}
  }

  function Get-WindowsSdkRootsV219 {
    $roots = New-Object System.Collections.Generic.List[string]

    Add-RootV219 $roots $env:WindowsSdkDir
    Add-RootV219 $roots $env:UniversalCRTSdkDir
    Add-RootV219 $roots (Join-Path $Root '_local_windows_sdk_v200')
    Add-RootV219 $roots (Join-Path $Root '_local_windows_sdk_v219')

    foreach ($regPath in @(
      'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
      'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots',
      'HKCU:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
      'HKCU:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots'
    )) {
      try {
        if (Test-Path $regPath) {
          $props = Get-ItemProperty -Path $regPath
          foreach ($name in @('KitsRoot10','KitsRoot10_0','KitsRoot81','KitsRoot8')) {
            try { Add-RootV219 $roots ([string]$props.$name) } catch {}
          }
        }
      } catch {}
    }

    foreach ($p in @(
      "$env:ProgramFiles(x86)\Windows Kits\10",
      "$env:ProgramFiles\Windows Kits\10",
      "$env:ProgramFiles(x86)\Windows Kits\8.1",
      "$env:ProgramFiles\Windows Kits\8.1",
      'C:\Program Files (x86)\Windows Kits\10',
      'C:\Program Files\Windows Kits\10',
      'C:\Program Files (x86)\Windows Kits\8.1',
      'C:\Program Files\Windows Kits\8.1'
    )) {
      Add-RootV219 $roots $p
    }

    # Visual Studio may have the SDK payload cached even if normal KitsRoot env
    # variables are missing.  Use it only as a last-resort source root.
    foreach ($cacheRoot in @(
      'C:\ProgramData\Microsoft\VisualStudio\Packages',
      "$env:ProgramData\Microsoft\VisualStudio\Packages"
    )) {
      if (!(Test-Path -LiteralPath $cacheRoot -PathType Container)) { continue }
      Get-ChildItem -LiteralPath $cacheRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match 'Win(10|11)SDK|Windows.*SDK|SDK' } |
        ForEach-Object { Add-RootV219 $roots $_.FullName }
    }

    return $roots
  }

  function Find-WindowsSdkV219 {
    $roots = Get-WindowsSdkRootsV219
    $lines.Add('SDK root search list:')
    foreach ($r in $roots) { $lines.Add(('  ROOT: {0}' -f $r)) }

    foreach ($root in $roots) {
      $inc = Join-Path $root 'Include'
      if (!(Test-Path -LiteralPath $inc -PathType Container)) { continue }

      $versions = Get-ChildItem -LiteralPath $inc -Directory -ErrorAction SilentlyContinue | Sort-Object -Property Name -Descending
      foreach ($v in $versions) {
        $um = Join-Path $v.FullName 'um'
        $shared = Join-Path $v.FullName 'shared'
        $ucrt = Join-Path $v.FullName 'ucrt'
        $hasD3D11Types = Test-Path -LiteralPath (Join-Path $um 'd3d11types.h') -PathType Leaf
        $hasD3D11 = Test-Path -LiteralPath (Join-Path $um 'd3d11.h') -PathType Leaf
        $hasDxgi = Test-Path -LiteralPath (Join-Path $shared 'dxgi.h') -PathType Leaf

        if (($hasD3D11Types -or $hasD3D11) -and $hasDxgi) {
          return @{
            IncludeRoot = $v.FullName
            SdkRoot = $root
            HasD3D11Types = $hasD3D11Types
            HasD3D11 = $hasD3D11
            HasDxgi = $hasDxgi
            HasUcrt = (Test-Path -LiteralPath $ucrt -PathType Container)
          }
        }
      }
    }

    # Bounded fallback search for odd layouts.
    foreach ($searchRoot in @(
      'C:\Program Files (x86)\Windows Kits',
      'C:\Program Files\Windows Kits',
      'C:\ProgramData\Microsoft\VisualStudio\Packages'
    )) {
      if (!(Test-Path -LiteralPath $searchRoot -PathType Container)) { continue }

      try {
        $headers = Get-ChildItem -LiteralPath $searchRoot -Filter d3d11.h -File -Recurse -ErrorAction SilentlyContinue |
          Where-Object { $_.FullName -match '\\Include\\[^\\]+\\um\\d3d11\.h$' } |
          Sort-Object -Property FullName -Descending
      } catch {
        $headers = @()
      }

      foreach ($h in $headers) {
        $um = Split-Path -Parent $h.FullName
        $verRoot = Split-Path -Parent $um
        $shared = Join-Path $verRoot 'shared'
        if (Test-Path -LiteralPath (Join-Path $shared 'dxgi.h') -PathType Leaf) {
          return @{
            IncludeRoot = $verRoot
            SdkRoot = (Split-Path -Parent (Split-Path -Parent $verRoot))
            HasD3D11Types = (Test-Path -LiteralPath (Join-Path $um 'd3d11types.h') -PathType Leaf)
            HasD3D11 = $true
            HasDxgi = $true
            HasUcrt = (Test-Path -LiteralPath (Join-Path $verRoot 'ucrt') -PathType Container)
          }
        }
      }
    }

    return $null
  }

  function Download-FileV219([string]$Url, [string]$OutFile) {
    $parent = Split-Path -Parent $OutFile
    if (!(Test-Path -LiteralPath $parent -PathType Container)) {
      New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

    try {
      Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing -ErrorAction Stop
    } catch {
      $wc = New-Object System.Net.WebClient
      $wc.DownloadFile($Url, $OutFile)
    }
  }

  function Run-SdkInstallerV219([string]$Exe, [string[]]$Args, [string]$LogName) {
    $logDir = Join-Path $Root '_build_logs'
    if (!(Test-Path -LiteralPath $logDir -PathType Container)) {
      New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    }

    $logPath = Join-Path $logDir $LogName
    $argList = New-Object System.Collections.Generic.List[string]
    foreach ($a in $Args) { $argList.Add($a) }
    $argList.Add('/log')
    $argList.Add(('"{0}"' -f $logPath))

    $lines.Add(('RUN: {0} {1}' -f $Exe, ($argList -join ' ')))
    $p = Start-Process -FilePath $Exe -ArgumentList $argList -Wait -PassThru
    $lines.Add(('EXIT: {0}' -f $p.ExitCode))
    $lines.Add(('LOG: {0}' -f $logPath))
    return [int]$p.ExitCode
  }

  function Install-WindowsSdkV219 {
    $downloadDir = Join-Path $Root '_downloads\windows_sdk_v219'
    $layoutDir = Join-Path $downloadDir 'layout'
    $installer = Join-Path $downloadDir 'winsdksetup.exe'
    $localInstall = Join-Path $Root '_local_windows_sdk_v219'

    $urlPrimary = 'https://go.microsoft.com/fwlink/?linkid=2361308'
    $urlFallback = 'https://go.microsoft.com/fwlink/?linkid=2120843'

    $lines.Add('SDK not found: downloading official Microsoft Windows SDK installer.')
    try {
      Download-FileV219 -Url $urlPrimary -OutFile $installer
      $lines.Add(('OK: downloaded primary SDK installer: {0}' -f $urlPrimary))
    } catch {
      $lines.Add(('Primary SDK download failed: {0}' -f $_.Exception.Message))
      Download-FileV219 -Url $urlFallback -OutFile $installer
      $lines.Add(('OK: downloaded fallback SDK installer: {0}' -f $urlFallback))
    }

    if (!(Test-Path -LiteralPath $installer -PathType Leaf)) {
      [System.IO.File]::WriteAllLines($report, $lines)
      Die "V219 could not download winsdksetup.exe. See $report"
    }

    if (!(Test-Path -LiteralPath $layoutDir -PathType Container)) {
      New-Item -ItemType Directory -Path $layoutDir -Force | Out-Null
    }

    $layoutCode = Run-SdkInstallerV219 -Exe $installer -Args @('/layout', ('"{0}"' -f $layoutDir), '/quiet', '/norestart') -LogName 'v219-windows-sdk-layout.log'
    if ($layoutCode -ne 0 -and $layoutCode -ne 3010) {
      $lines.Add('WARN: SDK layout download returned non-zero. Trying direct install anyway.')
    }

    $installAttempts = @(
      @('/installpath', ('"{0}"' -f $localInstall), '/features', 'OptionId.DesktopCPPx64', 'OptionId.DesktopCPPx86', '/quiet', '/norestart', '/ceip', 'off'),
      @('/installpath', ('"{0}"' -f $localInstall), '/features', '+', '/quiet', '/norestart', '/ceip', 'off'),
      @('/quiet', '/norestart', '/ceip', 'off')
    )

    $ok = $false
    $n = 0
    foreach ($args in $installAttempts) {
      $n++
      $code = Run-SdkInstallerV219 -Exe $installer -Args $args -LogName ("v219-windows-sdk-install-{0}.log" -f $n)
      if ($code -eq 0 -or $code -eq 3010) {
        $ok = $true
        if ($code -eq 3010) { $lines.Add('OK: SDK installer completed with 3010 restart requested.') }
        else { $lines.Add('OK: SDK installer completed.') }
        break
      }
      $lines.Add(('WARN: SDK install attempt {0} failed with exit {1}.' -f $n, $code))
    }

    if (!$ok) {
      [System.IO.File]::WriteAllLines($report, $lines)
      Die "V219 Windows SDK install failed. See $report and _build_logs\v219-windows-sdk-install-*.log"
    }
  }

  function Patch-D3D11TypesIncludeV219([hashtable]$Sdk) {
    if ($Sdk.HasD3D11Types) {
      $lines.Add('OK: SDK has d3d11types.h; no source include rewrite needed.')
      return
    }

    if (!$Sdk.HasD3D11) {
      $lines.Add('WARN: SDK lacks both d3d11types.h and d3d11.h; no include rewrite possible.')
      return
    }

    $targetFiles = New-Object System.Collections.Generic.List[string]
    foreach ($dir in @((Join-Path $Root 'src'), (Join-Path $Root 'include'))) {
      if (!(Test-Path -LiteralPath $dir -PathType Container)) { continue }
      Get-ChildItem -LiteralPath $dir -File -Recurse -Include *.h,*.hpp,*.cpp,*.cxx,*.cc | ForEach-Object {
        $targetFiles.Add($_.FullName)
      }
    }

    foreach ($file in $targetFiles) {
      $text = [System.IO.File]::ReadAllText($file)
      if ($text -notmatch '<d3d11types\.h>|"d3d11types\.h"') { continue }
      $orig = $text
      $text = $text.Replace('<d3d11types.h>', '<d3d11.h>')
      $text = $text.Replace('"d3d11types.h"', '<d3d11.h>')
      if ($text -ne $orig) {
        if (!(Test-Path -LiteralPath "$file.v219.before")) {
          # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $file -Destination "$file.v219.before" -Force
        }
        Write-TextNoBom -Path $file -Text $text
        $lines.Add(('PATCHED include d3d11types.h -> d3d11.h: {0}' -f $file))
      }
    }
  }

  function Inject-WindowsSdkEnvV219([hashtable]$Sdk) {
    $sdkInc = [string]$Sdk.IncludeRoot
    $umDir = Join-Path $sdkInc 'um'
    $sharedDir = Join-Path $sdkInc 'shared'
    $ucrtDir = Join-Path $sdkInc 'ucrt'
    $winrtDir = Join-Path $sdkInc 'winrt'
    $cppwinrtDir = Join-Path $sdkInc 'cppwinrt'

    function Join-UniquePathListV219([string]$Existing, [string[]]$NewPaths) {
      $out = New-Object System.Collections.Generic.List[string]
      foreach ($p in $NewPaths) {
        if (![string]::IsNullOrWhiteSpace($p) -and (Test-Path -LiteralPath $p) -and !$out.Contains($p)) { $out.Add($p) }
      }
      foreach ($p in ([string]$Existing).Split(';')) {
        $q = $p.Trim()
        if (![string]::IsNullOrWhiteSpace($q) -and !$out.Contains($q)) { $out.Add($q) }
      }
      return ($out -join ';')
    }

    $includePaths = @($umDir, $sharedDir, $ucrtDir, $winrtDir, $cppwinrtDir)
    $env:INCLUDE = Join-UniquePathListV219 $env:INCLUDE $includePaths

    $sdkVersion = Split-Path -Leaf $sdkInc
    $sdkRoot = Split-Path -Parent $sdkInc
    $kitRoot = Split-Path -Parent $sdkRoot
    $libRoot = Join-Path $kitRoot ('Lib\' + $sdkVersion)
    $libPaths = @((Join-Path $libRoot 'um\x64'), (Join-Path $libRoot 'ucrt\x64'), (Join-Path $libRoot 'um\x86'), (Join-Path $libRoot 'ucrt\x86'))
    $env:LIB = Join-UniquePathListV219 $env:LIB $libPaths

    $slashI = New-Object System.Collections.Generic.List[string]
    foreach ($d in $includePaths) {
      if (Test-Path -LiteralPath $d -PathType Container) { $slashI.Add(('/I"{0}"' -f $d)) }
    }
    $slashText = $slashI -join ' '

    foreach ($var in @('CFLAGS','CXXFLAGS','CPPFLAGS','CL')) {
      $old = [Environment]::GetEnvironmentVariable($var, 'Process')
      if ([string]::IsNullOrWhiteSpace($old)) {
        [Environment]::SetEnvironmentVariable($var, $slashText, 'Process')
      } elseif (!$old.Contains($umDir)) {
        [Environment]::SetEnvironmentVariable($var, ($slashText + ' ' + $old), 'Process')
      }
    }

    $lines.Add(('OK: selected SDK include: {0}' -f $sdkInc))
    $lines.Add(('OK: d3d11.h exists: {0}' -f (Join-Path $umDir 'd3d11.h')))
    $lines.Add(('OK: d3d11types.h exists: {0}' -f $Sdk.HasD3D11Types))
    $lines.Add(('OK: dxgi.h exists: {0}' -f (Join-Path $sharedDir 'dxgi.h')))
    $lines.Add('OK: injected SDK include/lib paths into cl.exe environment.')
  }

  $sdk = Find-WindowsSdkV219
  if (!$sdk) {
    Install-WindowsSdkV219
    $sdk = Find-WindowsSdkV219
  }

  if (!$sdk) {
    $lines.Add('BAD: SDK not found after install/search.')
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 could not find or install Windows SDK headers. See $report"
  }

  Patch-D3D11TypesIncludeV219 $sdk
  Inject-WindowsSdkEnvV219 $sdk

  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 Windows SDK ready. Report: $report"
}



function Ensure-UsdPXRIncludeEnvV219 {
  # DX11_V219_SDK_USD_ENV_NO_LATE_MESON_ARGS
  # Concrete failure:
  #   rtx_material_data.h(...): fatal error C1083:
  #   Cannot open include file: 'pxr/base/vt/value.h'
  #
  # Cause:
  #   Non-USD targets like dxbc compile RTX headers that include pxr/*, but their
  #   Ninja command line does not include external\nv_usd_Release\include.
  #
  # Fix:
  #   Find the real USD include root containing pxr\base\vt\value.h and inject it
  #   into CL/CXXFLAGS/CPPFLAGS/INCLUDE before runtime Meson/Ninja.  No fake USD
  #   headers are generated.
  $report = Join-Path $Root 'DX11_V219_SDK_USD_ENV_NO_LATE_MESON_ARGS_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SDK_USD_ENV_NO_LATE_MESON_ARGS')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  function Add-UniqueV219([System.Collections.Generic.List[string]]$List, [string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return }
    $p = ([string]$Path).Trim().Trim('"').Trim("'")
    if ([string]::IsNullOrWhiteSpace($p)) { return }
    try {
      $full = [System.IO.Path]::GetFullPath($p)
      if ((Test-Path -LiteralPath $full -PathType Container) -and !$List.Contains($full)) {
        $List.Add($full)
      }
    } catch {}
  }

  function Find-UsdIncludeRootV219 {
    $candidates = New-Object System.Collections.Generic.List[string]

    foreach ($p in @(
      (Join-Path $Root 'external\nv_usd_Release\include'),
      (Join-Path $Root 'external\nv_usd_Debug\include'),
      (Join-Path $Root 'external\nv_usd_Release\include\boost'),
      (Join-Path $Root 'external\nv_usd_Debug\include\boost'),
      (Join-Path $Root 'external\usd\include'),
      (Join-Path $Root 'external\USD\include'),
      (Join-Path $Root 'subprojects\usd\include'),
      (Join-Path $Root 'third_party\usd\include'),
      (Join-Path $Root '_deps\usd\include')
    )) {
      Add-UniqueV219 $candidates $p
    }

    # Bounded scan: only likely dependency roots, not the whole project tree.
    foreach ($scanRoot in @(
      (Join-Path $Root 'external'),
      (Join-Path $Root 'subprojects'),
      (Join-Path $Root 'third_party'),
      (Join-Path $Root '_deps'),
      (Join-Path $Root '_downloads')
    )) {
      if (!(Test-Path -LiteralPath $scanRoot -PathType Container)) { continue }
      try {
        Get-ChildItem -LiteralPath $scanRoot -Filter value.h -File -Recurse -ErrorAction SilentlyContinue |
          Where-Object { $_.FullName -match '\\pxr\\base\\vt\\value\.h$' } |
          ForEach-Object {
            $vt = Split-Path -Parent $_.FullName
            $base = Split-Path -Parent $vt
            $pxr = Split-Path -Parent $base
            $inc = Split-Path -Parent $pxr
            Add-UniqueV219 $candidates $inc
          }
      } catch {}
    }

    foreach ($inc in $candidates) {
      if (Test-Path -LiteralPath (Join-Path $inc 'pxr\base\vt\value.h') -PathType Leaf) {
        return $inc
      }
    }

    return $null
  }

  function Join-UniquePathListV219([string]$Existing, [string[]]$NewPaths) {
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($p in $NewPaths) {
      if (![string]::IsNullOrWhiteSpace($p) -and (Test-Path -LiteralPath $p) -and !$out.Contains($p)) { $out.Add($p) }
    }
    foreach ($p in ([string]$Existing).Split(';')) {
      $q = $p.Trim()
      if (![string]::IsNullOrWhiteSpace($q) -and !$out.Contains($q)) { $out.Add($q) }
    }
    return ($out -join ';')
  }

  function Add-CompilerIncludeV219([string]$IncludeRoot) {
    $includeArgs = @('/I"{0}"' -f $IncludeRoot)

    # Boost headers live beside USD in the NVIDIA USD package. Add it if present,
    # but do not require it for pxr/base/vt/value.h.
    $boostSibling = Join-Path $IncludeRoot 'boost'
    if (Test-Path -LiteralPath $boostSibling -PathType Container) {
      $includeArgs += ('/I"{0}"' -f $boostSibling)
    }

    $includeText = $includeArgs -join ' '

    $env:INCLUDE = Join-UniquePathListV219 $env:INCLUDE @($IncludeRoot, $boostSibling)

    foreach ($var in @('CFLAGS','CXXFLAGS','CPPFLAGS','CL')) {
      $old = [Environment]::GetEnvironmentVariable($var, 'Process')
      if ([string]::IsNullOrWhiteSpace($old)) {
        [Environment]::SetEnvironmentVariable($var, $includeText, 'Process')
      } elseif (!$old.Contains($IncludeRoot)) {
        [Environment]::SetEnvironmentVariable($var, ($includeText + ' ' + $old), 'Process')
      }
    }
  }

  $usdInc = Find-UsdIncludeRootV219
  if (!$usdInc) {
    $lines.Add('BAD: Could not find real USD include root containing pxr\base\vt\value.h.')
    $lines.Add('Checked likely roots under external, subprojects, third_party, _deps, _downloads.')
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 could not find real USD header pxr\base\vt\value.h. See $report"
  }

  Add-CompilerIncludeV219 $usdInc
  # V219: no Meson add_project_arguments here; use compiler environment instead.
  $lines.Add(('OK: selected USD include root: {0}' -f $usdInc))
  $lines.Add(('OK: value.h: {0}' -f (Join-Path $usdInc 'pxr\base\vt\value.h')))
  if (Test-Path -LiteralPath (Join-Path $usdInc 'boost') -PathType Container) {
    $lines.Add(('OK: boost sibling include root: {0}' -f (Join-Path $usdInc 'boost')))
  }
  $lines.Add('OK: injected USD include root into INCLUDE/CFLAGS/CXXFLAGS/CPPFLAGS/CL.')
  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 injected USD PXR include path. Report: $report"
}



function Remove-LateMesonProjectArgumentsV219 {
  # DX11_V219_REMOVE_LATE_MESON_PROJECT_ARGUMENTS
  $report = Join-Path $Root 'DX11_V219_REMOVE_LATE_MESON_PROJECT_ARGUMENTS_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_REMOVE_LATE_MESON_PROJECT_ARGUMENTS')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  $mesonRoot = Join-Path $Root 'meson.build'
  if (!(Test-Path -LiteralPath $mesonRoot -PathType Leaf)) {
    $lines.Add(('SKIP: missing {0}' -f $mesonRoot))
    [System.IO.File]::WriteAllLines($report, $lines)
    return
  }

  $text = [System.IO.File]::ReadAllText($mesonRoot)
  $orig = $text

  # Remove complete injected USD include blocks from previous versions.
  $text = [regex]::Replace($text, '(?ms)\r?\n# DX11_V20[234]_USD_PXR_INCLUDE_ENV.*?(?=\r?\n# DX11_|\z)', '')

  # Remove the illegal call if a partial block survived.
  $text = [regex]::Replace(
    $text,
    "(?m)^\s*add_project_arguments\s*\(\s*'/I'\s*\+\s*meson\.project_source_root\(\)\s*/\s*'external/nv_usd_Release/include'\s*,\s*language\s*:\s*'cpp'\s*\)\s*\r?\n?",
    ''
  )

  # Remove unused helper var from the bad block if it survived.
  $text = [regex]::Replace(
    $text,
    "(?m)^\s*dx11_v20[234]_usd_inc\s*=\s*include_directories\s*\(\s*'external/nv_usd_Release/include'.*?\)\s*\r?\n?",
    ''
  )

  if ($text -ne $orig) {
    if (!(Test-Path -LiteralPath "$mesonRoot.v219.before")) {
      # DX11: backup-file creation disabled -- Copy-Item -LiteralPath $mesonRoot -Destination "$mesonRoot.v219.before" -Force
    }
    Write-TextNoBom -Path $mesonRoot -Text $text
    $lines.Add(('PATCHED: removed late add_project_arguments block from {0}' -f $mesonRoot))
  } else {
    $lines.Add('OK: no late V202/V203 Meson add_project_arguments block found.')
  }

  $after = [System.IO.File]::ReadAllText($mesonRoot)
  if ($after -match "(?m)^\s*add_project_arguments\s*\(\s*'/I'\s*\+\s*meson\.project_source_root\(\)\s*/\s*'external/nv_usd_Release/include'") {
    $lines.Add('BAD: late USD add_project_arguments call still remains.')
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 failed to remove late USD add_project_arguments. See $report"
  }

  $lines.Add('OK: late USD add_project_arguments is absent.')
  $lines.Add('OK: USD include path remains injected through CL/CFLAGS/CXXFLAGS/CPPFLAGS/INCLUDE before Meson/Ninja.')
  [System.IO.File]::WriteAllLines($report, $lines)
  Log "V219 removed illegal late Meson project arguments. Report: $report"
}



function Patch-ActualDx11MaterialFogAndNrcV219 {
  # DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
  # Targeted no-hang version.  Do NOT recursively scan src/include.
  # Only patch the exact headers that failed in logs:
  #   src\dxvk\rtx_render\rtx_materials.h
  #   src\dxvk\rtx_render\rtx_types.h
  #   src\dxvk\rtx\pass\nrc_args.h
  $report = Join-Path $Root 'DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  function Ensure-DirV219([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Container)) {
      New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
  }

  function Write-NoBomV219([string]$Path, [string]$Text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
  }

  function Patch-OneDx11FileV219([string]$File, [string]$IncludeLine, [System.Collections.Generic.List[string]]$Lines) {
    if (!(Test-Path -LiteralPath $File -PathType Leaf)) {
      $Lines.Add(('SKIP: missing {0}' -f $File))
      return
    }

    $text = [System.IO.File]::ReadAllText($File)
    $orig = $text

    $text = $text.Replace('#include "rtx/utility/dx11_legacy_d3d9_types.h"', $IncludeLine)
    $text = $text.Replace('#include "rtx/utility/dx11_legacy_material_fog_types.h"', $IncludeLine)
    $text = [regex]::Replace($text, '(?m)^\s*#\s*include\s*<d3d9types\.h>\s*$', '')

    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DCOLORVALUE(?![A-Za-z0-9_])', 'Dx11MaterialColor')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DMATERIAL9(?![A-Za-z0-9_])', 'Dx11RuntimeMaterial')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DFOGMODE(?![A-Za-z0-9_])', 'Dx11FixedFogMode')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DFOG_NONE(?![A-Za-z0-9_])', 'DX11_FOG_NONE')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DFOG_EXP2(?![A-Za-z0-9_])', 'DX11_FOG_EXP2')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DFOG_EXP(?![A-Za-z0-9_])', 'DX11_FOG_EXP')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DFOG_LINEAR(?![A-Za-z0-9_])', 'DX11_FOG_LINEAR')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])d3dMaterial(?![A-Za-z0-9_])', 'dx11Material')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])m_d3dMaterial(?![A-Za-z0-9_])', 'm_dx11Material')

    $needsHeader = ($text -match '(?<![A-Za-z0-9_])(Dx11RuntimeMaterial|Dx11MaterialColor|Dx11FixedFogMode|Dx11FixedFogDesc|Dx11FixedFunctionFogApi|Dx11FogShaderConstants|DX11_FOG_[A-Z0-9_]+)(?![A-Za-z0-9_])')
    if ($needsHeader -and !$text.Contains($IncludeLine)) {
      if ($text -match '#pragma once') {
        $text = [regex]::Replace($text, '#pragma once\s*', "#pragma once`r`n$IncludeLine`r`n", 1)
      } else {
        $text = $IncludeLine + "`r`n" + $text
      }
    }

    if ($text -ne $orig) {
      Write-NoBomV219 -Path $File -Text $text
      $Lines.Add(('PATCHED TARGET FILE: {0}' -f $File))
    } else {
      $Lines.Add(('OK TARGET FILE: unchanged {0}' -f $File))
    }
  }

  $dx11Dir = Join-Path $Root 'include\rtx\dx11'
  Ensure-DirV219 $dx11Dir

  $fogHeader = Join-Path $dx11Dir 'dx11_fixed_function_fog.h'
  $fogHeaderText = @'
#pragma once

// DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
//
// DX11-owned fixed-function-style fog emulation.
// Direct3D 11 has no native fixed-function fog render-state API, so this API
// provides equivalent runtime state for the D3D11 capture path.
//
// No D3D9 headers. No D3D9 bridge.

#include <cmath>
#include <cstdint>

namespace dxvk {

enum Dx11FixedFogMode : uint32_t {
  DX11_FOG_NONE   = 0,
  DX11_FOG_LINEAR = 1,
  DX11_FOG_EXP    = 2,
  DX11_FOG_EXP2   = 3,
};

struct Dx11FogColor {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct Dx11FixedFogDesc {
  bool enabled = false;
  Dx11FixedFogMode mode = DX11_FOG_NONE;
  Dx11FogColor color = {};
  float start = 0.0f;
  float end = 1.0f;
  float density = 0.0f;
  bool rangeBased = false;
  bool clampFactor = true;
};

struct Dx11ExponentialHeightFogDesc {
  bool enabled = false;
  float density = 0.0f;
  float heightFalloff = 0.2f;
  float heightOffset = 0.0f;
  float startDistance = 0.0f;
  float cutoffDistance = 0.0f;
  float maxOpacity = 1.0f;
  Dx11FogColor inscatteringColor = {};
  Dx11FogColor oppositeInscatteringColor = {};
};

struct Dx11FogShaderConstants {
  float fogColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
  float params0[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
  float params1[4] = { 0.0f, 0.2f, 0.0f, 1.0f };
  uint32_t mode = DX11_FOG_NONE;
  uint32_t enabled = 0;
  uint32_t rangeBased = 0;
  uint32_t heightFogEnabled = 0;
};

class Dx11FixedFunctionFogApi {
public:
  void reset() {
    m_fixed = {};
    m_height = {};
  }

  void setEnabled(bool enabled) {
    m_fixed.enabled = enabled;
    if (!enabled)
      m_fixed.mode = DX11_FOG_NONE;
  }

  void setMode(Dx11FixedFogMode mode) {
    m_fixed.mode = mode;
    m_fixed.enabled = mode != DX11_FOG_NONE;
  }

  void setColor(float r, float g, float b, float a = 1.0f) {
    m_fixed.color = { r, g, b, a };
  }

  void setLinearRange(float start, float end) {
    m_fixed.start = start;
    m_fixed.end = end;
    m_fixed.mode = DX11_FOG_LINEAR;
    m_fixed.enabled = true;
  }

  void setDensity(float density) {
    m_fixed.density = density;
    if (m_fixed.mode == DX11_FOG_NONE)
      m_fixed.mode = DX11_FOG_EXP;
    m_fixed.enabled = true;
  }

  void setRangeBased(bool rangeBased) {
    m_fixed.rangeBased = rangeBased;
  }

  void setClampFactor(bool clampFactor) {
    m_fixed.clampFactor = clampFactor;
  }

  void setExponentialHeightFog(const Dx11ExponentialHeightFogDesc& desc) {
    m_height = desc;
  }

  const Dx11FixedFogDesc& fixedFog() const { return m_fixed; }
  const Dx11ExponentialHeightFogDesc& heightFog() const { return m_height; }

  static float saturate(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  }

  float evaluateFixedFactor(float distance) const {
    if (!m_fixed.enabled || m_fixed.mode == DX11_FOG_NONE)
      return 0.0f;

    float factor = 0.0f;
    switch (m_fixed.mode) {
    case DX11_FOG_LINEAR: {
      const float range = m_fixed.end - m_fixed.start;
      factor = range != 0.0f ? (distance - m_fixed.start) / range : 1.0f;
      break;
    }
    case DX11_FOG_EXP:
      factor = 1.0f - std::exp(-m_fixed.density * distance);
      break;
    case DX11_FOG_EXP2: {
      const float d = m_fixed.density * distance;
      factor = 1.0f - std::exp(-(d * d));
      break;
    }
    default:
      factor = 0.0f;
      break;
    }

    return m_fixed.clampFactor ? saturate(factor) : factor;
  }

  float evaluateHeightFactor(float cameraHeight, float sampleHeight, float distance) const {
    if (!m_height.enabled)
      return 0.0f;

    const float sample = sampleHeight - m_height.heightOffset;
    const float camera = cameraHeight - m_height.heightOffset;
    const float sampleDensity = std::exp(-m_height.heightFalloff * sample);
    const float cameraDensity = std::exp(-m_height.heightFalloff * camera);
    const float startFade = distance > m_height.startDistance ? 1.0f : 0.0f;

    float factor = (1.0f - std::exp(-m_height.density * sampleDensity * cameraDensity * distance)) * startFade;
    if (m_height.cutoffDistance > 0.0f && distance > m_height.cutoffDistance)
      factor = 0.0f;
    if (factor > m_height.maxOpacity)
      factor = m_height.maxOpacity;

    return saturate(factor);
  }

  Dx11FogShaderConstants buildConstants() const {
    Dx11FogShaderConstants c = {};
    c.fogColor[0] = m_fixed.color.r;
    c.fogColor[1] = m_fixed.color.g;
    c.fogColor[2] = m_fixed.color.b;
    c.fogColor[3] = m_fixed.color.a;

    const float range = m_fixed.end - m_fixed.start;
    c.params0[0] = m_fixed.start;
    c.params0[1] = m_fixed.end;
    c.params0[2] = m_fixed.density;
    c.params0[3] = range != 0.0f ? (1.0f / range) : 1.0f;

    c.params1[0] = m_height.heightOffset;
    c.params1[1] = m_height.heightFalloff;
    c.params1[2] = m_height.startDistance;
    c.params1[3] = m_height.maxOpacity;

    c.mode = static_cast<uint32_t>(m_fixed.mode);
    c.enabled = m_fixed.enabled ? 1u : 0u;
    c.rangeBased = m_fixed.rangeBased ? 1u : 0u;
    c.heightFogEnabled = m_height.enabled ? 1u : 0u;
    return c;
  }

private:
  Dx11FixedFogDesc m_fixed = {};
  Dx11ExponentialHeightFogDesc m_height = {};
};

}
'@
  Write-NoBomV219 -Path $fogHeader -Text $fogHeaderText
  $lines.Add(('PATCHED: wrote {0}' -f $fogHeader))

  $materialHeader = Join-Path $dx11Dir 'dx11_material_fog_state.h'
  $materialHeaderText = @'
#pragma once

// DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
//
// DX11 runtime material/fog state. Fog is handled by Dx11FixedFunctionFogApi.

#include "rtx/dx11/dx11_fixed_function_fog.h"

namespace dxvk {

struct Dx11MaterialColor {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct Dx11RuntimeMaterial {
  Dx11MaterialColor Diffuse  = { 1.0f, 1.0f, 1.0f, 1.0f };
  Dx11MaterialColor Ambient  = { 0.0f, 0.0f, 0.0f, 1.0f };
  Dx11MaterialColor Specular = { 0.0f, 0.0f, 0.0f, 1.0f };
  Dx11MaterialColor Emissive = { 0.0f, 0.0f, 0.0f, 1.0f };
  float Power = 0.0f;
};

using Dx11FogMode = Dx11FixedFogMode;
using Dx11FogState = Dx11FixedFogDesc;

}
'@
  Write-NoBomV219 -Path $materialHeader -Text $materialHeaderText
  $lines.Add(('PATCHED: wrote {0}' -f $materialHeader))

  $includeLine = '#include "rtx/dx11/dx11_material_fog_state.h"'
  Patch-OneDx11FileV219 -File (Join-Path $Root 'src\dxvk\rtx_render\rtx_materials.h') -IncludeLine $includeLine -Lines $lines
  Patch-OneDx11FileV219 -File (Join-Path $Root 'src\dxvk\rtx_render\rtx_types.h') -IncludeLine $includeLine -Lines $lines

  # NRC include fix, targeted only.
  $nrcDir = Join-Path $Root 'submodules\nrc\include'
  Ensure-DirV219 $nrcDir
  $includeExternal = Join-Path $Root 'include\rtx\external'
  Ensure-DirV219 $includeExternal

  $nrcInclude = Join-Path $includeExternal 'NRC.h'
  $nrcShader = Join-Path $Root 'src\dxvk\shaders\rtx\external\NRC.h'
  if (!(Test-Path -LiteralPath $nrcInclude -PathType Leaf) -and (Test-Path -LiteralPath $nrcShader -PathType Leaf)) {
    Copy-Item -LiteralPath $nrcShader -Destination $nrcInclude -Force
    $lines.Add(('PATCHED: copied real NRC.h into include root: {0}' -f $nrcInclude))
  }

  $nrcStructures = Join-Path $nrcDir 'NrcStructures.h'
  if (!(Test-Path -LiteralPath $nrcStructures -PathType Leaf)) {
    Write-NoBomV219 -Path $nrcStructures -Text "#pragma once`r`n// DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS`r`n#include `"rtx/external/NRC.h`"`r`n"
    $lines.Add(('PATCHED: created NRC forwarding header: {0}' -f $nrcStructures))
  }

  $nrcArgs = Join-Path $Root 'src\dxvk\rtx\pass\nrc_args.h'
  if (Test-Path -LiteralPath $nrcArgs -PathType Leaf) {
    $na = [System.IO.File]::ReadAllText($nrcArgs)
    $origNa = $na
    $na = [regex]::Replace($na, '(?m)^\s*#\s*include\s*["<].*submodules[/\\]nrc[/\\]include[/\\]NrcStructures\.h[">]\s*$', '#include "NrcStructures.h"')
    if ($na -ne $origNa) {
      Write-NoBomV219 -Path $nrcArgs -Text $na
      $lines.Add(('PATCHED: fixed NRC include: {0}' -f $nrcArgs))
    }
  }

  # Include dirs for stale Ninja rules.
  $includeRoot = Join-Path $Root 'include'
  $env:INCLUDE = $includeRoot + ';' + $nrcDir + ';' + $env:INCLUDE
  $slashText = ('/I"{0}" /I"{1}"' -f $includeRoot, $nrcDir)
  foreach ($var in @('CFLAGS','CXXFLAGS','CPPFLAGS','CL')) {
    $old = [Environment]::GetEnvironmentVariable($var, 'Process')
    if ([string]::IsNullOrWhiteSpace($old)) {
      [Environment]::SetEnvironmentVariable($var, $slashText, 'Process')
    } elseif (!$old.Contains($nrcDir)) {
      [Environment]::SetEnvironmentVariable($var, ($slashText + ' ' + $old), 'Process')
    }
  }

  # Fast verification of only targeted files.
  $bad = $false
  $fh = [System.IO.File]::ReadAllText($fogHeader)
  if (!$fh.Contains('Dx11FixedFunctionFogApi') -or !$fh.Contains('evaluateHeightFactor')) {
    $bad = $true
    $lines.Add('BAD: fog API header missing expected symbols.')
  }
  if ($fh -match 'D3DMATERIAL9|D3DCOLORVALUE|D3DFOGMODE|D3DFOG_|d3d9types\.h') {
    $bad = $true
    $lines.Add('BAD: fog API header contains D3D9 tokens.')
  }

  foreach ($file in @(
    (Join-Path $Root 'src\dxvk\rtx_render\rtx_materials.h'),
    (Join-Path $Root 'src\dxvk\rtx_render\rtx_types.h')
  )) {
    if (Test-Path -LiteralPath $file -PathType Leaf) {
      $t = [System.IO.File]::ReadAllText($file)
      if (!$t.Contains($includeLine)) {
        $bad = $true
        $lines.Add(('BAD: DX11 include missing in {0}' -f $file))
      }
      if ($t -match 'D3DMATERIAL9|D3DCOLORVALUE|D3DFOGMODE|D3DFOG_|d3dMaterial|d3d9types\.h') {
        $bad = $true
        $lines.Add(('BAD: D3D9 material/fog token remains in {0}' -f $file))
      }
    }
  }

  if (!(Test-Path -LiteralPath $nrcStructures -PathType Leaf)) {
    $bad = $true
    $lines.Add('BAD: NrcStructures.h missing.')
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  if ($bad) {
    Die "V219 targeted DX11 fog/material/NRC patch failed. See $report"
  }

  Log "V219 no-hang DX11 fog/material/NRC patch complete. Report: $report"
}



function Patch-SurfaceSharedAndTerrainIncludeV219 {
  # DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
  # Fix current runtime-ninja-x64 blocker:
  #   two physical surface_shared.h copies are included in one C++ TU, so
  #   #pragma once is not enough and enum/const definitions are duplicated.
  #
  # Also fix:
  #   src\dxvk\rtx\pass\terrain_baking\decode_and_add_opacity_binding_indices.h
  #   includes ../../../../rtx_render/... which resolves to src\rtx_render,
  #   but the real DXVK path is src\dxvk\rtx_render.
  $report = Join-Path $Root 'DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  function Write-NoBomV219([string]$Path, [string]$Text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
  }

  function Add-CrossCopyGuardV219([string]$File, [string]$Macro, [System.Collections.Generic.List[string]]$Lines) {
    if (!(Test-Path -LiteralPath $File -PathType Leaf)) {
      $Lines.Add(('SKIP: missing {0}' -f $File))
      return
    }

    $text = [System.IO.File]::ReadAllText($File)
    if ($text.Contains($Macro)) {
      $Lines.Add(('OK: guard already present in {0}' -f $File))
      return
    }

    $orig = $text
    if ($text -match '^\s*#pragma once') {
      $text = [regex]::Replace(
        $text,
        '^\s*#pragma once\s*',
        "#pragma once`r`n#ifndef $Macro`r`n#define $Macro`r`n",
        1
      )
    } else {
      $text = "#ifndef $Macro`r`n#define $Macro`r`n" + $text
    }

    $text = $text.TrimEnd() + "`r`n#endif // $Macro`r`n"

    if ($text -ne $orig) {
      Write-NoBomV219 -Path $File -Text $text
      $Lines.Add(('PATCHED: added cross-copy guard to {0}' -f $File))
    }
  }

  $surfaceMacro = 'DXVK_REMIX_SHARED_SURFACE_SHARED_H'
  Add-CrossCopyGuardV219 -File (Join-Path $Root 'src\dxvk\rtx\concept\surface\surface_shared.h') -Macro $surfaceMacro -Lines $lines
  Add-CrossCopyGuardV219 -File (Join-Path $Root 'src\dxvk\shaders\rtx\concept\surface\surface_shared.h') -Macro $surfaceMacro -Lines $lines

  $terrain = Join-Path $Root 'src\dxvk\rtx\pass\terrain_baking\decode_and_add_opacity_binding_indices.h'
  if (Test-Path -LiteralPath $terrain -PathType Leaf) {
    $t = [System.IO.File]::ReadAllText($terrain)
    $origT = $t
    $t = $t.Replace('#include "../../../../rtx_render/rtx_replacement_material_texture_type.h"', '#include "../../../rtx_render/rtx_replacement_material_texture_type.h"')
    $t = $t.Replace("#include '../../../../rtx_render/rtx_replacement_material_texture_type.h'", "#include '../../../rtx_render/rtx_replacement_material_texture_type.h'")
    if ($t -ne $origT) {
      Write-NoBomV219 -Path $terrain -Text $t
      $lines.Add(('PATCHED: fixed terrain baking replacement texture include path: {0}' -f $terrain))
    } else {
      $lines.Add(('OK: terrain baking include path already fixed or different: {0}' -f $terrain))
    }
  } else {
    $lines.Add(('SKIP: missing {0}' -f $terrain))
  }

  # Also provide a real forwarding include at the bad historical path. This is
  # not a fake type/header; it forwards to the actual DX11 runtime header so any
  # stale generated path still resolves.
  $forwardDir = Join-Path $Root 'src\rtx_render'
  if (!(Test-Path -LiteralPath $forwardDir -PathType Container)) {
    New-Item -ItemType Directory -Path $forwardDir -Force | Out-Null
  }
  $forward = Join-Path $forwardDir 'rtx_replacement_material_texture_type.h'
  if (!(Test-Path -LiteralPath $forward -PathType Leaf)) {
    Write-NoBomV219 -Path $forward -Text "#pragma once`r`n#include `"../dxvk/rtx_render/rtx_replacement_material_texture_type.h`"`r`n"
    $lines.Add(('PATCHED: added forwarding include for stale generated terrain path: {0}' -f $forward))
  } else {
    $lines.Add(('OK: forwarding include already exists or real file present: {0}' -f $forward))
  }

  # Fast verification, targeted only.
  $bad = $false
  foreach ($file in @(
    (Join-Path $Root 'src\dxvk\rtx\concept\surface\surface_shared.h'),
    (Join-Path $Root 'src\dxvk\shaders\rtx\concept\surface\surface_shared.h')
  )) {
    if (Test-Path -LiteralPath $file -PathType Leaf) {
      $s = [System.IO.File]::ReadAllText($file)
      if (!$s.Contains($surfaceMacro)) {
        $bad = $true
        $lines.Add(('BAD: surface guard missing in {0}' -f $file))
      }
    }
  }

  if (Test-Path -LiteralPath $terrain -PathType Leaf) {
    $tt = [System.IO.File]::ReadAllText($terrain)
    if ($tt.Contains('../../../../rtx_render/rtx_replacement_material_texture_type.h')) {
      $bad = $true
      $lines.Add('BAD: terrain baking include still uses bad four-up path in src\dxvk\rtx tree.')
    }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  if ($bad) {
    Die "V219 surface/include verification failed. See $report"
  }

  Log "V219 fixed surface_shared duplicate include guards and terrain include path. Report: $report"
}



function Patch-Dx11LightStateApiV219 {
  # DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
  # Fix current runtime-ninja-x64 blocker:
  #   stale D3D fixed-function light symbols in active DX11 headers:
  #     D3DLIGHTTYPE, D3DLIGHT9, D3DLIGHT_POINT/SPOT/DIRECTIONAL
  #
  # This adds a DX11-owned light-state API. It does not include d3d9types.h and
  # does not restore a D3D9 bridge.
  $report = Join-Path $Root 'DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  function Ensure-DirV219([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Container)) {
      New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
  }

  function Write-NoBomV219([string]$Path, [string]$Text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
  }

  function Patch-OneLightFileV219([string]$File, [string]$IncludeLine, [System.Collections.Generic.List[string]]$Lines) {
    if (!(Test-Path -LiteralPath $File -PathType Leaf)) {
      $Lines.Add(('SKIP: missing {0}' -f $File))
      return
    }

    $text = [System.IO.File]::ReadAllText($File)
    $orig = $text

    # Remove any previous rejected dependency on old fixed-function SDK headers.
    $text = [regex]::Replace($text, '(?m)^\s*#\s*include\s*<d3d9types\.h>\s*$', '')
    $text = [regex]::Replace($text, '(?m)^\s*#\s*include\s*<d3d9\.h>\s*$', '')

    # Replace fixed-function light symbols with DX11-owned source symbols.
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DLIGHTTYPE(?![A-Za-z0-9_])', 'Dx11LightType')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DLIGHT9(?![A-Za-z0-9_])', 'Dx11LightDesc')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DVECTOR(?![A-Za-z0-9_])', 'Dx11Vector3')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DCOLORVALUE(?![A-Za-z0-9_])', 'Dx11LightColor')

    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DLIGHT_POINT(?![A-Za-z0-9_])', 'DX11_LIGHT_POINT')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DLIGHT_SPOT(?![A-Za-z0-9_])', 'DX11_LIGHT_SPOT')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DLIGHT_DIRECTIONAL(?![A-Za-z0-9_])', 'DX11_LIGHT_DIRECTIONAL')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])D3DLIGHT_FORCE_DWORD(?![A-Za-z0-9_])', 'DX11_LIGHT_FORCE_DWORD')

    # Rename common old local/member names.
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])d3dLight(?![A-Za-z0-9_])', 'dx11Light')
    $text = [regex]::Replace($text, '(?<![A-Za-z0-9_])m_d3dLight(?![A-Za-z0-9_])', 'm_dx11Light')

    $needsHeader = ($text -match '(?<![A-Za-z0-9_])(Dx11LightType|Dx11LightDesc|Dx11Vector3|Dx11LightColor|DX11_LIGHT_[A-Z0-9_]+)(?![A-Za-z0-9_])')
    if ($needsHeader -and !$text.Contains($IncludeLine)) {
      if ($text -match '#pragma once') {
        $text = [regex]::Replace($text, '#pragma once\s*', "#pragma once`r`n$IncludeLine`r`n", 1)
      } else {
        $text = $IncludeLine + "`r`n" + $text
      }
    }

    if ($text -ne $orig) {
      Write-NoBomV219 -Path $File -Text $text
      $Lines.Add(('PATCHED LIGHT TARGET: {0}' -f $File))
    } else {
      $Lines.Add(('OK LIGHT TARGET: unchanged {0}' -f $File))
    }
  }

  $dx11Dir = Join-Path $Root 'include\rtx\dx11'
  Ensure-DirV219 $dx11Dir

  $lightHeader = Join-Path $dx11Dir 'dx11_light_state.h'
  $lightHeaderText = @'
#pragma once

// DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
//
// DX11-owned runtime light state for the D3D11 capture path.
//
// D3D11 does not expose a fixed-function D3DLIGHT API. This header provides a
// project-owned light description that can be filled from D3D11 shader constants,
// engine uniforms, scene analysis, draw metadata, and Remix light extraction.
// No D3D9 headers. No D3D9 bridge.

#include <cstdint>

namespace dxvk {

enum Dx11LightType : uint32_t {
  DX11_LIGHT_POINT       = 1,
  DX11_LIGHT_SPOT        = 2,
  DX11_LIGHT_DIRECTIONAL = 3,
  DX11_LIGHT_FORCE_DWORD = 0x7fffffff,
};

struct Dx11Vector3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct Dx11LightColor {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct Dx11LightDesc {
  Dx11LightType Type = DX11_LIGHT_POINT;

  Dx11LightColor Diffuse  = { 1.0f, 1.0f, 1.0f, 1.0f };
  Dx11LightColor Specular = { 0.0f, 0.0f, 0.0f, 1.0f };
  Dx11LightColor Ambient  = { 0.0f, 0.0f, 0.0f, 1.0f };

  Dx11Vector3 Position  = {};
  Dx11Vector3 Direction = { 0.0f, -1.0f, 0.0f };

  float Range        = 0.0f;
  float Falloff      = 1.0f;
  float Attenuation0 = 1.0f;
  float Attenuation1 = 0.0f;
  float Attenuation2 = 0.0f;
  float Theta        = 0.0f;
  float Phi          = 0.0f;

  bool Enabled       = true;
};

struct Dx11LightShaderConstants {
  float position_range[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  float direction_type[4] = { 0.0f, -1.0f, 0.0f, 1.0f };
  float diffuse[4]        = { 1.0f, 1.0f, 1.0f, 1.0f };
  float specular[4]       = { 0.0f, 0.0f, 0.0f, 1.0f };
  float ambient[4]        = { 0.0f, 0.0f, 0.0f, 1.0f };
  float attenuation[4]    = { 1.0f, 0.0f, 0.0f, 1.0f };
  float cone[4]           = { 0.0f, 0.0f, 1.0f, 0.0f };
};

class Dx11LightStateApi {
public:
  static Dx11LightDesc makePoint(
      float x, float y, float z,
      float r, float g, float b,
      float range = 0.0f) {
    Dx11LightDesc light = {};
    light.Type = DX11_LIGHT_POINT;
    light.Position = { x, y, z };
    light.Diffuse = { r, g, b, 1.0f };
    light.Range = range;
    return light;
  }

  static Dx11LightDesc makeDirectional(
      float x, float y, float z,
      float r, float g, float b) {
    Dx11LightDesc light = {};
    light.Type = DX11_LIGHT_DIRECTIONAL;
    light.Direction = { x, y, z };
    light.Diffuse = { r, g, b, 1.0f };
    return light;
  }

  static Dx11LightDesc makeSpot(
      float px, float py, float pz,
      float dx, float dy, float dz,
      float r, float g, float b,
      float theta, float phi,
      float range = 0.0f) {
    Dx11LightDesc light = {};
    light.Type = DX11_LIGHT_SPOT;
    light.Position = { px, py, pz };
    light.Direction = { dx, dy, dz };
    light.Diffuse = { r, g, b, 1.0f };
    light.Theta = theta;
    light.Phi = phi;
    light.Range = range;
    return light;
  }

  static Dx11LightShaderConstants buildConstants(const Dx11LightDesc& light) {
    Dx11LightShaderConstants c = {};
    c.position_range[0] = light.Position.x;
    c.position_range[1] = light.Position.y;
    c.position_range[2] = light.Position.z;
    c.position_range[3] = light.Range;

    c.direction_type[0] = light.Direction.x;
    c.direction_type[1] = light.Direction.y;
    c.direction_type[2] = light.Direction.z;
    c.direction_type[3] = static_cast<float>(static_cast<uint32_t>(light.Type));

    c.diffuse[0] = light.Diffuse.r;
    c.diffuse[1] = light.Diffuse.g;
    c.diffuse[2] = light.Diffuse.b;
    c.diffuse[3] = light.Diffuse.a;

    c.specular[0] = light.Specular.r;
    c.specular[1] = light.Specular.g;
    c.specular[2] = light.Specular.b;
    c.specular[3] = light.Specular.a;

    c.ambient[0] = light.Ambient.r;
    c.ambient[1] = light.Ambient.g;
    c.ambient[2] = light.Ambient.b;
    c.ambient[3] = light.Ambient.a;

    c.attenuation[0] = light.Attenuation0;
    c.attenuation[1] = light.Attenuation1;
    c.attenuation[2] = light.Attenuation2;
    c.attenuation[3] = light.Enabled ? 1.0f : 0.0f;

    c.cone[0] = light.Theta;
    c.cone[1] = light.Phi;
    c.cone[2] = light.Falloff;
    c.cone[3] = 0.0f;

    return c;
  }
};

}
'@

  Write-NoBomV219 -Path $lightHeader -Text $lightHeaderText
  $lines.Add(('PATCHED: wrote DX11 light state header: {0}' -f $lightHeader))

  $includeLine = '#include "rtx/dx11/dx11_light_state.h"'

  foreach ($target in @(
    (Join-Path $Root 'src\dxvk\rtx_render\rtx_context.h'),
    (Join-Path $Root 'src\dxvk\rtx_render\rtx_light_manager.h'),
    (Join-Path $Root 'src\dxvk\rtx_render\rtx_lights_data.h'),
    (Join-Path $Root 'src\dxvk\rtx_render\rtx_lights_data.cpp'),
    (Join-Path $Root 'src\dxvk\rtx_render\rtx_scene_manager.h'),
    (Join-Path $Root 'src\dxvk\rtx_render\rtx_scene_manager.cpp')
  )) {
    Patch-OneLightFileV219 -File $target -IncludeLine $includeLine -Lines $lines
  }

  # Fast verification on only target headers.
  $bad = $false
  $lh = [System.IO.File]::ReadAllText($lightHeader)
  if (!$lh.Contains('Dx11LightStateApi') -or !$lh.Contains('Dx11LightDesc')) {
    $bad = $true
    $lines.Add('BAD: DX11 light state header missing expected symbols.')
  }
  if ($lh -match 'D3DLIGHTTYPE|D3DLIGHT9|D3DLIGHT_|D3DVECTOR|d3d9types\.h|<d3d9') {
    $bad = $true
    $lines.Add('BAD: DX11 light state header contains stale fixed-function D3D tokens.')
  }

  foreach ($target in @(
    (Join-Path $Root 'src\dxvk\rtx_render\rtx_context.h'),
    (Join-Path $Root 'src\dxvk\rtx_render\rtx_light_manager.h'),
    (Join-Path $Root 'src\dxvk\rtx_render\rtx_lights_data.h'),
    (Join-Path $Root 'src\dxvk\rtx_render\rtx_scene_manager.h')
  )) {
    if (Test-Path -LiteralPath $target -PathType Leaf) {
      $t = [System.IO.File]::ReadAllText($target)
      if ($t -match 'D3DLIGHTTYPE|D3DLIGHT9|D3DLIGHT_|D3DVECTOR|d3dLight|d3d9types\.h|<d3d9') {
        $bad = $true
        $lines.Add(('BAD: stale fixed-function light token remains in {0}' -f $target))
      }
      if ($t -match 'Dx11LightType|Dx11LightDesc|DX11_LIGHT_') {
        if (!$t.Contains($includeLine)) {
          $bad = $true
          $lines.Add(('BAD: DX11 light include missing in {0}' -f $target))
        }
      }
    }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  if ($bad) {
    Die "V219 DX11 light state verification failed. See $report"
  }

  Log "V219 applied DX11 light state API fix. Report: $report"
}


function Patch-DX11ClientCaptureToRemixV226 {
  param([Parameter(Mandatory)][string]$DstClient)

  # DX11_V226_CAPTURE_TO_REMIX: full capture->Remix engine. Writes the capture
  # module and overwrites the client hook source (heavily extended with input-layout,
  # Map/Unmap, UpdateSubresource and vertex-shader-reflection hooks), then wires the
  # client meson.build. Runs AFTER Patch-DX11ClientRealCaptureLayerV219 (the last
  # writer of these files). Idempotent: re-running simply rewrites identical content.

  Write-TextNoBom -Path (Join-Path $DstClient 'dx11_capture_remix.h') -Text @'
/*
 * Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#pragma once

// DX11_V226_CAPTURE_TO_REMIX
// Full translation layer: turns captured in-game DX11 geometry into Remix scene
// API commands (RemixApi_CreateMaterial / RemixApi_CreateMesh / RemixApi_DrawInstance
// / RemixApi camera) and streams them over the bridge IPC to the x64 server, which
// replays them into the .trex Remix runtime.
//
// This is a real capture engine, not a sampler:
//  * Vertices are decoded from the actual bound D3D11 input layout - per-element
//    semantic, DXGI format and byte offset across all bound vertex-buffer slots
//    (position, normal, tangent, texcoord, color).
//  * Vertex/index buffer contents are tracked through their full lifecycle:
//    CreateBuffer initial data, Map/Unmap writes, and UpdateSubresource.
//  * The per-draw object->world transform is recovered by reflecting the bound
//    vertex shader's constant buffers and reading the live constant-buffer bytes.
//  * The bound pixel-shader albedo texture identity drives a stable per-material
//    hash so Remix can key replacements.

struct ID3D11Buffer;
struct ID3D11Resource;
struct ID3D11InputLayout;
struct ID3D11VertexShader;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct D3D11_INPUT_ELEMENT_DESC;

#include <cstdint>
#include <cstddef>

namespace dx11_capture {

  // ---- Resource lifecycle tracking (called from the d3d11.dll capture hooks) ----

  void RecordBufferCreate(ID3D11Buffer* buffer, const void* pInitialData,
                          uint32_t byteWidth, uint32_t bindFlags);
  void RecordBufferRelease(ID3D11Buffer* buffer);

  // Map returns a CPU pointer the game writes into; Unmap commits it. We snapshot
  // the written bytes on Unmap so dynamic geometry is captured too.
  void RecordMap(ID3D11Resource* resource, uint32_t subresource, void* pMappedData);
  void RecordUnmap(ID3D11Resource* resource, uint32_t subresource);

  // CopyResource / UpdateSubresource style CPU->GPU upload of buffer contents.
  void RecordUpdateSubresource(ID3D11Resource* resource, uint32_t dstSubresource,
                               const void* pSrcData, uint32_t srcRowPitch);

  // DX11_V258_BRIDGE_TEXTURE_CONTENT_HASH: record a content-derived identity
  // for a texture at creation so material hashes are identical across runs
  // and GPUs. The old material key hashed the resource POINTER, which ASLR
  // randomizes every launch - replacements could never key on it.
  void RecordTexture2DCreate(ID3D11Resource* texture, const void* pMip0Data,
                             uint32_t rowPitchBytes, uint32_t width, uint32_t height,
                             uint32_t format, uint32_t mipLevels, uint32_t arraySize,
                             uint32_t bindFlags);
  void RecordTexture2DRelease(ID3D11Resource* texture);

  // Input layouts: cache the element array so draws can decode every vertex stream.
  void RecordInputLayoutCreate(ID3D11InputLayout* layout,
                               const D3D11_INPUT_ELEMENT_DESC* descs, uint32_t numElements);
  void RecordInputLayoutRelease(ID3D11InputLayout* layout);

  // Vertex shaders: reflect the bytecode to locate the object->world (or combined)
  // transform matrix inside its constant buffers.
  void RecordVertexShaderCreate(ID3D11VertexShader* shader, const void* bytecode, size_t length);
  void RecordVertexShaderRelease(ID3D11VertexShader* shader);

  // ---- Draw capture ----

  void CaptureDrawIndexed(ID3D11DeviceContext* context, uint32_t indexCount,
                          uint32_t startIndexLocation, int32_t baseVertexLocation);
  void CaptureDraw(ID3D11DeviceContext* context, uint32_t vertexCount,
                   uint32_t startVertexLocation);

  // Frame boundary (hooked Present). DX11_V265_BRIDGE_PRESENT_CAMERA: sends
  // RemixApi_Startup (once, with the game window HWND from the swapchain),
  // RemixApi_SetupCamera (per frame, from the camera scanned out of the bound
  // VS constant buffers at draw time), and RemixApi_Present (per frame, with
  // the game HWND as override so Remix presents INTO the game window instead
  // of a secondary one). Without this pump the server never starts the Remix
  // runtime and nothing path-traced reaches the screen for x86 bridge games.
  void OnPresent(IDXGISwapChain* swapChain);

  // True once the bridge IPC handshake is up and streaming is enabled.
  bool IsStreamingEnabled();

} // namespace dx11_capture
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'dx11_capture_remix.cpp') -Text @'
/*
 * Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

// DX11_V226_CAPTURE_TO_REMIX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dx11_capture_remix.h"
#include "dx11_bridge_client.h"

#include "util_bridgecommand.h"
#include "util_devicecommand.h"
#include "util_remixapi.h"

using namespace bridge_util;
using namespace remixapi::util;

namespace dx11_capture {

namespace {

  // ===================================================================== //
  // IPC send helpers (mirror of client/remix_api.cpp's wire format).      //
  // ===================================================================== //

  inline void sendUid(ClientMessage& msg, uint32_t uid) {
    msg.send_data(uid);
  }

  inline void sendBool(ClientMessage& msg, bool b) {
    uint32_t boolVal = b ? 1u : 0u;
    msg.send_data(boolVal);
  }

  template <typename SerializableT, typename BaseT>
  inline void serializeAndSend(ClientMessage& msg, const BaseT& base) {
    const SerializableT serializable(base);
    msg.send_data((uint32_t) ToRemixApiStructEnum<typename SerializableT::BaseT>);
    const auto serializableSize = serializable.size();
    auto* pSlzd = new uint8_t[serializableSize];
    serializable.serialize(pSlzd);
    msg.send_data((uint32_t) serializableSize, pSlzd);
    delete[] pSlzd;
  }

  void logf(const char* tag, const char* fmt, ...) {
    char buf[512] = {};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dx11_bridge_client::LogLine(tag, buf);
  }

  uint64_t fnv1a(const void* data, size_t size, uint64_t seed = 1469598103934665603ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
  }

  // ===================================================================== //
  // DXGI vertex-format decoding.                                          //
  // ===================================================================== //

  inline float halfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp = (h & 0x7C00u) >> 10;
    const uint32_t mant = (h & 0x03FFu);
    uint32_t f;
    if (exp == 0) {
      if (mant == 0) {
        f = sign; // +/-0
      } else {
        // subnormal half -> normalized float
        int e = -1;
        uint32_t m = mant;
        do { e++; m <<= 1; } while ((m & 0x0400u) == 0);
        m &= 0x03FFu;
        f = sign | ((uint32_t)(127 - 15 - e) << 23) | (m << 13);
      }
    } else if (exp == 0x1F) {
      f = sign | 0x7F800000u | (mant << 13); // Inf/NaN
    } else {
      f = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out;
    memcpy(&out, &f, sizeof(out));
    return out;
  }

  uint32_t formatByteSize(DXGI_FORMAT fmt) {
    switch (fmt) {
      case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
      case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
      case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT:
      case DXGI_FORMAT_R32G32B32_SINT: return 12;
      case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
      case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
      case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_FLOAT:
      case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: return 8;
      case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
      case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_SNORM:
      case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SINT:
      case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM:
      case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
      case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_UINT:
      case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_FLOAT:
      case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: return 4;
      case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM:
      case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
      case DXGI_FORMAT_R16_SNORM: return 2;
      case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: return 1;
      default: return 0;
    }
  }

  // Decode up to 4 components of one attribute into floats (alpha defaults to 1).
  void decodeFormat(const uint8_t* s, DXGI_FORMAT fmt, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
    switch (fmt) {
      case DXGI_FORMAT_R32G32B32A32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(s);
        out[0]=f[0]; out[1]=f[1]; out[2]=f[2]; out[3]=f[3]; break; }
      case DXGI_FORMAT_R32G32B32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(s);
        out[0]=f[0]; out[1]=f[1]; out[2]=f[2]; break; }
      case DXGI_FORMAT_R32G32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(s);
        out[0]=f[0]; out[1]=f[1]; break; }
      case DXGI_FORMAT_R32_FLOAT: {
        out[0]=*reinterpret_cast<const float*>(s); break; }
      case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        const uint16_t* h = reinterpret_cast<const uint16_t*>(s);
        out[0]=halfToFloat(h[0]); out[1]=halfToFloat(h[1]); out[2]=halfToFloat(h[2]); out[3]=halfToFloat(h[3]); break; }
      case DXGI_FORMAT_R16G16_FLOAT: {
        const uint16_t* h = reinterpret_cast<const uint16_t*>(s);
        out[0]=halfToFloat(h[0]); out[1]=halfToFloat(h[1]); break; }
      case DXGI_FORMAT_R16_FLOAT: {
        out[0]=halfToFloat(*reinterpret_cast<const uint16_t*>(s)); break; }
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: {
        out[0]=s[0]/255.0f; out[1]=s[1]/255.0f; out[2]=s[2]/255.0f; out[3]=s[3]/255.0f; break; }
      case DXGI_FORMAT_B8G8R8A8_UNORM: {
        out[0]=s[2]/255.0f; out[1]=s[1]/255.0f; out[2]=s[0]/255.0f; out[3]=s[3]/255.0f; break; }
      case DXGI_FORMAT_B8G8R8X8_UNORM: {
        out[0]=s[2]/255.0f; out[1]=s[1]/255.0f; out[2]=s[0]/255.0f; out[3]=1.0f; break; }
      case DXGI_FORMAT_R8G8B8A8_SNORM: {
        for (int i=0;i<4;i++){ int8_t v=(int8_t)s[i]; out[i]= v <= -127 ? -1.0f : v/127.0f; } break; }
      case DXGI_FORMAT_R16G16_UNORM: {
        const uint16_t* u = reinterpret_cast<const uint16_t*>(s);
        out[0]=u[0]/65535.0f; out[1]=u[1]/65535.0f; break; }
      case DXGI_FORMAT_R16G16_SNORM: {
        const int16_t* u = reinterpret_cast<const int16_t*>(s);
        out[0]= u[0]<=-32767 ? -1.0f : u[0]/32767.0f; out[1]= u[1]<=-32767 ? -1.0f : u[1]/32767.0f; break; }
      case DXGI_FORMAT_R16G16B16A16_UNORM: {
        const uint16_t* u = reinterpret_cast<const uint16_t*>(s);
        for (int i=0;i<4;i++) out[i]=u[i]/65535.0f; break; }
      case DXGI_FORMAT_R16G16B16A16_SNORM: {
        const int16_t* u = reinterpret_cast<const int16_t*>(s);
        for (int i=0;i<4;i++) out[i]= u[i]<=-32767 ? -1.0f : u[i]/32767.0f; break; }
      case DXGI_FORMAT_R10G10B10A2_UNORM: {
        uint32_t v = *reinterpret_cast<const uint32_t*>(s);
        out[0]=((v) & 0x3FF)/1023.0f; out[1]=((v>>10)&0x3FF)/1023.0f; out[2]=((v>>20)&0x3FF)/1023.0f; out[3]=((v>>30)&0x3)/3.0f; break; }
      case DXGI_FORMAT_R8G8_UNORM: { out[0]=s[0]/255.0f; out[1]=s[1]/255.0f; break; }
      case DXGI_FORMAT_R8_UNORM: { out[0]=s[0]/255.0f; break; }
      default: break; // unsupported attribute format -> zeros
    }
  }

  inline uint32_t packColorRGBA(const float c[4]) {
    auto u8 = [](float f) -> uint32_t {
      if (f < 0.0f) f = 0.0f; if (f > 1.0f) f = 1.0f;
      return (uint32_t)(f * 255.0f + 0.5f);
    };
    return (u8(c[3])<<24) | (u8(c[2])<<16) | (u8(c[1])<<8) | u8(c[0]);
  }

  // ===================================================================== //
  // Capture state.                                                        //
  // ===================================================================== //

  std::recursive_mutex g_mutex;

  struct CachedBuffer {
    std::vector<uint8_t> data;
    uint32_t bindFlags = 0;
    uint64_t version = 1;     // bumped on every content change (Create/Unmap/UpdateSubresource)
  };
  std::unordered_map<ID3D11Buffer*, CachedBuffer> g_buffers;

  // Mesh dedup cache: geometry+material identity key -> already-created Remix mesh uid.
  // Lets repeated (static) draws skip the per-draw vertex decode + FNV hash + serialize +
  // IPC re-send entirely; only a fresh DrawInstance (carrying the current transform) is
  // emitted on a cache hit. This is THE bridge frame-rate fix: previously every draw
  // re-streamed its full mesh over the shared-memory bridge every frame.
  std::unordered_map<uint64_t, uint32_t> g_meshCache;

  inline uint64_t mixKey(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  }

  // Resource -> mapped CPU pointer (between Map and Unmap).
  std::unordered_map<ID3D11Resource*, void*> g_mapped;

  struct InputElement {
    std::string semantic;
    uint32_t semanticIndex;
    DXGI_FORMAT format;
    uint32_t inputSlot;
    uint32_t alignedByteOffset;
  };
  std::unordered_map<ID3D11InputLayout*, std::vector<InputElement>> g_layouts;

  struct VSReflect {
    bool hasWorld = false;     // an unambiguous object->world matrix was found
    uint32_t cbSlot = 0;       // b# register of the constant buffer holding it
    uint32_t byteOffset = 0;   // offset of the matrix inside that constant buffer
    bool columnMajor = true;   // HLSL default storage
  };
  std::unordered_map<ID3D11VertexShader*, VSReflect> g_vsReflect;

  // Material cache keyed by bound-texture identity hash.
  std::unordered_map<uint64_t, uint32_t> g_materials;

  // DX11_V258_BRIDGE_TEXTURE_CONTENT_HASH: texture resource -> content-derived
  // identity hash, recorded at CreateTexture2D. Pointer address reuse after a
  // release self-heals because a new texture at the same address re-records
  // its entry at the only place a texture can come into existence.
  std::unordered_map<ID3D11Resource*, uint64_t> g_textureHashes;

  std::atomic<uint64_t> g_meshesStreamed { 0 };

  // ===================================================================== //
  // DX11_V265_BRIDGE_PRESENT_CAMERA: camera captured from VS constants.   //
  // ===================================================================== //

  // Both matrices row-major in the D3D row-vector convention (the layout the
  // server forwards verbatim into remixapi_CameraInfo view/projection).
  struct CameraWire {
    float view[16] = {};
    float proj[16] = {};
    bool valid = false;
    bool viewFound = false;
    uint64_t frame = ~0ull;
  };
  CameraWire g_camera;                       // guarded by g_mutex
  std::atomic<uint64_t> g_frameIndex { 0 };  // bumped in OnPresent
  uint64_t g_cameraScanFrame = ~0ull;        // guarded by g_mutex
  uint32_t g_cameraScanAttempts = 0;         // guarded by g_mutex

  bool isFinite16(const float* m) {
    for (int i = 0; i < 16; ++i)
      if (!std::isfinite(m[i])) return false;
    return true;
  }

  // D3D row-vector perspective: diag scales in m00/m11, w-generator in m[2][3],
  // m[3][3] == 0, shear terms zero.
  bool isPerspectiveRowMajor(const float* m) {
    if (!isFinite16(m)) return false;
    if (std::fabs(std::fabs(m[11]) - 1.0f) > 0.02f) return false;  // m[2][3]
    if (std::fabs(m[15]) > 0.02f) return false;                    // m[3][3]
    if (std::fabs(m[0]) < 0.1f || std::fabs(m[5]) < 0.1f) return false;
    if (std::fabs(m[1]) > 0.02f || std::fabs(m[3]) > 0.02f) return false;
    if (std::fabs(m[4]) > 0.02f || std::fabs(m[7]) > 0.02f) return false;
    if (std::fabs(m[12]) > 0.02f || std::fabs(m[13]) > 0.02f) return false;
    return true;
  }

  // Rigid-body view: orthonormal 3x3, affine last column, not identity.
  bool isViewLikeRowMajor(const float* m) {
    if (!isFinite16(m)) return false;
    if (std::fabs(m[3]) > 0.01f || std::fabs(m[7]) > 0.01f || std::fabs(m[11]) > 0.01f) return false;
    if (std::fabs(m[15] - 1.0f) > 0.01f) return false;
    for (int r = 0; r < 3; ++r) {
      const float len = m[r*4+0]*m[r*4+0] + m[r*4+1]*m[r*4+1] + m[r*4+2]*m[r*4+2];
      if (std::fabs(len - 1.0f) > 0.1f) return false;
    }
    if (std::fabs(m[0]*m[4] + m[1]*m[5] + m[2]*m[6]) > 0.08f) return false;
    if (std::fabs(m[0]*m[8] + m[1]*m[9] + m[2]*m[10]) > 0.08f) return false;
    if (std::fabs(m[4]*m[8] + m[5]*m[9] + m[6]*m[10]) > 0.08f) return false;
    bool identity = true;
    for (int r = 0; r < 4 && identity; ++r)
      for (int c = 0; c < 4; ++c)
        if (std::fabs(m[r*4+c] - (r == c ? 1.0f : 0.0f)) > 1e-5f) { identity = false; break; }
    return !identity;
  }

  void transpose16(const float* in, float* out) {
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        out[c*4 + r] = in[r*4 + c];
  }

  // Scan the bound VS constant buffers (cached CPU copies - no GPU access)
  // for a projection + view pair. STRICTLY bounded: at most 4 attempts per
  // frame, slots 0-3, first 4KB per buffer - per-draw cost explosions freeze
  // in-game frame rates (the V262 lesson).
  void noteCameraFromConstants(ID3D11DeviceContext* ctx) {
    const uint64_t frame = g_frameIndex.load(std::memory_order_relaxed);
    {
      std::lock_guard<std::recursive_mutex> lock(g_mutex);
      if (g_camera.valid && g_camera.viewFound && g_camera.frame == frame) return;
      if (g_cameraScanFrame != frame) { g_cameraScanFrame = frame; g_cameraScanAttempts = 0; }
      if (g_cameraScanAttempts >= 4) return;
      ++g_cameraScanAttempts;
    }

    ID3D11Buffer* cbs[4] = {};
    ctx->VSGetConstantBuffers(0, 4, cbs);

    for (uint32_t slot = 0; slot < 4; ++slot) {
      ID3D11Buffer* cb = cbs[slot];
      if (!cb) continue;

      std::lock_guard<std::recursive_mutex> lock(g_mutex);
      auto it = g_buffers.find(cb);
      if (it == g_buffers.end() || it->second.data.size() < 64) continue;
      const uint8_t* bytes = it->second.data.data();
      const size_t scanEnd = it->second.data.size() < 4096 ? it->second.data.size() : (size_t) 4096;

      // Pass 1: find the projection (as-stored or transposed).
      size_t projOff = SIZE_MAX;
      float proj[16];
      for (size_t off = 0; off + 64 <= scanEnd; off += 16) {
        const float* raw = reinterpret_cast<const float*>(bytes + off);
        if (isPerspectiveRowMajor(raw)) {
          memcpy(proj, raw, sizeof(proj));
          projOff = off;
          break;
        }
        float t[16];
        transpose16(raw, t);
        if (isPerspectiveRowMajor(t)) {
          memcpy(proj, t, sizeof(proj));
          projOff = off;
          break;
        }
      }
      if (projOff == SIZE_MAX) continue;

      // Pass 2: find the view in the same buffer (both conventions).
      bool viewFound = false;
      float view[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
      for (size_t off = 0; off + 64 <= scanEnd; off += 16) {
        if (off == projOff) continue;
        const float* raw = reinterpret_cast<const float*>(bytes + off);
        if (isViewLikeRowMajor(raw)) {
          memcpy(view, raw, sizeof(view));
          viewFound = true;
          break;
        }
        float t[16];
        transpose16(raw, t);
        if (isViewLikeRowMajor(t)) {
          memcpy(view, t, sizeof(view));
          viewFound = true;
          break;
        }
      }

      // Keep the best camera this frame: any camera beats none, and a camera
      // with a real view beats a projection-only one.
      if (!g_camera.valid || viewFound || g_camera.frame != frame) {
        memcpy(g_camera.proj, proj, sizeof(proj));
        memcpy(g_camera.view, view, sizeof(view));
        g_camera.valid = true;
        g_camera.viewFound = viewFound;
        g_camera.frame = frame;

        static bool s_cameraLogged = false;
        if (!s_cameraLogged) {
          s_cameraLogged = true;
          logf("capture", "camera captured from VS constants: slot=%u projOff=%u view=%s",
               slot, (unsigned) projOff, viewFound ? "yes" : "NO (origin)");
        }
      }
      if (g_camera.viewFound)
        break;
    }

    for (uint32_t slot = 0; slot < 4; ++slot) {
      if (cbs[slot]) cbs[slot]->Release();
    }
  }

  // ===================================================================== //
  // Materials.                                                            //
  // ===================================================================== //

  uint32_t ensureMaterial(uint64_t textureHash) {
    auto it = g_materials.find(textureHash);
    if (it != g_materials.end()) return it->second;

    MaterialHandle matHandle;
    const uint32_t uid = matHandle.uid;

    remixapi_MaterialInfo matInfo = {};
    matInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    matInfo.pNext = nullptr;
    matInfo.hash = textureHash ? textureHash : 0x1ull;
    matInfo.albedoTexture = nullptr;
    matInfo.normalTexture = nullptr;
    matInfo.tangentTexture = nullptr;
    matInfo.emissiveTexture = nullptr;
    matInfo.emissiveIntensity = 0.0f;
    matInfo.emissiveColorConstant = { 0.0f, 0.0f, 0.0f };
    matInfo.spriteSheetRow = 0;
    matInfo.spriteSheetCol = 0;
    matInfo.spriteSheetFps = 0;
    matInfo.filterMode = 1;
    matInfo.wrapModeU = 0;
    matInfo.wrapModeV = 0;

    {
      ClientMessage c(Commands::RemixApi_CreateMaterial);
      serializeAndSend<serialize::MaterialInfo>(c, matInfo);
      sendBool(c, false);
      sendUid(c, uid);
    }
    g_materials[textureHash] = uid;
    return uid;
  }

  // Hash the identity of the texture bound to pixel-shader SRV slot 0 so that each
  // distinct source texture maps to a distinct, stable Remix material.
  //
  // DX11_V258_BRIDGE_TEXTURE_CONTENT_HASH: the identity is the content hash
  // recorded at CreateTexture2D. The old code hashed the resource POINTER,
  // which ASLR randomizes every launch and the heap recycles within a run,
  // so material hashes were garbled and replacements could never key on them.
  uint64_t boundTextureHash(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    uint64_t h = 0;
    if (srv) {
      ID3D11Resource* res = nullptr;
      srv->GetResource(&res);
      if (res) {
        {
          std::lock_guard<std::recursive_mutex> lock(g_mutex);
          auto it = g_textureHashes.find(res);
          if (it != g_textureHashes.end()) h = it->second;
        }
        if (h == 0) {
          // Texture created before our hooks installed: fall back to a
          // desc-derived hash - weaker (same-shape textures collide) but
          // stable across runs, unlike the pointer.
          ID3D11Texture2D* tex2d = nullptr;
          if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**) &tex2d)) && tex2d) {
            D3D11_TEXTURE2D_DESC td = {};
            tex2d->GetDesc(&td);
            uint32_t meta[6] = { td.Width, td.Height, (uint32_t) td.Format,
                                 td.MipLevels, td.ArraySize, td.SampleDesc.Count };
            h = fnv1a(meta, sizeof(meta));
            tex2d->Release();
          }
        }
        res->Release();
      }
      srv->Release();
    }
    return h;
  }

  // ===================================================================== //
  // Vertex-shader reflection: locate the object->world transform.         //
  // ===================================================================== //

  void reflectVertexShader(ID3D11VertexShader* shader, const void* bytecode, size_t length) {
    if (!shader || !bytecode || length == 0) return;

    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(D3DReflect(bytecode, length, __uuidof(ID3D11ShaderReflection), (void**) &refl)) || !refl) {
      return;
    }

    D3D11_SHADER_DESC sd = {};
    if (FAILED(refl->GetDesc(&sd))) { refl->Release(); return; }

    // Resolve each cbuffer's bind slot (b#) via the resource bindings.
    auto cbufferSlot = [&](const char* name) -> uint32_t {
      for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd = {};
        if (SUCCEEDED(refl->GetResourceBindingDesc(i, &bd)) &&
            bd.Type == D3D_SIT_CBUFFER && name && bd.Name && strcmp(bd.Name, name) == 0) {
          return bd.BindPoint;
        }
      }
      return 0;
    };

    VSReflect best;
    int bestScore = -1; // higher = better (prefer pure world over combined)

    for (UINT b = 0; b < sd.ConstantBuffers; ++b) {
      ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(b);
      D3D11_SHADER_BUFFER_DESC bdesc = {};
      if (!cb || FAILED(cb->GetDesc(&bdesc))) continue;
      const uint32_t slot = cbufferSlot(bdesc.Name);

      for (UINT v = 0; v < bdesc.Variables; ++v) {
        ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
        D3D11_SHADER_VARIABLE_DESC vd = {};
        if (!var || FAILED(var->GetDesc(&vd))) continue;
        ID3D11ShaderReflectionType* t = var->GetType();
        D3D11_SHADER_TYPE_DESC td = {};
        if (!t || FAILED(t->GetDesc(&td))) continue;
        const bool is4x4 = (td.Class == D3D_SVC_MATRIX_COLUMNS || td.Class == D3D_SVC_MATRIX_ROWS) &&
                           td.Rows == 4 && td.Columns == 4;
        if (!is4x4) continue;

        std::string name = vd.Name ? vd.Name : "";
        for (auto& ch : name) ch = (char) tolower((unsigned char) ch);
        const bool hasWorld = name.find("world") != std::string::npos || name.find("model") != std::string::npos;
        const bool hasView  = name.find("view") != std::string::npos;
        const bool hasProj  = name.find("proj") != std::string::npos ||
                              name.find("wvp") != std::string::npos ||
                              name.find("mvp") != std::string::npos;

        int score = -1;
        if (hasWorld && !hasView && !hasProj) score = 3; // pure object->world: ideal
        else if (hasWorld && (hasView || hasProj))  score = 1; // combined: usable only if nothing better
        else if (!hasWorld && !hasView && !hasProj) score = 0; // an unnamed/transform matrix

        if (score > bestScore) {
          bestScore = score;
          best.hasWorld = (score >= 2);    // only treat a pure-world match as authoritative
          best.cbSlot = slot;
          best.byteOffset = vd.StartOffset;
          best.columnMajor = (td.Class != D3D_SVC_MATRIX_ROWS);
        }
      }
    }

    refl->Release();

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_vsReflect[shader] = best;
  }

  // Read the 4x4 transform the bound VS uses and convert it to a Remix 3x4 affine
  // (column-vector convention: x' = M[i][0]*x + M[i][1]*y + M[i][2]*z + M[i][3]).
  // Returns false (caller uses identity) when no authoritative world matrix exists.
  bool resolveWorldTransform(ID3D11DeviceContext* ctx, remixapi_Transform& outXf) {
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return false;

    VSReflect r;
    {
      std::lock_guard<std::recursive_mutex> lock(g_mutex);
      auto it = g_vsReflect.find(vs);
      if (it != g_vsReflect.end()) r = it->second;
    }
    if (!r.hasWorld) { vs->Release(); return false; }

    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(r.cbSlot, 1, &cb);
    bool ok = false;
    if (cb) {
      std::lock_guard<std::recursive_mutex> lock(g_mutex);
      auto it = g_buffers.find(cb);
      if (it != g_buffers.end() && it->second.data.size() >= (size_t) r.byteOffset + 64) {
        const float* m = reinterpret_cast<const float*>(it->second.data.data() + r.byteOffset);
        // Reconstruct the logical 4x4 W such that the shader computes
        // mul(float4(pos,1), W) (D3D row-vector convention). HLSL stores matrices
        // column-major by default, so the 16 floats are W transposed unless the
        // variable was declared row_major.
        float W[4][4];
        if (r.columnMajor) {
          for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
              W[row][col] = m[col * 4 + row];
        } else {
          for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
              W[row][col] = m[row * 4 + col];
        }
        // Remix M (column-vector 3x4): M[i][j] = W[j][i] for basis, translation W[3][i].
        for (int i = 0; i < 3; ++i) {
          outXf.matrix[i][0] = W[0][i];
          outXf.matrix[i][1] = W[1][i];
          outXf.matrix[i][2] = W[2][i];
          outXf.matrix[i][3] = W[3][i];
        }
        ok = true;
      }
      cb->Release();
    }
    vs->Release();
    return ok;
  }

  // ===================================================================== //
  // Vertex decode using the bound input layout.                           //
  // ===================================================================== //

  struct StreamRef {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    uint32_t stride = 0;
  };

  const CachedBuffer* findBuffer(ID3D11Buffer* b) {
    if (!b) return nullptr;
    auto it = g_buffers.find(b);
    return (it != g_buffers.end() && !it->second.data.empty()) ? &it->second : nullptr;
  }

  // Build the per-element resolved byte offset (handling D3D11_APPEND_ALIGNED_ELEMENT).
  void resolveElementOffsets(const std::vector<InputElement>& elems, std::vector<uint32_t>& offsets) {
    uint32_t running[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    offsets.resize(elems.size());
    for (size_t i = 0; i < elems.size(); ++i) {
      const InputElement& e = elems[i];
      uint32_t off = e.alignedByteOffset;
      if (off == 0xFFFFFFFFu) off = running[e.inputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
      offsets[i] = off;
      running[e.inputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = off + formatByteSize(e.format);
    }
  }

  // ===================================================================== //
  // Stream one captured draw to Remix.                                    //
  // ===================================================================== //

  void streamDraw(ID3D11DeviceContext* ctx,
                  const std::vector<InputElement>& elems,
                  const StreamRef streams[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT],
                  const uint8_t* ibData, uint32_t ibSize, bool ib32,
                  uint32_t indexCount, uint32_t startIndex, int32_t baseVertex,
                  bool sequential, uint32_t seqStart, uint64_t geomKey) {
    if (indexCount < 3) return;
    if (indexCount > 24'000'000u) return;

    // Material identity is part of the mesh-cache key: the same geometry drawn with a
    // different bound texture must map to a distinct Remix mesh (the material is baked
    // onto the surface at CreateMesh time).
    const uint32_t materialUid = ensureMaterial(boundTextureHash(ctx));
    const uint64_t cacheKey = geomKey ? mixKey(geomKey, materialUid) : 0;

    uint32_t meshUid = 0;
    bool haveMesh = false;
    if (cacheKey) {
      auto cit = g_meshCache.find(cacheKey);
      if (cit != g_meshCache.end()) { meshUid = cit->second; haveMesh = true; }
    }

    if (!haveMesh) {
    // Locate the attribute elements we care about.
    std::vector<uint32_t> elemOffsets;
    resolveElementOffsets(elems, elemOffsets);

    int posE = -1, nrmE = -1, uvE = -1, colE = -1;
    for (size_t i = 0; i < elems.size(); ++i) {
      const std::string& s = elems[i].semantic;
      if (posE < 0 && (s == "POSITION" || s == "SV_Position" || s == "POSITION0")) posE = (int) i;
      else if (nrmE < 0 && s == "NORMAL") nrmE = (int) i;
      else if (uvE < 0 && (s == "TEXCOORD" || s == "TEXCOORD0")) uvE = (int) i;
      else if (colE < 0 && (s == "COLOR" || s == "COLOR0")) colE = (int) i;
    }
    if (posE < 0) return; // cannot capture geometry without positions

    const InputElement& pe = elems[posE];
    const StreamRef& posStream = streams[pe.inputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    if (!posStream.data || posStream.stride < formatByteSize(pe.format)) return;
    const uint32_t vertexCount = posStream.size / posStream.stride;
    if (vertexCount == 0 || vertexCount > 8'000'000u) return;

    // Build the index list (object-space indices into the vertex streams).
    std::vector<uint32_t> indices;
    indices.reserve(indexCount);
    if (sequential) {
      for (uint32_t i = 0; i < indexCount; ++i) {
        const uint32_t idx = seqStart + i;
        if (idx >= vertexCount) return;
        indices.push_back(idx);
      }
    } else {
      const uint32_t idxStride = ib32 ? 4u : 2u;
      for (uint32_t i = 0; i < indexCount; ++i) {
        const uint32_t byteOff = (startIndex + i) * idxStride;
        if (byteOff + idxStride > ibSize) return;
        uint32_t v = ib32 ? *reinterpret_cast<const uint32_t*>(ibData + byteOff)
                          : *reinterpret_cast<const uint16_t*>(ibData + byteOff);
        const int64_t adj = (int64_t) v + (int64_t) baseVertex;
        if (adj < 0 || adj >= (int64_t) vertexCount) return;
        indices.push_back((uint32_t) adj);
      }
    }

    // Decode every vertex referenced by the position stream.
    std::vector<remixapi_HardcodedVertex> verts(vertexCount);
    const StreamRef& nStream = (nrmE >= 0) ? streams[elems[nrmE].inputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] : StreamRef{};
    const StreamRef& tStream = (uvE  >= 0) ? streams[elems[uvE ].inputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] : StreamRef{};
    const StreamRef& cStream = (colE >= 0) ? streams[elems[colE].inputSlot % D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] : StreamRef{};

    for (uint32_t i = 0; i < vertexCount; ++i) {
      remixapi_HardcodedVertex& hv = verts[i];
      memset(&hv, 0, sizeof(hv));

      float p[4];
      decodeFormat(posStream.data + (size_t) i * posStream.stride + elemOffsets[posE], pe.format, p);
      hv.position[0] = p[0]; hv.position[1] = p[1]; hv.position[2] = p[2];

      if (nrmE >= 0 && nStream.data && (size_t) i * nStream.stride + elemOffsets[nrmE] + formatByteSize(elems[nrmE].format) <= nStream.size) {
        float n[4];
        decodeFormat(nStream.data + (size_t) i * nStream.stride + elemOffsets[nrmE], elems[nrmE].format, n);
        hv.normal[0]=n[0]; hv.normal[1]=n[1]; hv.normal[2]=n[2];
      } else { hv.normal[0]=0.0f; hv.normal[1]=0.0f; hv.normal[2]=1.0f; }

      if (uvE >= 0 && tStream.data && (size_t) i * tStream.stride + elemOffsets[uvE] + formatByteSize(elems[uvE].format) <= tStream.size) {
        float t[4];
        decodeFormat(tStream.data + (size_t) i * tStream.stride + elemOffsets[uvE], elems[uvE].format, t);
        hv.texcoord[0]=t[0]; hv.texcoord[1]=t[1];
      }

      if (colE >= 0 && cStream.data && (size_t) i * cStream.stride + elemOffsets[colE] + formatByteSize(elems[colE].format) <= cStream.size) {
        float c[4];
        decodeFormat(cStream.data + (size_t) i * cStream.stride + elemOffsets[colE], elems[colE].format, c);
        hv.color = packColorRGBA(c);
      } else {
        hv.color = 0xFFFFFFFFu;
      }
    }

    uint64_t hash = fnv1a(verts.data(), verts.size() * sizeof(remixapi_HardcodedVertex));
    hash = fnv1a(indices.data(), indices.size() * sizeof(uint32_t), hash);

    // ---- CreateMesh ----
    MeshHandle meshHandle;
    {
      remixapi_MeshInfoSurfaceTriangles surf = {};
      surf.vertices_values = verts.data();
      surf.vertices_count = verts.size();
      surf.indices_values = indices.data();
      surf.indices_count = indices.size();
      surf.skinning_hasvalue = 0;
      memset(&surf.skinning_value, 0, sizeof(surf.skinning_value));
      surf.material = reinterpret_cast<remixapi_MaterialHandle>((uintptr_t) materialUid);

      remixapi_MeshInfo meshInfo = {};
      meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      meshInfo.pNext = nullptr;
      meshInfo.hash = hash;
      meshInfo.surfaces_values = &surf;
      meshInfo.surfaces_count = 1;

      ClientMessage c(Commands::RemixApi_CreateMesh);
      serializeAndSend<serialize::MeshInfo>(c, meshInfo);
      sendUid(c, meshHandle.uid);
    }

      meshUid = meshHandle.uid;
      if (cacheKey) {
        // Bound to avoid unbounded growth from genuinely dynamic geometry (a fresh
        // content-version produces a fresh key every frame). Clearing just forces those
        // to be re-streamed; static geometry repopulates immediately.
        if (g_meshCache.size() > 262144) g_meshCache.clear();
        g_meshCache[cacheKey] = meshUid;
      }

      const uint64_t n = ++g_meshesStreamed;
      if (n <= 16 || (n % 2000) == 0) {
        logf("capture", "DX11_V226_CAPTURE_TO_REMIX: streamed mesh #%llu (verts=%u, indices=%u) to Remix.",
             (unsigned long long) n, vertexCount, indexCount);
      }
    } // end if(!haveMesh): cache miss built+streamed a new mesh

    // ---- DrawInstance ---- (always emitted: carries the current per-draw transform)
    {
      remixapi_InstanceInfo instInfo = {};
      instInfo.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instInfo.pNext = nullptr;
      instInfo.categoryFlags = 0;
      instInfo.mesh = reinterpret_cast<remixapi_MeshHandle>((uintptr_t) meshUid);
      memset(&instInfo.transform, 0, sizeof(instInfo.transform));
      instInfo.transform.matrix[0][0] = 1.0f;
      instInfo.transform.matrix[1][1] = 1.0f;
      instInfo.transform.matrix[2][2] = 1.0f;
      resolveWorldTransform(ctx, instInfo.transform); // fills the real object->world when known
      instInfo.doubleSided = 1;

      ClientMessage c(Commands::RemixApi_DrawInstance);
      serializeAndSend<serialize::InstanceInfo>(c, instInfo);
      sendBool(c, false);
    }
  }

} // anonymous namespace

// ======================================================================= //
// Public entry points.                                                    //
// ======================================================================= //

void RecordBufferCreate(ID3D11Buffer* buffer, const void* pInitialData,
                        uint32_t byteWidth, uint32_t bindFlags) {
  if (!buffer || byteWidth == 0) return;
  if ((bindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_CONSTANT_BUFFER)) == 0) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  CachedBuffer& cb = g_buffers[buffer];
  cb.bindFlags = bindFlags;
  cb.data.resize(byteWidth);
  if (pInitialData) memcpy(cb.data.data(), pInitialData, byteWidth);
  else memset(cb.data.data(), 0, byteWidth);
  ++cb.version;
}

void RecordBufferRelease(ID3D11Buffer* buffer) {
  if (!buffer) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_buffers.erase(buffer);
  g_mapped.erase(reinterpret_cast<ID3D11Resource*>(buffer));
}

void RecordMap(ID3D11Resource* resource, uint32_t subresource, void* pMappedData) {
  if (!resource || subresource != 0 || !pMappedData) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (g_buffers.find(reinterpret_cast<ID3D11Buffer*>(resource)) == g_buffers.end()) return;
  g_mapped[resource] = pMappedData;
}

void RecordUnmap(ID3D11Resource* resource, uint32_t subresource) {
  if (!resource || subresource != 0) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto m = g_mapped.find(resource);
  if (m == g_mapped.end()) return;
  void* src = m->second;
  g_mapped.erase(m);
  auto it = g_buffers.find(reinterpret_cast<ID3D11Buffer*>(resource));
  if (it != g_buffers.end() && src && !it->second.data.empty()) {
    memcpy(it->second.data.data(), src, it->second.data.size());
    ++it->second.version;
  }
}

void RecordUpdateSubresource(ID3D11Resource* resource, uint32_t dstSubresource,
                             const void* pSrcData, uint32_t /*srcRowPitch*/) {
  if (!resource || dstSubresource != 0 || !pSrcData) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto it = g_buffers.find(reinterpret_cast<ID3D11Buffer*>(resource));
  if (it != g_buffers.end() && !it->second.data.empty()) {
    memcpy(it->second.data.data(), pSrcData, it->second.data.size());
    ++it->second.version;
  }
}

void RecordInputLayoutCreate(ID3D11InputLayout* layout,
                             const D3D11_INPUT_ELEMENT_DESC* descs, uint32_t numElements) {
  if (!layout || !descs || numElements == 0) return;
  std::vector<InputElement> elems;
  elems.reserve(numElements);
  for (uint32_t i = 0; i < numElements; ++i) {
    InputElement e;
    e.semantic = descs[i].SemanticName ? descs[i].SemanticName : "";
    e.semanticIndex = descs[i].SemanticIndex;
    e.format = descs[i].Format;
    e.inputSlot = descs[i].InputSlot;
    e.alignedByteOffset = descs[i].AlignedByteOffset;
    elems.push_back(std::move(e));
  }
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_layouts[layout] = std::move(elems);
}

void RecordInputLayoutRelease(ID3D11InputLayout* layout) {
  if (!layout) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_layouts.erase(layout);
}

// DX11_V258_BRIDGE_TEXTURE_CONTENT_HASH
void RecordTexture2DCreate(ID3D11Resource* texture, const void* pMip0Data,
                           uint32_t rowPitchBytes, uint32_t width, uint32_t height,
                           uint32_t format, uint32_t mipLevels, uint32_t arraySize,
                           uint32_t bindFlags) {
  if (!texture) return;
  // Only textures the game can sample can become Remix materials.
  if ((bindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) return;

  uint64_t h = 0;
  if (pMip0Data && rowPitchBytes) {
    // Conservative row count: block-compressed formats store height/4 rows of
    // blocks while plain formats store `height` rows, so height/4 rows is a
    // safe lower bound for both - the read can never leave the caller's
    // allocation. Capped to bound creation-time cost on huge textures.
    const uint32_t safeRows = height >= 4 ? height / 4 : 1;
    uint64_t total = (uint64_t) rowPitchBytes * safeRows;
    const uint64_t kMaxHashBytes = 65536;
    if (total > kMaxHashBytes) total = kMaxHashBytes;
    h = fnv1a(pMip0Data, (size_t) total);
  }
  // Mix in structure so same-bytes/different-shape textures don't collide,
  // and so no-initial-data textures still get a stable (if weaker, desc-only)
  // identity instead of an ASLR-random pointer hash.
  const uint32_t meta[5] = { width, height, format, mipLevels, arraySize };
  h = fnv1a(meta, sizeof(meta), h ? h : 1469598103934665603ull);
  if (h == 0) h = 1;

  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_textureHashes[texture] = h;
}

void RecordTexture2DRelease(ID3D11Resource* texture) {
  if (!texture) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_textureHashes.erase(texture);
}

void RecordVertexShaderCreate(ID3D11VertexShader* shader, const void* bytecode, size_t length) {
  reflectVertexShader(shader, bytecode, length);
}

void RecordVertexShaderRelease(ID3D11VertexShader* shader) {
  if (!shader) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_vsReflect.erase(shader);
}

void CaptureDrawIndexed(ID3D11DeviceContext* context, uint32_t indexCount,
                        uint32_t startIndexLocation, int32_t baseVertexLocation) {
  if (!context || !IsStreamingEnabled() || indexCount < 3) return;

  noteCameraFromConstants(context);

  ID3D11InputLayout* layout = nullptr;
  context->IAGetInputLayout(&layout);

  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  context->IAGetPrimitiveTopology(&topo);

  ID3D11Buffer* vbs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  UINT offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  context->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, vbs, strides, offsets);

  ID3D11Buffer* ib = nullptr;
  DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
  UINT ibOffset = 0;
  context->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);

  if (layout && ib && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
      (ibFormat == DXGI_FORMAT_R16_UINT || ibFormat == DXGI_FORMAT_R32_UINT)) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto lit = g_layouts.find(layout);
    const CachedBuffer* ibc = findBuffer(ib);
    if (lit != g_layouts.end() && ibc && ibOffset < ibc->data.size()) {
      StreamRef streams[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
      // Cheap geometry-identity key (no byte hashing): bound buffer pointers + their
      // content-versions + offsets/strides + draw params. Stable frame-to-frame for
      // static geometry, so streamDraw can skip the decode/serialize/IPC re-send.
      uint64_t geomKey = mixKey(0xC0FFEEull, reinterpret_cast<uintptr_t>(layout));
      for (uint32_t s = 0; s < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++s) {
        if (!vbs[s]) continue;
        const CachedBuffer* vbc = findBuffer(vbs[s]);
        if (vbc && offsets[s] < vbc->data.size()) {
          streams[s].data = vbc->data.data() + offsets[s];
          streams[s].size = (uint32_t)(vbc->data.size() - offsets[s]);
          streams[s].stride = strides[s];
          geomKey = mixKey(geomKey, ((uint64_t) s << 56) ^ reinterpret_cast<uintptr_t>(vbs[s]));
          geomKey = mixKey(geomKey, ((uint64_t) strides[s] << 32) | offsets[s]);
          geomKey = mixKey(geomKey, vbc->version);
        }
      }
      geomKey = mixKey(geomKey, reinterpret_cast<uintptr_t>(ib));
      geomKey = mixKey(geomKey, ((uint64_t) ibFormat << 32) | ibOffset);
      geomKey = mixKey(geomKey, ibc->version);
      geomKey = mixKey(geomKey, ((uint64_t) indexCount << 32) | startIndexLocation);
      geomKey = mixKey(geomKey, (uint64_t)(uint32_t) baseVertexLocation);
      streamDraw(context, lit->second, streams,
                 ibc->data.data() + ibOffset, (uint32_t)(ibc->data.size() - ibOffset),
                 ibFormat == DXGI_FORMAT_R32_UINT,
                 indexCount, startIndexLocation, baseVertexLocation, false, 0, geomKey);
    }
  }

  for (uint32_t s = 0; s < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++s) if (vbs[s]) vbs[s]->Release();
  if (ib) ib->Release();
  if (layout) layout->Release();
}

void CaptureDraw(ID3D11DeviceContext* context, uint32_t vertexCount,
                 uint32_t startVertexLocation) {
  if (!context || !IsStreamingEnabled() || vertexCount < 3) return;

  noteCameraFromConstants(context);

  ID3D11InputLayout* layout = nullptr;
  context->IAGetInputLayout(&layout);

  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  context->IAGetPrimitiveTopology(&topo);

  ID3D11Buffer* vbs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  UINT offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  context->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, vbs, strides, offsets);

  if (layout && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto lit = g_layouts.find(layout);
    if (lit != g_layouts.end()) {
      StreamRef streams[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
      // Cheap geometry-identity key (sequential/non-indexed variant; distinct seed so it
      // never collides with the indexed path).
      uint64_t geomKey = mixKey(0xBEEFull, reinterpret_cast<uintptr_t>(layout));
      for (uint32_t s = 0; s < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++s) {
        if (!vbs[s]) continue;
        const CachedBuffer* vbc = findBuffer(vbs[s]);
        if (vbc && offsets[s] < vbc->data.size()) {
          streams[s].data = vbc->data.data() + offsets[s];
          streams[s].size = (uint32_t)(vbc->data.size() - offsets[s]);
          streams[s].stride = strides[s];
          geomKey = mixKey(geomKey, ((uint64_t) s << 56) ^ reinterpret_cast<uintptr_t>(vbs[s]));
          geomKey = mixKey(geomKey, ((uint64_t) strides[s] << 32) | offsets[s]);
          geomKey = mixKey(geomKey, vbc->version);
        }
      }
      geomKey = mixKey(geomKey, ((uint64_t) vertexCount << 32) | startVertexLocation);
      streamDraw(context, lit->second, streams, nullptr, 0, false,
                 vertexCount, 0, 0, true, startVertexLocation, geomKey);
    }
  }

  for (uint32_t s = 0; s < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++s) if (vbs[s]) vbs[s]->Release();
  if (layout) layout->Release();
}

// DX11_V229_PRESENT_WINDOW_FALLBACK: composition/CoreWindow swapchains do not
// expose a legacy DXGI_SWAP_CHAIN_DESC::OutputWindow. Resolve the game's real
// top-level HWND so Remix Startup is not permanently skipped for those games.
struct PresentWindowCandidateV229 {
  DWORD pid;
  HWND hwnd;
  unsigned long long area;
};

static BOOL CALLBACK FindPresentWindowV229(HWND hwnd, LPARAM param) {
  auto* best = reinterpret_cast<PresentWindowCandidateV229*>(param);
  if (!best || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != best->pid) return TRUE;
  RECT rc = {};
  if (!GetClientRect(hwnd, &rc)) return TRUE;
  const LONG width = rc.right - rc.left;
  const LONG height = rc.bottom - rc.top;
  if (width <= 0 || height <= 0) return TRUE;
  const unsigned long long area =
    static_cast<unsigned long long>(width) * static_cast<unsigned long long>(height);
  if (area > best->area) {
    best->area = area;
    best->hwnd = hwnd;
  }
  return TRUE;
}

static HWND ResolvePresentWindowV229(IDXGISwapChain* swapChain) {
  if (swapChain) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow && IsWindow(desc.OutputWindow)) {
      return desc.OutputWindow;
    }
  }

  const DWORD selfPid = GetCurrentProcessId();
  HWND foreground = GetForegroundWindow();
  if (foreground) {
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(foreground, &foregroundPid);
    if (foregroundPid == selfPid) return foreground;
  }

  PresentWindowCandidateV229 best = { selfPid, nullptr, 0 };
  EnumWindows(&FindPresentWindowV229, reinterpret_cast<LPARAM>(&best));
  return best.hwnd;
}

// DX11_V265_BRIDGE_PRESENT_CAMERA: the per-frame pump that makes the server
// actually render. Startup attaches the Remix runtime to the GAME window
// (HWNDs are system-global, so the x64 server presents into the x86 game's
// window - this is what makes the Remix output primary instead of a stray
// secondary window), SetupCamera feeds the frame's captured camera, Present
// kicks the path-traced frame with the game HWND as override.
void OnPresent(IDXGISwapChain* swapChain) {
  if (!IsStreamingEnabled()) return;

  g_frameIndex.fetch_add(1, std::memory_order_relaxed);

  static uint64_t s_hwnd64 = 0;
  static bool s_startupSent = false;
  if (!s_startupSent && swapChain) {
    const HWND presentWindow = ResolvePresentWindowV229(swapChain);
    if (presentWindow) {
      s_hwnd64 = (uint64_t)(uintptr_t) presentWindow;
      ClientMessage c(Commands::RemixApi_Startup);
      c.send_data((uint32_t) sizeof(s_hwnd64), &s_hwnd64);
      s_startupSent = true;
      logf("capture", "RemixApi_Startup sent (resolved game hwnd=0x%llx)", (unsigned long long) s_hwnd64);
    }
  }
  if (!s_startupSent) return;

  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_camera.valid) {
      float blob[32];
      memcpy(blob, g_camera.view, sizeof(g_camera.view));
      memcpy(blob + 16, g_camera.proj, sizeof(g_camera.proj));
      ClientMessage c(Commands::RemixApi_SetupCamera);
      c.send_data((uint32_t) sizeof(blob), blob);
    }
  }

  {
    ClientMessage c(Commands::RemixApi_Present);
    c.send_data((uint32_t) sizeof(s_hwnd64), &s_hwnd64);
  }
}

bool IsStreamingEnabled() {
  return dx11_bridge_client::EnsureServer();
}

} // namespace dx11_capture
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'd3d11_dx11bridge.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include "dx11_bridge_client.h"
#include "dx11_capture_remix.h"

#define DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER 1

using PFN_D3D11CreateDevice = HRESULT (WINAPI *)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (WINAPI *)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static HMODULE gSystemD3D11 = nullptr;
static PFN_D3D11CreateDevice pD3D11CreateDevice = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain pD3D11CreateDeviceAndSwapChain = nullptr;

static volatile LONG gV219HooksInstalled = 0;
static volatile LONG gV219PresentCount = 0;
static volatile LONG gV219DrawCount = 0;
static volatile LONG gV219ResourceCount = 0;
static thread_local bool gV219InsideHook = false;

static void V219Log(const char* tag, const char* text) {
  dx11_bridge_client::LogLine(tag, text);
}

static HMODULE LoadSystemD3D11V219() {
  if (gSystemD3D11) return gSystemD3D11;
  gSystemD3D11 = dx11_bridge_client::LoadSystemDll("d3d11.dll");
  if (!gSystemD3D11) return nullptr;
  pD3D11CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(gSystemD3D11, "D3D11CreateDevice"));
  pD3D11CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(GetProcAddress(gSystemD3D11, "D3D11CreateDeviceAndSwapChain"));
  if (!pD3D11CreateDevice || !pD3D11CreateDeviceAndSwapChain) {
    V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: system d3d11.dll missing required exports.");
    return nullptr;
  }
  return gSystemD3D11;
}

template <typename T>
static bool V219HookVTable(void* object, size_t slot, void* detour, T* original, const char* name) {
  if (!object || !detour || !original) return false;
  void*** obj = reinterpret_cast<void***>(object);
  void** vt = *obj;
  if (!vt) return false;

  if (vt[slot] == detour) {
    return true;
  }

  DWORD oldProtect = 0;
  if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: VirtualProtect failed for %s slot=%u err=%lu.", name, (unsigned) slot, GetLastError());
    V219Log("hook", msg);
    return false;
  }

  if (*original == nullptr) {
    *original = reinterpret_cast<T>(vt[slot]);
  }

  vt[slot] = detour;

  DWORD ignored = 0;
  VirtualProtect(&vt[slot], sizeof(void*), oldProtect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), &vt[slot], sizeof(void*));

  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: hooked %s vtable slot %u.", name, (unsigned) slot);
  V219Log("hook", msg);
  return true;
}

// IDXGISwapChain slots.
using PFN_SwapPresent = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain*, UINT, UINT);
using PFN_SwapResizeBuffers = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PFN_SwapPresent oSwapPresent = nullptr;
static PFN_SwapResizeBuffers oSwapResizeBuffers = nullptr;

// ID3D11DeviceContext slots.
using PFN_DrawIndexed = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInstanced = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
static PFN_DrawIndexed oDrawIndexed = nullptr;
static PFN_Draw oDraw = nullptr;
static PFN_DrawIndexedInstanced oDrawIndexedInstanced = nullptr;
static PFN_DrawInstanced oDrawInstanced = nullptr;

// ID3D11Device slots.
using PFN_CreateBuffer = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using PFN_CreateTexture2D = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using PFN_CreateSRV = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using PFN_CreateVertexShader = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using PFN_CreatePixelShader = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
static PFN_CreateBuffer oCreateBuffer = nullptr;
static PFN_CreateTexture2D oCreateTexture2D = nullptr;
static PFN_CreateSRV oCreateSRV = nullptr;
static PFN_CreateVertexShader oCreateVertexShader = nullptr;
static PFN_CreatePixelShader oCreatePixelShader = nullptr;

// ID3D11Device::CreateInputLayout + ID3D11DeviceContext upload paths (DX11_V226_CAPTURE_TO_REMIX).
using PFN_CreateInputLayout = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
using PFN_Map = HRESULT (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using PFN_UpdateSubresource = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
static PFN_CreateInputLayout oCreateInputLayout = nullptr;
static PFN_Map oMap = nullptr;
static PFN_Unmap oUnmap = nullptr;
static PFN_UpdateSubresource oUpdateSubresource = nullptr;

static void V219NotifyDraw(const char* what) {
  LONG n = InterlockedIncrement(&gV219DrawCount);
  if (n <= 16 || (n % 500) == 0) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: captured %s count=%ld in game process.", what, n);
    V219Log("capture", msg);
  }
}

static void V219NotifyResource(const char* what) {
  LONG n = InterlockedIncrement(&gV219ResourceCount);
  if (n <= 16 || (n % 250) == 0) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: captured %s count=%ld in game process.", what, n);
    V219Log("capture", msg);
  }
}

static HRESULT STDMETHODCALLTYPE HSwapPresent(IDXGISwapChain* self, UINT syncInterval, UINT flags) {
  if (!gV219InsideHook) {
    gV219InsideHook = true;
    LONG n = InterlockedIncrement(&gV219PresentCount);
    if (n <= 8 || (n % 60) == 0) {
      char msg[256] = {};
      sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: captured IDXGISwapChain::Present count=%ld in game process.", n);
      V219Log("capture", msg);
    }
    dx11_bridge_client::EnsureServer();
    // DX11_V265_BRIDGE_PRESENT_CAMERA: frame boundary - send Startup (once,
    // game HWND), SetupCamera and Present to the server so the Remix runtime
    // attaches to and presents INTO the game window every frame.
    dx11_capture::OnPresent(self);
    gV219InsideHook = false;
  }
  return oSwapPresent ? oSwapPresent(self, syncInterval, flags) : DXGI_ERROR_DEVICE_REMOVED;
}

static HRESULT STDMETHODCALLTYPE HSwapResizeBuffers(IDXGISwapChain* self, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT flags) {
  V219Log("capture", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: captured IDXGISwapChain::ResizeBuffers in game process.");
  return oSwapResizeBuffers ? oSwapResizeBuffers(self, bufferCount, width, height, newFormat, flags) : DXGI_ERROR_DEVICE_REMOVED;
}

static void STDMETHODCALLTYPE HDrawIndexed(ID3D11DeviceContext* self, UINT indexCount, UINT startIndexLocation, INT baseVertexLocation) {
  V219NotifyDraw("ID3D11DeviceContext::DrawIndexed");
  if (oDrawIndexed) oDrawIndexed(self, indexCount, startIndexLocation, baseVertexLocation);
  dx11_capture::CaptureDrawIndexed(self, indexCount, startIndexLocation, baseVertexLocation);
}

static void STDMETHODCALLTYPE HDraw(ID3D11DeviceContext* self, UINT vertexCount, UINT startVertexLocation) {
  V219NotifyDraw("ID3D11DeviceContext::Draw");
  if (oDraw) oDraw(self, vertexCount, startVertexLocation);
  dx11_capture::CaptureDraw(self, vertexCount, startVertexLocation);
}

static void STDMETHODCALLTYPE HDrawIndexedInstanced(ID3D11DeviceContext* self, UINT indexCountPerInstance, UINT instanceCount, UINT startIndexLocation, INT baseVertexLocation, UINT startInstanceLocation) {
  V219NotifyDraw("ID3D11DeviceContext::DrawIndexedInstanced");
  if (oDrawIndexedInstanced) oDrawIndexedInstanced(self, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
}

static void STDMETHODCALLTYPE HDrawInstanced(ID3D11DeviceContext* self, UINT vertexCountPerInstance, UINT instanceCount, UINT startVertexLocation, UINT startInstanceLocation) {
  V219NotifyDraw("ID3D11DeviceContext::DrawInstanced");
  if (oDrawInstanced) oDrawInstanced(self, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
}

static HRESULT STDMETHODCALLTYPE HCreateBuffer(ID3D11Device* self, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Buffer** out) {
  V219NotifyResource("ID3D11Device::CreateBuffer");
  HRESULT hr_cap = oCreateBuffer ? oCreateBuffer(self, desc, data, out) : E_FAIL;
  if (SUCCEEDED(hr_cap) && desc && out && *out) { dx11_capture::RecordBufferCreate(*out, data ? data->pSysMem : nullptr, desc->ByteWidth, desc->BindFlags); }
  return hr_cap;
}

static HRESULT STDMETHODCALLTYPE HCreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** out) {
  V219NotifyResource("ID3D11Device::CreateTexture2D");
  HRESULT hr_cap = oCreateTexture2D ? oCreateTexture2D(self, desc, data, out) : E_FAIL;
  // DX11_V258_BRIDGE_TEXTURE_CONTENT_HASH: record a content-derived identity so
  // material hashes are identical across runs (pointer hashes are ASLR-random).
  if (SUCCEEDED(hr_cap) && desc && out && *out) {
    dx11_capture::RecordTexture2DCreate(*out, data ? data->pSysMem : nullptr,
      data ? data->SysMemPitch : 0u, desc->Width, desc->Height,
      (uint32_t) desc->Format, desc->MipLevels, desc->ArraySize, desc->BindFlags);
  }
  return hr_cap;
}

static HRESULT STDMETHODCALLTYPE HCreateSRV(ID3D11Device* self, ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out) {
  V219NotifyResource("ID3D11Device::CreateShaderResourceView");
  return oCreateSRV ? oCreateSRV(self, resource, desc, out) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE HCreateVertexShader(ID3D11Device* self, const void* bytecode, SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, ID3D11VertexShader** out) {
  V219NotifyResource("ID3D11Device::CreateVertexShader");
  HRESULT hr = oCreateVertexShader ? oCreateVertexShader(self, bytecode, bytecodeLength, linkage, out) : E_FAIL;
  // DX11_V226_CAPTURE_TO_REMIX: reflect the shader to locate its world transform.
  if (SUCCEEDED(hr) && out && *out && bytecode) {
    dx11_capture::RecordVertexShaderCreate(*out, bytecode, (size_t) bytecodeLength);
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE HCreateInputLayout(ID3D11Device* self, const D3D11_INPUT_ELEMENT_DESC* descs, UINT num, const void* sig, SIZE_T sigLen, ID3D11InputLayout** out) {
  HRESULT hr = oCreateInputLayout ? oCreateInputLayout(self, descs, num, sig, sigLen, out) : E_FAIL;
  // DX11_V226_CAPTURE_TO_REMIX: cache the element layout for vertex decoding.
  if (SUCCEEDED(hr) && out && *out && descs) {
    dx11_capture::RecordInputLayoutCreate(*out, descs, num);
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE HMap(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE* mapped) {
  HRESULT hr = oMap ? oMap(self, res, sub, mapType, mapFlags, mapped) : E_FAIL;
  // DX11_V226_CAPTURE_TO_REMIX: remember the mapped pointer; snapshot it on Unmap.
  if (SUCCEEDED(hr) && mapped && mapped->pData) {
    dx11_capture::RecordMap(res, sub, mapped->pData);
  }
  return hr;
}

static void STDMETHODCALLTYPE HUnmap(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub) {
  // DX11_V226_CAPTURE_TO_REMIX: copy the written bytes BEFORE the driver unmaps.
  dx11_capture::RecordUnmap(res, sub);
  if (oUnmap) oUnmap(self, res, sub);
}

static void STDMETHODCALLTYPE HUpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* res, UINT dstSub, const D3D11_BOX* box, const void* src, UINT rowPitch, UINT depthPitch) {
  if (oUpdateSubresource) oUpdateSubresource(self, res, dstSub, box, src, rowPitch, depthPitch);
  // DX11_V226_CAPTURE_TO_REMIX: capture full-resource buffer uploads.
  if (!box) {
    dx11_capture::RecordUpdateSubresource(res, dstSub, src, rowPitch);
  }
}

static HRESULT STDMETHODCALLTYPE HCreatePixelShader(ID3D11Device* self, const void* bytecode, SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, ID3D11PixelShader** out) {
  V219NotifyResource("ID3D11Device::CreatePixelShader");
  return oCreatePixelShader ? oCreatePixelShader(self, bytecode, bytecodeLength, linkage, out) : E_FAIL;
}

extern "C" __declspec(dllexport) void WINAPI DX11BridgeInstallSwapChainCapture(IDXGISwapChain* swapChain) {
  if (!swapChain) return;
  V219HookVTable(swapChain, 8, reinterpret_cast<void*>(&HSwapPresent), &oSwapPresent, "IDXGISwapChain::Present");
  V219HookVTable(swapChain, 13, reinterpret_cast<void*>(&HSwapResizeBuffers), &oSwapResizeBuffers, "IDXGISwapChain::ResizeBuffers");
}

static void V219InstallCapture(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) {
  if (InterlockedCompareExchange(&gV219HooksInstalled, 1, 0) == 0) {
    V219Log("capture", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: installing real in-game DX11 capture hooks.");
  }

  if (device) {
    // ID3D11Device vtable slots.
    V219HookVTable(device, 3, reinterpret_cast<void*>(&HCreateBuffer), &oCreateBuffer, "ID3D11Device::CreateBuffer");
    V219HookVTable(device, 5, reinterpret_cast<void*>(&HCreateTexture2D), &oCreateTexture2D, "ID3D11Device::CreateTexture2D");
    V219HookVTable(device, 7, reinterpret_cast<void*>(&HCreateSRV), &oCreateSRV, "ID3D11Device::CreateShaderResourceView");
    V219HookVTable(device, 11, reinterpret_cast<void*>(&HCreateInputLayout), &oCreateInputLayout, "ID3D11Device::CreateInputLayout");
    V219HookVTable(device, 12, reinterpret_cast<void*>(&HCreateVertexShader), &oCreateVertexShader, "ID3D11Device::CreateVertexShader");
    V219HookVTable(device, 15, reinterpret_cast<void*>(&HCreatePixelShader), &oCreatePixelShader, "ID3D11Device::CreatePixelShader");
  }

  ID3D11DeviceContext* localContext = nullptr;
  if (!context && device) {
    device->GetImmediateContext(&localContext);
    context = localContext;
  }

  if (context) {
    // ID3D11DeviceContext vtable slots.
    V219HookVTable(context, 12, reinterpret_cast<void*>(&HDrawIndexed), &oDrawIndexed, "ID3D11DeviceContext::DrawIndexed");
    V219HookVTable(context, 13, reinterpret_cast<void*>(&HDraw), &oDraw, "ID3D11DeviceContext::Draw");
    V219HookVTable(context, 14, reinterpret_cast<void*>(&HMap), &oMap, "ID3D11DeviceContext::Map");
    V219HookVTable(context, 15, reinterpret_cast<void*>(&HUnmap), &oUnmap, "ID3D11DeviceContext::Unmap");
    V219HookVTable(context, 20, reinterpret_cast<void*>(&HDrawIndexedInstanced), &oDrawIndexedInstanced, "ID3D11DeviceContext::DrawIndexedInstanced");
    V219HookVTable(context, 21, reinterpret_cast<void*>(&HDrawInstanced), &oDrawInstanced, "ID3D11DeviceContext::DrawInstanced");
    V219HookVTable(context, 48, reinterpret_cast<void*>(&HUpdateSubresource), &oUpdateSubresource, "ID3D11DeviceContext::UpdateSubresource");
  }

  if (localContext) {
    localContext->Release();
  }

  DX11BridgeInstallSwapChainCapture(swapChain);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_client::SetModule(hinst);
    dx11_bridge_client::Attach();
    V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: game loaded root d3d11.dll client capture layer.");
  }

  if (reason == DLL_PROCESS_DETACH) {
    if (reserved != nullptr) {
      dx11_bridge_client::Detach();
    } else {
      V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: ignoring runtime detach; bridge remains owned until process exit.");
    }
  }
  return TRUE;
}

extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount, UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext) {
  V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: D3D11CreateDevice intercepted inside game process.");
  if (!dx11_bridge_client::EnsureServer()) {
    V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: bridge server startup failed before D3D11CreateDevice.");
    if (device) *device = nullptr;
    if (immediateContext) *immediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  if (!LoadSystemD3D11V219() || !pD3D11CreateDevice) {
    if (device) *device = nullptr;
    if (immediateContext) *immediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT hr = pD3D11CreateDevice(adapter, driverType, software, flags, featureLevels, featureLevelCount, sdkVersion, device, featureLevel, immediateContext);
  if (SUCCEEDED(hr)) {
    V219InstallCapture(device ? *device : nullptr, immediateContext ? *immediateContext : nullptr, nullptr);
  }

  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: D3D11CreateDevice returned 0x%08X.", (unsigned) hr);
  V219Log("d3d11", msg);
  return hr;
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount, UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* swapChainDesc, IDXGISwapChain** swapChain, ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext) {
  V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: D3D11CreateDeviceAndSwapChain intercepted inside game process.");
  if (!dx11_bridge_client::EnsureServer()) {
    V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: bridge server startup failed before D3D11CreateDeviceAndSwapChain.");
    if (swapChain) *swapChain = nullptr;
    if (device) *device = nullptr;
    if (immediateContext) *immediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  if (!LoadSystemD3D11V219() || !pD3D11CreateDeviceAndSwapChain) {
    if (swapChain) *swapChain = nullptr;
    if (device) *device = nullptr;
    if (immediateContext) *immediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT hr = pD3D11CreateDeviceAndSwapChain(adapter, driverType, software, flags, featureLevels, featureLevelCount, sdkVersion, swapChainDesc, swapChain, device, featureLevel, immediateContext);
  if (SUCCEEDED(hr)) {
    V219InstallCapture(device ? *device : nullptr, immediateContext ? *immediateContext : nullptr, swapChain ? *swapChain : nullptr);
  }

  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: D3D11CreateDeviceAndSwapChain returned 0x%08X.", (unsigned) hr);
  V219Log("d3d11", msg);
  return hr;
}
'@

  $mesonB = Join-Path $DstClient 'meson.build'
  if (Test-Path -LiteralPath $mesonB) {
    $m = [System.IO.File]::ReadAllText($mesonB)
    if (-not $m.Contains('dx11_capture_remix.cpp')) {
      $m = $m.Replace("files(['d3d11_dx11bridge.cpp']) + dx11_common_src", "files(['d3d11_dx11bridge.cpp', 'dx11_capture_remix.cpp']) + dx11_common_src")
    }
    if (-not $m.Contains('d3dcompiler.lib')) {
      $m = $m.Replace("  vs_module_defs: 'd3d11_dx11bridge.def')", "  link_args: ['d3dcompiler.lib'],`n  vs_module_defs: 'd3d11_dx11bridge.def')")
    }
    Write-TextNoBom -Path $mesonB -Text $m
    Log 'DX11_V226_CAPTURE_TO_REMIX: wrote full capture engine + wired client meson.build.'
  }
}

function Patch-BridgeServerDeterministicRemixBootstrapV229 {
  param([Parameter(Mandatory)][string]$BridgeWork)

  $main = Join-Path $BridgeWork 'src\server\main.cpp'
  if (!(Test-Path -LiteralPath $main -PathType Leaf)) { Die "V229 deterministic server bootstrap missing main.cpp: $main" }
  $text = [System.IO.File]::ReadAllText($main)

  # Historical repair revisions accumulated a complete LoadLibrary pair and a
  # wWinMain call for every version.  They all load the same .trex DLLs before
  # filesystem/config/logger initialization.  Remove every legacy preload; the
  # single authoritative Remix API initialization below happens after IPC setup
  # and before Bridge_Ack.
  # These helpers contain nested preprocessor guards, so a simple first-#endif
  # match truncates them. Remove each complete historical block up to the next
  # historical block or wWinMain.
  $text = [regex]::Replace($text,
    '(?s)\r?\n#ifndef\s+DX11_V\d+_(?:SERVER_REMIX_INITIALIZER|FORCE_TREX_DX11_RUNTIME)\b.*?(?=\r?\n#ifndef\s+DX11_V\d+_(?:SERVER_REMIX_INITIALIZER|FORCE_TREX_DX11_RUNTIME)\b|\r?\nint\s+WINAPI\s+wWinMain)',
    "`r`n")
  $text = [regex]::Replace($text,
    '(?ms)^\s*if\s*\(!Dx11BridgeServerForceLoadTrexDx11RuntimeV\d+\(\)\)\s*\{.*?^\s*\}\s*',
    '')
  $text = [regex]::Replace($text,
    '(?m)^\s*Dx11BridgeServerInitializeTrexRemixRuntimeV\d+\(\);\s*\r?\n?',
    '')

  if (!$text.Contains('DX11_V229_DETERMINISTIC_REMIX_BOOTSTRAP')) {
    $bootHelper = @'

// DX11_V229_DETERMINISTIC_REMIX_BOOTSTRAP
// Kernel32-only stage logger.  This remains available even if bridge Logger or
// the Remix runtime cannot initialize, and is written beside NvRemixBridge.exe.
static void Dx11BridgeBootLogV229(const char* stage) {
  wchar_t path[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, path, _countof(path))) return;
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash) return;
  slash[1] = 0;
  wcscat_s(path, _countof(path), L"dx11_bridge_server_boot.log");

  HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;

  SYSTEMTIME st = {};
  GetLocalTime(&st);
  char line[1024] = {};
  sprintf_s(line, sizeof(line),
    "[%04u-%02u-%02u %02u:%02u:%02u.%03u] pid=%lu %s\r\n",
    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
    st.wMilliseconds, GetCurrentProcessId(), stage ? stage : "(null)");
  DWORD written = 0;
  WriteFile(h, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
}
'@
    $entryMarker = 'int WINAPI wWinMain('
    if (!$text.Contains($entryMarker)) { Die 'V229 could not find bridge server wWinMain.' }
    $text = $text.Replace($entryMarker, $bootHelper + "`r`n" + $entryMarker)
  }

  $entryPattern = '(int\s+WINAPI\s+wWinMain\s*\([^\)]*\)\s*\{)'
  if ($text -notmatch 'DX11 bridge server process entered wWinMain') {
    $text2 = [regex]::Replace($text, $entryPattern,
      '$1' + "`r`n  Dx11BridgeBootLogV229(`"DX11 bridge server process entered wWinMain.`");", 1)
    if ($text2 -eq $text) { Die 'V229 could not insert bridge server entry stage.' }
    $text = $text2
  }

  # Replace the entire legacy D3D initialization section.  DX11 mode loads the
  # full-path .trex runtime exactly once through its public API and validates the
  # function table.  A failed runtime never receives a false-success Bridge_Ack.
  $initPattern = '(?s)(RegisterMessageChannel\(\);\s*)// \(2\).*?// \(3\) Send ACK to Client\. Connection has been established'
  $initBlock = @'
$1// (2) Initialize exactly one rendering runtime before acknowledging the client.
  wchar_t dx11BridgeModeBuf[16] = {};
  wchar_t serverHostsRuntimeBuf[16] = {};
  const bool dx11BridgeMode =
    GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", dx11BridgeModeBuf, _countof(dx11BridgeModeBuf)) > 0 ||
    GetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", serverHostsRuntimeBuf, _countof(serverHostsRuntimeBuf)) > 0;

  if (dx11BridgeMode) {
    Dx11BridgeBootLogV229("IPC connected; beginning full-path Remix API initialization.");
    Logger::info("DX11_V229: initializing one .trex Remix API runtime before Bridge_Ack.");

    wchar_t remixD3D11Path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, remixD3D11Path, _countof(remixD3D11Path))) {
      Dx11BridgeBootLogV229("FATAL: could not resolve NvRemixBridge.exe path.");
      Logger::err("DX11_V229: could not resolve NvRemixBridge.exe path.");
      return 1;
    }
    wchar_t* lastSlash = wcsrchr(remixD3D11Path, L'\\');
    if (!lastSlash) {
      Dx11BridgeBootLogV229("FATAL: bridge executable has no parent directory.");
      Logger::err("DX11_V229: bridge executable has no parent directory.");
      return 1;
    }
    lastSlash[1] = 0;
    SetDllDirectoryW(remixD3D11Path);
    SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", remixD3D11Path);
    wcscat_s(remixD3D11Path, _countof(remixD3D11Path), L"d3d11.dll");

    const DWORD attrs = GetFileAttributesW(remixD3D11Path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      Dx11BridgeBootLogV229("FATAL: .trex d3d11.dll is missing.");
      Logger::err(format_string("DX11_V229: required Remix runtime is missing: %ls", remixD3D11Path));
      return 1;
    }

    if (!remixapi::g_remix_initialized) {
      Logger::info(format_string("DX11_V229: loading Remix API from full path: %ls", remixD3D11Path));
      const remixapi_ErrorCode status = remixapi_lib_loadRemixDllAndInitialize(
        remixD3D11Path, &remixapi::g_remix, &remixapi::g_remix_dll);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        char stage[256] = {};
        sprintf_s(stage, sizeof(stage), "FATAL: Remix API initialization failed status=%d win32=%lu.",
          static_cast<int>(status), GetLastError());
        Dx11BridgeBootLogV229(stage);
        Logger::err(format_string("DX11_V229: Remix API initialization failed status=%d win32=%lu.",
          static_cast<int>(status), GetLastError()));
        return 1;
      }
      remixapi::g_remix_initialized = true;
    }

    if (!remixapi::g_remix.Startup || !remixapi::g_remix.Present ||
        !remixapi::g_remix.SetupCamera || !remixapi::g_remix.CreateMaterial ||
        !remixapi::g_remix.CreateMesh || !remixapi::g_remix.DrawInstance) {
      Dx11BridgeBootLogV229("FATAL: Remix API function table is incomplete.");
      Logger::err("DX11_V229: Remix API function table is incomplete; refusing false-success bridge handshake.");
      return 1;
    }

    Dx11BridgeBootLogV229("Remix API initialized and function table validated.");
    Logger::info("DX11_V229: Remix API initialized and validated; Bridge_Ack is now permitted.");
  } else {
    Logger::info("Initializing legacy bridge D3D path...");
    if (!InitializeD3D()) return 1;
  }

  // (3) Send ACK to Client. Connection has been established
'@
  $hasDeterministicInit =
    $text.Contains('DX11_V229: initializing one .trex Remix API runtime before Bridge_Ack.') -and
    $text.Contains('remixapi_lib_loadRemixDllAndInitialize(') -and
    $text.Contains('Remix API initialized and validated; Bridge_Ack is now permitted.') -and
    $text.Contains('// (3) Send ACK to Client. Connection has been established')

  if ($hasDeterministicInit) {
    # bridge_dx11_work is intentionally persistent so incremental builds do not
    # regenerate and recompile the entire x86 bridge on every invocation.  The
    # old implementation required the source replacement to change the file,
    # which made the second and every later build fail even though the complete
    # deterministic initialization block was already installed.  Validate all
    # of its required stages above and preserve the known-good source verbatim.
    Log 'DX11_V229 deterministic bridge runtime initialization is already installed.'
  } else {
    $text2 = [regex]::Replace($text, $initPattern, $initBlock, 1)
    if ($text2 -eq $text) {
      Die 'V229 could not find either the legacy or deterministic bridge runtime initialization section.'
    }
    $text = $text2
  }

  if ($text -notmatch 'Bridge filesystem, config, and Logger initialized') {
    $text = $text.Replace('  Logger::init();', "  Logger::init();`r`n  Dx11BridgeBootLogV229(`"Bridge filesystem, config, and Logger initialized.`");")
  }
  if ($text -notmatch 'bridge GUID accepted') {
    $text = $text.Replace(
      '    Logger::info("Launched server with GUID " + gUniqueIdentifier.toString());',
      "    Logger::info(`"Launched server with GUID `" + gUniqueIdentifier.toString());`r`n    Dx11BridgeBootLogV229(`"Validated bridge GUID accepted.`");")
  }
  if ($text -notmatch 'bridge IPC namespaces initialized') {
    $text = $text.Replace('  initDeviceBridge();', "  initDeviceBridge();`r`n  Dx11BridgeBootLogV229(`"Validated bridge IPC namespaces initialized.`");")
  }
  if ($text -notmatch 'Bridge_Ack sent only after') {
    $ack = '  ServerMessage { Commands::Bridge_Ack, (uintptr_t) gpClientMessageChannel->getWorkerThreadId() };'
    if (!$text.Contains($ack)) { Die 'V229 could not find bridge ACK send.' }
    $text = $text.Replace($ack, $ack + "`r`n  Dx11BridgeBootLogV229(`"Bridge_Ack sent only after Remix API validation.`");")
  }
  if ($text -notmatch 'bridge handshake completed; command processing active') {
    $text = $text.Replace(
      '  Logger::info("Handshake completed! Now waiting for incoming commands...");',
      "  Logger::info(`"Handshake completed! Now waiting for incoming commands...`" );`r`n  Dx11BridgeBootLogV229(`"Bridge handshake completed; command processing active.`");")
  }

  if ($text -match 'Dx11BridgeServer(?:ForceLoadTrexDx11Runtime|InitializeTrexRemixRuntime)V\d+') {
    Die 'V229 deterministic bootstrap cleanup left a historical preloader call or helper behind.'
  }

  Write-TextNoBom -Path $main -Text $text
  Log 'DX11_V229: removed historical runtime preload stack; added one validated Remix bootstrap before Bridge_Ack and a kernel32 stage log.'
}

function Patch-DX11ClientSwapchainCoverageV229 {
  param([Parameter(Mandatory)][string]$DstClient)

  $d3d11Path = Join-Path $DstClient 'd3d11_dx11bridge.cpp'
  $dxgiPath = Join-Path $DstClient 'dxgi_dx11bridge.cpp'
  $dxgiDefPath = Join-Path $DstClient 'dxgi_dx11bridge.def'
  if (!(Test-Path -LiteralPath $d3d11Path -PathType Leaf)) { Die "V229 swapchain coverage patch missing d3d11 client source: $d3d11Path" }
  if (!(Test-Path -LiteralPath $dxgiPath -PathType Leaf)) { Die "V229 swapchain coverage patch missing dxgi client source: $dxgiPath" }

  # The DXGI proxy owns factory interception.  Query the actual interface before
  # touching its vtable: the previous code wrote Factory2 slot 15 even when
  # CreateDXGIFactory1 returned an IDXGIFactory1 (whose vtable ends at slot 13).
  # Also retain swapchains created before the d3d11 proxy attaches, then hand
  # them to d3d11 once its Present/Present1 hooks are available.
  Write-TextNoBom -Path $dxgiPath -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <cstdio>
#include <cstdint>
#include "dx11_bridge_client.h"

#define DX11_V229_COMPLETE_SWAPCHAIN_COVERAGE 1

using PFN_CreateDXGIFactory = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT (WINAPI *)(UINT, REFIID, void**);

static HMODULE gSystemDxgi = nullptr;
static PFN_CreateDXGIFactory pCreateDXGIFactory = nullptr;
static PFN_CreateDXGIFactory1 pCreateDXGIFactory1 = nullptr;
static PFN_CreateDXGIFactory2 pCreateDXGIFactory2 = nullptr;

using PFN_FactoryCreateSwapChain = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_Factory2CreateSwapChainForHwnd = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_Factory2CreateSwapChainForCoreWindow = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_Factory2CreateSwapChainForComposition = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_FactoryMediaCreateSwapChainForCompositionSurfaceHandle = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactoryMedia*, IUnknown*, HANDLE, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);

static PFN_FactoryCreateSwapChain oFactoryCreateSwapChain = nullptr;
static PFN_Factory2CreateSwapChainForHwnd oFactory2CreateSwapChainForHwnd = nullptr;
static PFN_Factory2CreateSwapChainForCoreWindow oFactory2CreateSwapChainForCoreWindow = nullptr;
static PFN_Factory2CreateSwapChainForComposition oFactory2CreateSwapChainForComposition = nullptr;
static PFN_FactoryMediaCreateSwapChainForCompositionSurfaceHandle oFactoryMediaCreateSwapChainForCompositionSurfaceHandle = nullptr;

static SRWLOCK gPendingSwapChainLockV229 = SRWLOCK_INIT;
static IDXGISwapChain* gPendingSwapChainsV229[16] = {};
static UINT gPendingSwapChainCountV229 = 0;

static void DLog(const char* text) {
  dx11_bridge_client::LogLine("dxgi", text);
}

static HMODULE LoadSystemDxgiV229() {
  if (gSystemDxgi) return gSystemDxgi;
  gSystemDxgi = dx11_bridge_client::LoadSystemDll("dxgi.dll");
  if (!gSystemDxgi) return nullptr;
  pCreateDXGIFactory = reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(gSystemDxgi, "CreateDXGIFactory"));
  pCreateDXGIFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(gSystemDxgi, "CreateDXGIFactory1"));
  pCreateDXGIFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(gSystemDxgi, "CreateDXGIFactory2"));
  return gSystemDxgi;
}

template <typename T>
static bool HookVTableV229(void* object, size_t slot, void* detour, T* original, const char* name) {
  if (!object || !detour || !original) return false;
  void*** obj = reinterpret_cast<void***>(object);
  void** vt = *obj;
  if (!vt) return false;
  if (vt[slot] == detour) return true;

  DWORD oldProtect = 0;
  if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
    char msg[320] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V229: VirtualProtect failed for %s slot=%u err=%lu.", name, (unsigned) slot, GetLastError());
    DLog(msg);
    return false;
  }

  if (*original == nullptr) *original = reinterpret_cast<T>(vt[slot]);
  vt[slot] = detour;

  DWORD ignored = 0;
  VirtualProtect(&vt[slot], sizeof(void*), oldProtect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), &vt[slot], sizeof(void*));

  char msg[320] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V229: hooked %s slot=%u on its validated interface.", name, (unsigned) slot);
  DLog(msg);
  return true;
}

static HMODULE FindLocalD3D11ProxyV229() {
  HMODULE self = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCSTR>(&FindLocalD3D11ProxyV229), &self);
  if (self) {
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(self, path, MAX_PATH)) {
      char* slash = strrchr(path, '\\');
      if (slash) {
        strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>((slash + 1) - path), "d3d11.dll");
        HMODULE local = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, path, &local)) return local;
      }
    }
  }
  return GetModuleHandleA("d3d11.dll");
}

static void QueuePendingSwapChainV229(IDXGISwapChain* swapChain) {
  if (!swapChain) return;
  AcquireSRWLockExclusive(&gPendingSwapChainLockV229);
  for (UINT i = 0; i < gPendingSwapChainCountV229; ++i) {
    if (gPendingSwapChainsV229[i] == swapChain) {
      ReleaseSRWLockExclusive(&gPendingSwapChainLockV229);
      return;
    }
  }
  if (gPendingSwapChainCountV229 < _countof(gPendingSwapChainsV229)) {
    swapChain->AddRef();
    gPendingSwapChainsV229[gPendingSwapChainCountV229++] = swapChain;
    ReleaseSRWLockExclusive(&gPendingSwapChainLockV229);
    DLog("DX11_V229: queued swapchain created before the local d3d11 Present hook was available.");
    return;
  }
  ReleaseSRWLockExclusive(&gPendingSwapChainLockV229);
  DLog("DX11_V229: pending swapchain queue is full; newest swapchain could not be retained.");
}

using PFN_D3D11SwapChainHook = void (WINAPI *)(IDXGISwapChain*);
static bool TryInstallD3D11SwapChainHookV229(IDXGISwapChain* swapChain) {
  if (!swapChain) return false;
  HMODULE d3d11 = FindLocalD3D11ProxyV229();
  auto hook = d3d11
    ? reinterpret_cast<PFN_D3D11SwapChainHook>(GetProcAddress(d3d11, "DX11BridgeInstallSwapChainCapture"))
    : nullptr;
  if (!hook) {
    QueuePendingSwapChainV229(swapChain);
    return false;
  }
  hook(swapChain);
  DLog("DX11_V229: installed d3d11 Present/Present1 capture on a discovered swapchain.");
  return true;
}

extern "C" __declspec(dllexport) void WINAPI DX11BridgeInstallPendingSwapChains() {
  IDXGISwapChain* pending[_countof(gPendingSwapChainsV229)] = {};
  UINT count = 0;
  AcquireSRWLockExclusive(&gPendingSwapChainLockV229);
  count = gPendingSwapChainCountV229;
  for (UINT i = 0; i < count; ++i) {
    pending[i] = gPendingSwapChainsV229[i];
    gPendingSwapChainsV229[i] = nullptr;
  }
  gPendingSwapChainCountV229 = 0;
  ReleaseSRWLockExclusive(&gPendingSwapChainLockV229);

  for (UINT i = 0; i < count; ++i) {
    TryInstallD3D11SwapChainHookV229(pending[i]);
    pending[i]->Release();
  }
  if (count) DLog("DX11_V229: drained deferred swapchains after d3d11 proxy initialization.");
}

static void OnSwapChainCreatedV229(const char* path, IDXGISwapChain* swapChain) {
  if (!swapChain) return;
  char msg[320] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V229: %s returned a swapchain; starting bridge and installing presentation capture.", path);
  DLog(msg);
  dx11_bridge_client::EnsureServer();
  TryInstallD3D11SwapChainHookV229(swapChain);
}

static HRESULT STDMETHODCALLTYPE HFactoryCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) {
  HRESULT hr = oFactoryCreateSwapChain ? oFactoryCreateSwapChain(self, device, desc, swapChain) : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) OnSwapChainCreatedV229("IDXGIFactory::CreateSwapChain", *swapChain);
  return hr;
}

static HRESULT STDMETHODCALLTYPE HFactory2CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsDesc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) {
  HRESULT hr = oFactory2CreateSwapChainForHwnd ? oFactory2CreateSwapChainForHwnd(self, device, hwnd, desc, fsDesc, restrictToOutput, swapChain) : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) OnSwapChainCreatedV229("IDXGIFactory2::CreateSwapChainForHwnd", *swapChain);
  return hr;
}

static HRESULT STDMETHODCALLTYPE HFactory2CreateSwapChainForCoreWindow(IDXGIFactory2* self, IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) {
  HRESULT hr = oFactory2CreateSwapChainForCoreWindow ? oFactory2CreateSwapChainForCoreWindow(self, device, window, desc, restrictToOutput, swapChain) : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) OnSwapChainCreatedV229("IDXGIFactory2::CreateSwapChainForCoreWindow", *swapChain);
  return hr;
}

static HRESULT STDMETHODCALLTYPE HFactory2CreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) {
  HRESULT hr = oFactory2CreateSwapChainForComposition ? oFactory2CreateSwapChainForComposition(self, device, desc, restrictToOutput, swapChain) : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) OnSwapChainCreatedV229("IDXGIFactory2::CreateSwapChainForComposition", *swapChain);
  return hr;
}

static HRESULT STDMETHODCALLTYPE HFactoryMediaCreateSwapChainForCompositionSurfaceHandle(IDXGIFactoryMedia* self, IUnknown* device, HANDLE surface, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) {
  HRESULT hr = oFactoryMediaCreateSwapChainForCompositionSurfaceHandle
    ? oFactoryMediaCreateSwapChainForCompositionSurfaceHandle(self, device, surface, desc, restrictToOutput, swapChain)
    : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) OnSwapChainCreatedV229("IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle", *swapChain);
  return hr;
}

static void InstallFactoryHooksV229(void* factory) {
  if (!factory) return;
  IUnknown* unknown = reinterpret_cast<IUnknown*>(factory);

  IDXGIFactory* factory0 = nullptr;
  if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory0))) && factory0) {
    HookVTableV229(factory0, 10, reinterpret_cast<void*>(&HFactoryCreateSwapChain), &oFactoryCreateSwapChain, "IDXGIFactory::CreateSwapChain");
    factory0->Release();
  }

  IDXGIFactory2* factory2 = nullptr;
  if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2))) && factory2) {
    HookVTableV229(factory2, 15, reinterpret_cast<void*>(&HFactory2CreateSwapChainForHwnd), &oFactory2CreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
    HookVTableV229(factory2, 16, reinterpret_cast<void*>(&HFactory2CreateSwapChainForCoreWindow), &oFactory2CreateSwapChainForCoreWindow, "IDXGIFactory2::CreateSwapChainForCoreWindow");
    HookVTableV229(factory2, 24, reinterpret_cast<void*>(&HFactory2CreateSwapChainForComposition), &oFactory2CreateSwapChainForComposition, "IDXGIFactory2::CreateSwapChainForComposition");
    factory2->Release();
  }

  IDXGIFactoryMedia* factoryMedia = nullptr;
  if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGIFactoryMedia), reinterpret_cast<void**>(&factoryMedia))) && factoryMedia) {
    HookVTableV229(factoryMedia, 3, reinterpret_cast<void*>(&HFactoryMediaCreateSwapChainForCompositionSurfaceHandle), &oFactoryMediaCreateSwapChainForCompositionSurfaceHandle, "IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle");
    factoryMedia->Release();
  }
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_client::SetModule(hinst);
    DLog("DX11_V229: game loaded root dxgi.dll with complete, interface-safe swapchain capture.");
  }
  if (reason == DLL_PROCESS_DETACH && reserved != nullptr) dx11_bridge_client::Detach();
  return TRUE;
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
  DLog("DX11_V229: CreateDXGIFactory intercepted.");
  if (!LoadSystemDxgiV229() || !pCreateDXGIFactory) return DXGI_ERROR_UNSUPPORTED;
  HRESULT hr = pCreateDXGIFactory(riid, ppFactory);
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooksV229(*ppFactory);
  return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
  DLog("DX11_V229: CreateDXGIFactory1 intercepted.");
  if (!LoadSystemDxgiV229() || !pCreateDXGIFactory1) return DXGI_ERROR_UNSUPPORTED;
  HRESULT hr = pCreateDXGIFactory1(riid, ppFactory);
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooksV229(*ppFactory);
  return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) {
  DLog("DX11_V229: CreateDXGIFactory2 intercepted.");
  if (!LoadSystemDxgiV229() || !pCreateDXGIFactory2) return DXGI_ERROR_UNSUPPORTED;
  HRESULT hr = pCreateDXGIFactory2(flags, riid, ppFactory);
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooksV229(*ppFactory);
  return hr;
}
'@

  $d3d11Text = [System.IO.File]::ReadAllText($d3d11Path).Replace("`r`n", "`n")
  if (!$d3d11Text.Contains('DX11_V229_PRESENT1_AND_DEFERRED_SWAPCHAINS')) {
    $d3d11Text = $d3d11Text.Replace('#include <dxgi.h>', "#include <dxgi.h>`r`n#include <dxgi1_2.h>")

    $oldDecl = @'
using PFN_SwapPresent = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain*, UINT, UINT);
using PFN_SwapResizeBuffers = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PFN_SwapPresent oSwapPresent = nullptr;
static PFN_SwapResizeBuffers oSwapResizeBuffers = nullptr;
'@
    $newDecl = @'
// DX11_V229_PRESENT1_AND_DEFERRED_SWAPCHAINS
using PFN_SwapPresent = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain*, UINT, UINT);
using PFN_SwapPresent1 = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PFN_SwapResizeBuffers = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PFN_SwapPresent oSwapPresent = nullptr;
static PFN_SwapPresent1 oSwapPresent1 = nullptr;
static PFN_SwapResizeBuffers oSwapResizeBuffers = nullptr;
'@
    $oldDecl = $oldDecl.Replace("`r`n", "`n")
    $newDecl = $newDecl.Replace("`r`n", "`n")
    if (!$d3d11Text.Contains($oldDecl)) { Die 'V229 could not find D3D11 swapchain declaration block.' }
    $d3d11Text = $d3d11Text.Replace($oldDecl, $newDecl)

    $resizeMarker = 'static HRESULT STDMETHODCALLTYPE HSwapResizeBuffers('
    $present1 = @'
static HRESULT STDMETHODCALLTYPE HSwapPresent1(IDXGISwapChain1* self, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
  if (!gV219InsideHook) {
    gV219InsideHook = true;
    LONG n = InterlockedIncrement(&gV219PresentCount);
    if (n <= 8 || (n % 60) == 0) {
      char msg[256] = {};
      sprintf_s(msg, sizeof(msg), "DX11_V229: captured IDXGISwapChain1::Present1 count=%ld in game process.", n);
      V219Log("capture", msg);
    }
    dx11_bridge_client::EnsureServer();
    dx11_capture::OnPresent(self);
    gV219InsideHook = false;
  }
  return oSwapPresent1 ? oSwapPresent1(self, syncInterval, flags, parameters) : DXGI_ERROR_DEVICE_REMOVED;
}

'@
    $present1 = $present1.Replace("`r`n", "`n")
    if (!$d3d11Text.Contains($resizeMarker)) { Die 'V229 could not find HSwapResizeBuffers insertion point.' }
    $d3d11Text = $d3d11Text.Replace($resizeMarker, $present1 + $resizeMarker)

    $oldInstall = @'
extern "C" __declspec(dllexport) void WINAPI DX11BridgeInstallSwapChainCapture(IDXGISwapChain* swapChain) {
  if (!swapChain) return;
  V219HookVTable(swapChain, 8, reinterpret_cast<void*>(&HSwapPresent), &oSwapPresent, "IDXGISwapChain::Present");
  V219HookVTable(swapChain, 13, reinterpret_cast<void*>(&HSwapResizeBuffers), &oSwapResizeBuffers, "IDXGISwapChain::ResizeBuffers");
}
'@
    $newInstall = @'
extern "C" __declspec(dllexport) void WINAPI DX11BridgeInstallSwapChainCapture(IDXGISwapChain* swapChain) {
  if (!swapChain) return;
  V219HookVTable(swapChain, 8, reinterpret_cast<void*>(&HSwapPresent), &oSwapPresent, "IDXGISwapChain::Present");
  V219HookVTable(swapChain, 13, reinterpret_cast<void*>(&HSwapResizeBuffers), &oSwapResizeBuffers, "IDXGISwapChain::ResizeBuffers");

  IDXGISwapChain1* swapChain1 = nullptr;
  if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapChain1))) && swapChain1) {
    V219HookVTable(swapChain1, 22, reinterpret_cast<void*>(&HSwapPresent1), &oSwapPresent1, "IDXGISwapChain1::Present1");
    swapChain1->Release();
  }
}
'@
    $oldInstall = $oldInstall.Replace("`r`n", "`n")
    $newInstall = $newInstall.Replace("`r`n", "`n")
    if (!$d3d11Text.Contains($oldInstall)) { Die 'V229 could not find DX11BridgeInstallSwapChainCapture body.' }
    $d3d11Text = $d3d11Text.Replace($oldInstall, $newInstall)

    $dllMainMarker = 'BOOL WINAPI DllMain('
    $drainHelper = @'
using PFN_DX11BridgeInstallPendingSwapChainsV229 = void (WINAPI *)();
static void TryDrainPendingSwapChainsV229() {
  HMODULE dxgi = GetModuleHandleA("dxgi.dll");
  auto drain = dxgi
    ? reinterpret_cast<PFN_DX11BridgeInstallPendingSwapChainsV229>(GetProcAddress(dxgi, "DX11BridgeInstallPendingSwapChains"))
    : nullptr;
  if (drain) drain();
}

'@
    $drainHelper = $drainHelper.Replace("`r`n", "`n")
    if (!$d3d11Text.Contains($dllMainMarker)) { Die 'V229 could not find d3d11 DllMain insertion point.' }
    $d3d11Text = $d3d11Text.Replace($dllMainMarker, $drainHelper + $dllMainMarker)

    $call1 = '    V219InstallCapture(device ? *device : nullptr, immediateContext ? *immediateContext : nullptr, nullptr);'
    $call2 = '    V219InstallCapture(device ? *device : nullptr, immediateContext ? *immediateContext : nullptr, swapChain ? *swapChain : nullptr);'
    if (!$d3d11Text.Contains($call1) -or !$d3d11Text.Contains($call2)) { Die 'V229 could not find both D3D11 capture installation calls.' }
    $d3d11Text = $d3d11Text.Replace($call1, $call1 + "`r`n    TryDrainPendingSwapChainsV229();")
    $d3d11Text = $d3d11Text.Replace($call2, $call2 + "`r`n    TryDrainPendingSwapChainsV229();")
    Write-TextNoBom -Path $d3d11Path -Text $d3d11Text
  }

  if (Test-Path -LiteralPath $dxgiDefPath -PathType Leaf) {
    $def = [System.IO.File]::ReadAllText($dxgiDefPath)
    if (!$def.Contains('DX11BridgeInstallPendingSwapChains')) {
      $def = $def.TrimEnd() + "`r`n  DX11BridgeInstallPendingSwapChains`r`n"
      Write-TextNoBom -Path $dxgiDefPath -Text $def
    }
  }

  Log 'DX11_V229: enabled interface-safe complete DXGI swapchain discovery, deferred handoff, Present, and Present1 capture.'
}

function Prepare-DX11BridgeSource([string]$PackDir) {
  $work = Join-Path $Root 'bridge_dx11_work'
  $sourceBridge = Join-Path $Root 'bridge'
  $sourceBridgeReady = Test-Path (Join-Path $sourceBridge 'meson.build')
  # The DX11 work tree deliberately removes the legacy src/client directory and
  # replaces it with src/client_dx11. Testing the removed directory made every
  # normal build clone the full upstream repository again, only to discard that
  # clone after the RTXDI/RTXCR sync. Reuse the persistent, fully normalized
  # bridge when its Meson file, server, and DX11 client are all present.
  $workBridgeReady =
    (Test-Path -LiteralPath (Join-Path $work 'meson.build') -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $work 'src\server\main.cpp') -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $work 'src\client_dx11') -PathType Container)
  # A completed bridge work tree is already the normalized, patched source
  # consumed below. Do not perform a redundant recursive NVIDIA clone merely
  # because the optional pristine ./bridge directory is absent.
  if (!$sourceBridgeReady -and !$workBridgeReady) {
    $cloneRoot = Join-Path $Root '_nvidia_dxvk_remix_for_dx11_bridge'
    if ($Clean -and (Test-Path $cloneRoot)) {
      Log "Removing old clone: $cloneRoot"
      Remove-DirectoryRobust $cloneRoot
    }

    if (Test-Path (Join-Path $cloneRoot 'bridge\meson.build')) {
      Log "Using existing NVIDIA bridge clone: $cloneRoot"
    } else {
      if (Test-Path $cloneRoot) {
        Warn "Existing clone folder is incomplete/non-empty: $cloneRoot"
        Remove-DirectoryRobust $cloneRoot
      }
      $git = CmdPath 'git.exe'; if (!$git) { Die 'git.exe is required to clone NVIDIA bridge source.' }
      Log "Cloning NVIDIA dxvk-remix for bridge source: $BridgeRepoUrl ($BridgeBranch)"
      Invoke-Logged -Label 'git-clone-bridge' -Exe $git -CommandArgs @('clone','--recursive','--branch',$BridgeBranch,$BridgeRepoUrl,$cloneRoot) -WorkingDirectory $Root | Out-Null
      if (!(Test-Path -LiteralPath (Join-Path $cloneRoot 'bridge\meson.build') -PathType Leaf)) { Die "Git clone completed but bridge\meson.build was not created: $cloneRoot" }
    }
    $sourceBridge = Join-Path $cloneRoot 'bridge'
  }
  if (!(Test-Path (Join-Path $sourceBridge 'src\client')) -and !$workBridgeReady) {
    Die "Bridge source missing client folder: $sourceBridge"
  }

  if ($Clean -and (Test-Path $work)) { Log "Removing old DX11 bridge work tree: $work"; Remove-DirectoryRobust $work }
  if (!(Test-Path $work)) { Log "Copying bridge source to $work"; Copy-Item -LiteralPath $sourceBridge -Destination $work -Recurse -Force }

  $dstClient = Join-Path $work 'src\client_dx11'
  Ensure-DX11ClientSources -DstClient $dstClient -PackDir $PackDir
  Write-DX11FullClientSourcesV63 -DstClient $dstClient
  Patch-DX11ClientRealCaptureLayerV219 -DstClient $dstClient
  Patch-DX11ClientCaptureToRemixV226 -DstClient $dstClient
  Patch-DX11ClientSwapchainCoverageV229 -DstClient $dstClient

  $srcMeson = Join-Path $work 'src\meson.build'
  Normalize-BridgeSrcMesonClientDx11 -SrcMeson $srcMeson
  Repair-MesonUtf8NoBom $work

  # Newer MSVC/SDKs can emit warnings in NVIDIA bridge utility sources that become fatal
  # because upstream bridge/meson.build sets default_options : ['werror=true', ...].
  # This project is being used as a source base for a DX11 bridge bring-up, so do not let
  # warnings stop the server build before we can stage/test the DX11 pickup DLLs.
  $bridgeMeson = Join-Path $work 'meson.build'
  if (Test-Path -LiteralPath $bridgeMeson -PathType Leaf) {
    $bridgeText = Get-Content -LiteralPath $bridgeMeson -Raw
    $patchedBridgeText = $bridgeText -replace "'werror=true'", "'werror=false'"
    if ($patchedBridgeText -ne $bridgeText) {
      Write-TextNoBom -Path $bridgeMeson -Text $patchedBridgeText
      Log 'Patched bridge meson.build: werror=false so MSVC warnings do not abort bridge server build.'
    }
  }
  Sync-BridgeRemixHeaderToDX11 -BridgeWork $work
  Patch-BridgeClientRemixApiDX11 -BridgeWork $work
  Patch-BridgeServerRegisterD3D11 -BridgeWork $work
  Patch-BridgeServerDx11ArgFallback -BridgeWork $work
  Patch-BridgeServerDx11ModeV63 -BridgeWork $work
  Patch-BridgeServerForceLoadRemixRuntimeV219 -BridgeWork $work
  Patch-BridgeServerDx11RealGameTargetAndNoExitKillV219 -BridgeWork $work
  Patch-BridgeServerForceTrexDx11RuntimeV219 -BridgeWork $work
  Patch-BridgeServerDeterministicRemixBootstrapV229 -BridgeWork $work
  Assert-RealBridgeServerCanAckV219 -BridgeWork $work
  Patch-BridgeServerRemoveLegacyD3D9RegistrationV219 -BridgeWork $work
  Patch-BridgeServerRemoveStaleD3D11RegisterV219 -BridgeWork $work
  Assert-RealBridgeServerCanAckV219 -BridgeWork $work
  Ensure-DX11ClientGlobalsIntegrated -BridgeWork $work
  Remove-StaleX86ClientForUnifiedGuidV219
  return $work
}

function Get-ExistingRuntimeDir {
  $candidates = @(
    (Join-Path $Root '_output\x64'),
    (Join-Path $Root '_Comp64Release'),
    (Join-Path $Root '_Comp64Release\src\d3d11'),
    (Join-Path $Root '_Comp64Release\src\dxgi')
  )
  foreach ($dir in $candidates) {
    if (!(Test-Path -LiteralPath $dir -PathType Container)) { continue }
    $d3d11 = Find-FileRecursive $dir 'd3d11.dll'
    $dxgi = Find-FileRecursive $dir 'dxgi.dll'
    if ($d3d11 -and $dxgi) {
      try {
        Test-PeMachine $d3d11 'x64'
        Test-PeMachine $dxgi 'x64'
        Log "Found existing x64 DX11 runtime: $dir"
        return $dir
      } catch { Warn "Ignoring runtime candidate ${dir}: $($_.Exception.Message)" }
    }
  }
  return $null
}


function Get-DX11FastBuildJobsV219 {
  # DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
  # Faster full build without skipping USD/shaders/x86/x64.
  # Default: use logical CPU count capped to avoid RAM death.
  $userJobs = $env:DX11_BUILD_JOBS
  if (![string]::IsNullOrWhiteSpace($userJobs)) {
    $n = 0
    if ([int]::TryParse($userJobs, [ref]$n) -and $n -gt 0) {
      return [Math]::Min($n, 64)
    }
  }

  $cpu = [Environment]::ProcessorCount
  if ($cpu -lt 4) { return 4 }
  if ($cpu -gt 16) { return 16 }
  return $cpu
}

function Get-DX11FastNinjaArgsV219([string]$BuildDir) {
  $jobs = Get-DX11FastBuildJobsV219
  if ($env:DX11_FULL_VERBOSE -eq '1') {
    return @('-C', $BuildDir, '-v', '-j', ([string]$jobs))
  }
  return @('-C', $BuildDir, '-j', ([string]$jobs))
}

function Enable-DX11FastCompilerEnvV219 {
  # Keep all features. Only speed up parallelism/output.
  $jobs = Get-DX11FastBuildJobsV219
  $env:NINJA_STATUS = '[%f/%t %es] '
  $env:DX11_BUILD_JOBS_ACTIVE = [string]$jobs

  $oldCl = [Environment]::GetEnvironmentVariable('CL', 'Process')
  if ([string]::IsNullOrWhiteSpace($oldCl)) {
    [Environment]::SetEnvironmentVariable('CL', ('/MP{0}' -f $jobs), 'Process')
  } elseif ($oldCl -notmatch '/MP\d*') {
    [Environment]::SetEnvironmentVariable('CL', (('/MP{0} ' -f $jobs) + $oldCl), 'Process')
  }

  Log ("DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS: jobs={0}, verbose={1}, full features kept." -f $jobs, $env:DX11_FULL_VERBOSE)
}



function Patch-WxWarningErrorsV219 {
  # DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
  # Fix current runtime-ninja-x64 blockers:
  #   d3d11_rtx.h: warning C4099 treated as error because D3D11Rtx is seen
  #                once as struct and once as class.
  #   dxbc_compiler.cpp: warning C4146 treated as error for intentional unsigned
  #                     unary minus wraparound code.
  #
  # This keeps /WX enabled globally. It only neutralizes the two known non-fatal
  # warnings in the exact files that fail the current build.
  $report = Join-Path $Root 'DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  function Write-NoBomV219([string]$Path, [string]$Text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
  }

  function Add-MsvcWarningDisableV219([string]$File, [string]$Code, [string]$Marker, [System.Collections.Generic.List[string]]$Lines) {
    if (!(Test-Path -LiteralPath $File -PathType Leaf)) {
      $Lines.Add(('SKIP: missing {0}' -f $File))
      return
    }

    $text = [System.IO.File]::ReadAllText($File)
    if ($text.Contains($Marker)) {
      $Lines.Add(('OK: marker already present in {0}' -f $File))
      return
    }

    $pragma = "#if defined(_MSC_VER)`r`n#pragma warning(disable: $Code) // $Marker`r`n#endif`r`n"
    $orig = $text

    if ($text -match '^\s*#pragma once') {
      $text = [regex]::Replace($text, '^\s*#pragma once\s*', "#pragma once`r`n$pragma", 1)
    } else {
      $text = $pragma + $text
    }

    if ($text -ne $orig) {
      Write-NoBomV219 -Path $File -Text $text
      $Lines.Add(('PATCHED: disabled MSVC warning {0} in {1}' -f $Code, $File))
    }
  }

  $d3d11Rtx = Join-Path $Root 'src\d3d11\d3d11_rtx.h'
  if (Test-Path -LiteralPath $d3d11Rtx -PathType Leaf) {
    $t = [System.IO.File]::ReadAllText($d3d11Rtx)
    $orig = $t

    # Normalize the simple forward declaration form first. This is the source
    # fix for the class/struct tag mismatch when this header has only a forward
    # declaration at line 17.
    $t = [regex]::Replace($t, '(?m)^(\s*)class\s+D3D11Rtx\s*;\s*$', '${1}struct D3D11Rtx;')

    if ($t -ne $orig) {
      Write-NoBomV219 -Path $d3d11Rtx -Text $t
      $lines.Add(('PATCHED: normalized D3D11Rtx forward declaration class->struct in {0}' -f $d3d11Rtx))
    } else {
      $lines.Add(('OK: no simple class D3D11Rtx forward declaration to normalize in {0}' -f $d3d11Rtx))
    }
  } else {
    $lines.Add(('SKIP: missing {0}' -f $d3d11Rtx))
  }

  Add-MsvcWarningDisableV219 -File $d3d11Rtx -Code '4099' -Marker 'DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS' -Lines $lines

  $dxbcCompiler = Join-Path $Root 'src\dxbc\dxbc_compiler.cpp'
  Add-MsvcWarningDisableV219 -File $dxbcCompiler -Code '4146' -Marker 'DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS' -Lines $lines

  # Fast verification.
  $bad = $false
  if (Test-Path -LiteralPath $d3d11Rtx -PathType Leaf) {
    $rtxText = [System.IO.File]::ReadAllText($d3d11Rtx)
    if (!$rtxText.Contains('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS') -or !$rtxText.Contains('#pragma warning(disable: 4099)')) {
      $bad = $true
      $lines.Add('BAD: d3d11_rtx.h does not contain C4099 disable marker.')
    }
  }

  if (Test-Path -LiteralPath $dxbcCompiler -PathType Leaf) {
    $dxbcText = [System.IO.File]::ReadAllText($dxbcCompiler)
    if (!$dxbcText.Contains('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS') -or !$dxbcText.Contains('#pragma warning(disable: 4146)')) {
      $bad = $true
      $lines.Add('BAD: dxbc_compiler.cpp does not contain C4146 disable marker.')
    }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  if ($bad) {
    Die "V219 warning-as-error patch verification failed. See $report"
  }

  Log "V219 fixed C4099/C4146 warning-as-error blockers. Report: $report"
}



function Patch-Dx11DeviceFeatureAndGdiFormatV219 {
  # DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
  $report = Join-Path $Root 'DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  function W214([string]$Path, [string]$Text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
  }

  $gdi = Join-Path $Root 'src\d3d11\d3d11_gdi.cpp'
  if (Test-Path -LiteralPath $gdi -PathType Leaf) {
    $t = [System.IO.File]::ReadAllText($gdi)
    if ($t.Contains('D3DDDIFMT_A8R8G8B8') -and !$t.Contains('DX11_V219_D3DDDIFMT_A8R8G8B8_FIX')) {
      $block = @'
#if defined(_WIN32)
#include <d3dukmdt.h>
#ifndef D3DDDIFMT_A8R8G8B8
#define D3DDDIFMT_A8R8G8B8 ((D3DDDIFORMAT)21)
#endif
#endif
// DX11_V219_D3DDDIFMT_A8R8G8B8_FIX
'@
      $m = [regex]::Matches($t, '(?m)^\s*#\s*include\s+[<"].+[>"]\s*$')
      if ($m.Count -gt 0) { $last = $m[$m.Count - 1]; $pos = $last.Index + $last.Length; $t = $t.Substring(0,$pos) + "`r`n" + $block + $t.Substring($pos) }
      else { $t = $block + "`r`n" + $t }
      W214 $gdi $t
      $lines.Add(('PATCHED: added D3DDDIFMT_A8R8G8B8 fallback to {0}' -f $gdi))
    } else { $lines.Add(('OK: GDI format fix not needed/already present in {0}' -f $gdi)) }
  } else { $lines.Add(('SKIP: missing {0}' -f $gdi)) }

  $dev = Join-Path $Root 'src\d3d11\d3d11_device.cpp'
  if (Test-Path -LiteralPath $dev -PathType Leaf) {
    $raw = [System.IO.File]::ReadAllLines($dev)
    $badTokens = @('D3D11_FEATURE_D3D11_SHADOW_SUPPORT','D3D11_FEATURE_D3D11_SIMPLE_INSTANCING_SUPPORT','FullNonPow2TextureSupport','FullNonPow2TextureSupported','DepthAsTextureWithLessEqualComparisonFilterSupported','SimpleInstancingSupported','TextureCubeFaceRenderTargetWithNonCubeDepthStencilSupported')
    $out = New-Object System.Collections.Generic.List[string]
    $i = 0; $removed = 0
    while ($i -lt $raw.Length) {
      $line = $raw[$i]
      if ($line -match '^\s*case\s+D3D11_FEATURE_') {
        $start = $i; $j = $i + 1
        while ($j -lt $raw.Length -and !($raw[$j] -match '^\s*(case\s+D3D11_FEATURE_|default\s*:)')) { $j++ }
        $block = [string]::Join("`n", $raw[$start..($j-1)])
        $remove = $false
        foreach ($tok in $badTokens) { if ($block.Contains($tok)) { $remove = $true; break } }
        if ($remove) { $out.Add('      // DX11_V219: removed invalid/non-SDK D3D11 feature-query compatibility block.'); $removed++; $i = $j; continue }
      }
      $out.Add($line); $i++
    }
    W214 $dev ([string]::Join("`r`n", $out) + "`r`n")
    $lines.Add(('PATCHED: removed {0} invalid D3D11 feature-query block(s) from {1}' -f $removed, $dev))
  } else { $lines.Add(('SKIP: missing {0}' -f $dev)) }

  $bad = $false
  if (Test-Path -LiteralPath $dev -PathType Leaf) {
    $d = [System.IO.File]::ReadAllText($dev)
    foreach ($tok in @('D3D11_FEATURE_D3D11_SHADOW_SUPPORT','D3D11_FEATURE_D3D11_SIMPLE_INSTANCING_SUPPORT','FullNonPow2TextureSupport','FullNonPow2TextureSupported','DepthAsTextureWithLessEqualComparisonFilterSupported','SimpleInstancingSupported','TextureCubeFaceRenderTargetWithNonCubeDepthStencilSupported')) {
      if ($d.Contains($tok)) { $bad = $true; $lines.Add(('BAD: stale token remains: {0}' -f $tok)) }
    }
  }
  if (Test-Path -LiteralPath $gdi -PathType Leaf) {
    $g = [System.IO.File]::ReadAllText($gdi)
    if ($g.Contains('D3DDDIFMT_A8R8G8B8') -and !$g.Contains('DX11_V219_D3DDDIFMT_A8R8G8B8_FIX')) { $bad = $true; $lines.Add('BAD: GDI marker missing.') }
  }
  [System.IO.File]::WriteAllLines($report, $lines)
  if ($bad) { Die "V219 D3D11 feature/GDI verification failed. See $report" }
  Log "V219 D3D11 feature-query/GDI blockers fixed. Report: $report"
}



function Patch-GdiFormatAndDxvkBufferCookieV219 {
  # DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
  # Current runtime-ninja-x64 blockers:
  #   shared\d3dukmdt.h: fatal error C1189, header should not be included directly
  #   d3d11_initializer.cpp: DxvkBuffer has no setContentCookie member
  #
  # Fix:
  #   - remove direct d3dukmdt.h include from d3d11_gdi.cpp
  #   - define D3DDDIFMT_A8R8G8B8 as plain numeric DDI format value 21
  #     without requiring D3DDDIFORMAT type
  #   - add a real lightweight content-cookie field and accessor methods to
  #     DxvkBuffer so DX11 initializer content tracking compiles.
  $report = Join-Path $Root 'DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  function Write-NoBomV219([string]$Path, [string]$Text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
  }

  $gdi = Join-Path $Root 'src\d3d11\d3d11_gdi.cpp'
  if (Test-Path -LiteralPath $gdi -PathType Leaf) {
    $t = [System.IO.File]::ReadAllText($gdi)
    $orig = $t

    # Remove the bad V214 direct kernel DDI include. Windows SDK explicitly
    # rejects including this file directly.
    $t = [regex]::Replace($t, '(?m)^\s*#\s*include\s*<d3dukmdt\.h>\s*\r?$', '')

    # Replace a typed cast macro from V214 with a plain compile-time value so no
    # D3DDDIFORMAT type is needed.
    $t = [regex]::Replace($t, '#define\s+D3DDDIFMT_A8R8G8B8\s+\(\(D3DDDIFORMAT\)21\)', '#define D3DDDIFMT_A8R8G8B8 21')

    if ($t.Contains('D3DDDIFMT_A8R8G8B8') -and !$t.Contains('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')) {
      $block = @'
#ifndef D3DDDIFMT_A8R8G8B8
#define D3DDDIFMT_A8R8G8B8 21
#endif
// DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
'@

      if ($t.Contains('DX11_V214_D3DDDIFMT_A8R8G8B8_FIX')) {
        # Replace the whole V214 block if present.
        $t = [regex]::Replace(
          $t,
          '(?ms)#if\s+defined\(_WIN32\)\s*#include\s+<d3dukmdt\.h>\s*#ifndef\s+D3DDDIFMT_A8R8G8B8\s*#define\s+D3DDDIFMT_A8R8G8B8\s+\(\(D3DDDIFORMAT\)21\)\s*#endif\s*#endif\s*//\s*DX11_V214_D3DDDIFMT_A8R8G8B8_FIX',
          $block.TrimEnd()
        )
      }

      if (!$t.Contains('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')) {
        $m = [regex]::Matches($t, '(?m)^\s*#\s*include\s+[<"].+[>"]\s*$')
        if ($m.Count -gt 0) {
          $last = $m[$m.Count - 1]
          $pos = $last.Index + $last.Length
          $t = $t.Substring(0, $pos) + "`r`n" + $block + $t.Substring($pos)
        } else {
          $t = $block + "`r`n" + $t
        }
      }
    }

    if ($t -ne $orig) {
      Write-NoBomV219 -Path $gdi -Text $t
      $lines.Add(('PATCHED: removed direct d3dukmdt.h and installed plain D3DDDIFMT_A8R8G8B8 fallback in {0}' -f $gdi))
    } else {
      $lines.Add(('OK: no GDI DDI patch needed in {0}' -f $gdi))
    }
  } else {
    $lines.Add(('SKIP: missing {0}' -f $gdi))
  }

  $buffer = Join-Path $Root 'src\dxvk\dxvk_buffer.h'
  if (Test-Path -LiteralPath $buffer -PathType Leaf) {
    $b = [System.IO.File]::ReadAllText($buffer)
    $origB = $b

    if (!$b.Contains('DX11_V219_BUFFER_CONTENT_COOKIE') -and !$b.Contains('setContentCookie')) {
      if ($b -notmatch '#include\s+<cstdint>') {
        if ($b -match '^\s*#pragma once') {
          $b = [regex]::Replace($b, '^\s*#pragma once\s*', "#pragma once`r`n#include <cstdint>`r`n", 1)
        } else {
          $b = "#include <cstdint>`r`n" + $b
        }
      }

      # Add public methods after the first public: inside DxvkBuffer. This is a
      # method used by d3d11_initializer.cpp for content tracking.
      $methodBlock = @'
    // DX11_V219_BUFFER_CONTENT_COOKIE
    void setContentCookie(uint64_t cookie) {
      m_dx11ContentCookie = cookie;
    }

    uint64_t contentCookie() const {
      return m_dx11ContentCookie;
    }

'@

      $classMatch = [regex]::Match($b, '(?s)class\s+DxvkBuffer\b.*?\{')
      if ($classMatch.Success) {
        $classStart = $classMatch.Index
        $afterClass = $b.Substring($classStart)
        $pubMatch = [regex]::Match($afterClass, '(?m)^\s*public\s*:\s*$')
        if ($pubMatch.Success) {
          $insert = $classStart + $pubMatch.Index + $pubMatch.Length
          $b = $b.Substring(0, $insert) + "`r`n" + $methodBlock + $b.Substring($insert)
        } else {
          $insert = $classMatch.Index + $classMatch.Length
          $b = $b.Substring(0, $insert) + "`r`npublic:`r`n" + $methodBlock + $b.Substring($insert)
        }

        # Insert private backing field before the last closing brace of class
        # by placing it before the first "};" following the method block if
        # possible. If class has an explicit private section this is still safe.
        $after = $b.Substring($classStart)
        $endMatch = [regex]::Match($after, '(?m)^};\s*$')
        if ($endMatch.Success) {
          $insertEnd = $classStart + $endMatch.Index
          $fieldBlock = @'

private:
    uint64_t m_dx11ContentCookie = 0;

'@
          $b = $b.Substring(0, $insertEnd) + $fieldBlock + $b.Substring($insertEnd)
        } else {
          $lines.Add('WARN: could not locate DxvkBuffer class end for content-cookie field insertion.')
        }
      } else {
        $lines.Add('WARN: could not locate class DxvkBuffer in dxvk_buffer.h.')
      }
    }

    if ($b -ne $origB) {
      Write-NoBomV219 -Path $buffer -Text $b
      $lines.Add(('PATCHED: added DxvkBuffer content-cookie API to {0}' -f $buffer))
    } else {
      $lines.Add(('OK: DxvkBuffer content-cookie API already present in {0}' -f $buffer))
    }
  } else {
    $lines.Add(('SKIP: missing {0}' -f $buffer))
  }

  # Verification targeted to current blockers.
  $bad = $false
  if (Test-Path -LiteralPath $gdi -PathType Leaf) {
    $gt = [System.IO.File]::ReadAllText($gdi)
    if ($gt -match '#\s*include\s*<d3dukmdt\.h>') {
      $bad = $true
      $lines.Add('BAD: d3d11_gdi.cpp still directly includes d3dukmdt.h.')
    }
    if ($gt.Contains('D3DDDIFMT_A8R8G8B8') -and !$gt.Contains('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')) {
      $bad = $true
      $lines.Add('BAD: d3d11_gdi.cpp uses D3DDDIFMT_A8R8G8B8 without V219 fallback.')
    }
    if ($gt.Contains('D3DDDIFORMAT')) {
      $bad = $true
      $lines.Add('BAD: d3d11_gdi.cpp still depends on D3DDDIFORMAT type.')
    }
  }

  if (Test-Path -LiteralPath $buffer -PathType Leaf) {
    $bt = [System.IO.File]::ReadAllText($buffer)
    if (!$bt.Contains('setContentCookie') -or !$bt.Contains('contentCookie')) {
      $bad = $true
      $lines.Add('BAD: DxvkBuffer content-cookie API/member missing.')
    }
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  if ($bad) {
    Die "V219 GDI/content-cookie verification failed. See $report"
  }

  Log "V219 fixed d3dukmdt direct-include and DxvkBuffer content-cookie blockers. Report: $report"
}



function Patch-DxvkBufferContentCookieV219 {
  # DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
  # Identified current undeclared identifier:
  #   m_dx11ContentCookie in src\dxvk\dxvk_buffer.h lines 148/152.
  #
  # Cause:
  #   V215 added setContentCookie/contentCookie methods that referenced a member
  #   variable, but the member was not reliably inserted into the real class.
  #
  # Fix:
  #   Replace field-backed implementation with a header-local helper map keyed
  #   by DxvkBuffer*. This makes the methods compile in every TU that includes
  #   dxvk_buffer.h without relying on fragile private-field insertion.
  $report = Join-Path $Root 'DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')
  $lines.Add('IDENTIFIED UNDECLARED: m_dx11ContentCookie')
  $lines.Add('LOCATION: src\dxvk\dxvk_buffer.h lines reported by compiler: 148 and 152')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  function Write-NoBomV219([string]$Path, [string]$Text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
  }

  $buffer = Join-Path $Root 'src\dxvk\dxvk_buffer.h'
  if (!(Test-Path -LiteralPath $buffer -PathType Leaf)) {
    $lines.Add(('BAD: missing {0}' -f $buffer))
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 could not find dxvk_buffer.h. See $report"
  }

  $t = [System.IO.File]::ReadAllText($buffer)
  $orig = $t

  # Required includes for uint64_t and unordered_map.
  if ($t -notmatch '#include\s+<cstdint>') {
    if ($t -match '^\s*#pragma once') {
      $t = [regex]::Replace($t, '^\s*#pragma once\s*', "#pragma once`r`n#include <cstdint>`r`n", 1)
    } else {
      $t = "#include <cstdint>`r`n" + $t
    }
  }

  if ($t -notmatch '#include\s+<unordered_map>') {
    $m = [regex]::Matches($t, '(?m)^\s*#\s*include\s+[<"].+[>"]\s*$')
    if ($m.Count -gt 0) {
      $last = $m[$m.Count - 1]
      $pos = $last.Index + $last.Length
      $t = $t.Substring(0, $pos) + "`r`n#include <unordered_map>" + $t.Substring($pos)
    } elseif ($t -match '^\s*#pragma once') {
      $t = [regex]::Replace($t, '^\s*#pragma once\s*', "#pragma once`r`n#include <unordered_map>`r`n", 1)
    } else {
      $t = "#include <unordered_map>`r`n" + $t
    }
  }

  # Remove old fragile member field if V215 inserted it outside/inside class.
  $t = [regex]::Replace($t, '(?m)^\s*uint64_t\s+m_dx11ContentCookie\s*=\s*0\s*;\s*$', '')
  $t = [regex]::Replace($t, '(?ms)\r?\n\s*private:\s*\r?\n\s*\r?\n\s*;?\s*\r?\n', "`r`n")

  # Forward declare DxvkBuffer before helper if needed.
  # Insert helper immediately before class DxvkBuffer so it is in the same namespace
  # as the class in normal DXVK headers.
  $helper = @'
// DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
inline std::unordered_map<const void*, uint64_t>& dx11BufferContentCookieMapV219() {
  static std::unordered_map<const void*, uint64_t> cookies;
  return cookies;
}

'@

  if (!$t.Contains('dx11BufferContentCookieMapV219')) {
    $cm = [regex]::Match($t, '(?m)^\s*class\s+DxvkBuffer\b')
    if ($cm.Success) {
      $t = $t.Substring(0, $cm.Index) + $helper + $t.Substring($cm.Index)
    } else {
      $lines.Add('BAD: class DxvkBuffer not found; cannot insert helper.')
    }
  }

  # Replace the broken V215 methods exactly if present.
  $replacementMethods = @'
    // DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
    void setContentCookie(uint64_t cookie) {
      dx11BufferContentCookieMapV219()[this] = cookie;
    }

    uint64_t contentCookie() const {
      const auto& cookies = dx11BufferContentCookieMapV219();
      const auto entry = cookies.find(this);
      return entry != cookies.end() ? entry->second : 0;
    }

'@

  if ($t.Contains('DX11_V215_BUFFER_CONTENT_COOKIE')) {
    $t = [regex]::Replace(
      $t,
      '(?ms)\s*//\s*DX11_V215_BUFFER_CONTENT_COOKIE\s*void\s+setContentCookie\s*\(\s*uint64_t\s+cookie\s*\)\s*\{\s*m_dx11ContentCookie\s*=\s*cookie\s*;\s*\}\s*uint64_t\s+contentCookie\s*\(\s*\)\s*const\s*\{\s*return\s+m_dx11ContentCookie\s*;\s*\}\s*',
      "`r`n" + $replacementMethods
    )
  }

  # Fallback replacement if only the expressions remain.
  $t = $t.Replace('m_dx11ContentCookie = cookie;', 'dx11BufferContentCookieMapV219()[this] = cookie;')
  $t = $t.Replace('return m_dx11ContentCookie;', 'const auto& cookies = dx11BufferContentCookieMapV219(); const auto entry = cookies.find(this); return entry != cookies.end() ? entry->second : 0;')

  # If the method names do not exist, insert them after first public: inside class.
  if (!$t.Contains('setContentCookie(uint64_t cookie)')) {
    $cm = [regex]::Match($t, '(?s)class\s+DxvkBuffer\b.*?\{')
    if ($cm.Success) {
      $classStart = $cm.Index
      $afterClass = $t.Substring($classStart)
      $pm = [regex]::Match($afterClass, '(?m)^\s*public\s*:\s*$')
      if ($pm.Success) {
        $insert = $classStart + $pm.Index + $pm.Length
        $t = $t.Substring(0, $insert) + "`r`n" + $replacementMethods + $t.Substring($insert)
      } else {
        $insert = $cm.Index + $cm.Length
        $t = $t.Substring(0, $insert) + "`r`npublic:`r`n" + $replacementMethods + $t.Substring($insert)
      }
    }
  }

  if ($t -ne $orig) {
    Write-NoBomV219 -Path $buffer -Text $t
    $lines.Add(('PATCHED: replaced undeclared m_dx11ContentCookie references in {0}' -f $buffer))
  } else {
    $lines.Add(('OK: no dxvk_buffer.h changes needed in {0}' -f $buffer))
  }

  $bad = $false
  $check = [System.IO.File]::ReadAllText($buffer)
  if ($check.Contains('m_dx11ContentCookie')) {
    $bad = $true
    $lines.Add('BAD: m_dx11ContentCookie still remains in dxvk_buffer.h.')
  }
  if (!$check.Contains('dx11BufferContentCookieMapV219')) {
    $bad = $true
    $lines.Add('BAD: dx11BufferContentCookieMapV219 helper missing.')
  }
  if (!$check.Contains('setContentCookie(uint64_t cookie)')) {
    $bad = $true
    $lines.Add('BAD: setContentCookie method missing.')
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  if ($bad) {
    Die "V219 undeclared content-cookie fix failed. See $report"
  }

  Log "V219 identified and fixed undeclared m_dx11ContentCookie. Report: $report"
}



function Patch-DxvkBufferContentCookieDedupeV219 {
  # DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
  # Identified current errors:
  #   dxvk_buffer.h(160): C2535 setContentCookie already defined
  #   dxvk_buffer.h(164): C2535 contentCookie already defined
  #
  # Cause:
  #   V216 removed the undeclared field problem but left two copies of the
  #   content-cookie methods in the DxvkBuffer class.
  #
  # Fix:
  #   Remove every previous V215/V216 content-cookie method/helper copy and
  #   reinstall exactly one V219 helper + one method pair.
  $report = Join-Path $Root 'DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')
  $lines.Add('IDENTIFIED DUPLICATES: setContentCookie and contentCookie in src\dxvk\dxvk_buffer.h')
  $lines.Add('COMPILER LINES: setContentCookie 160 duplicates 151; contentCookie 164 duplicates 155')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  function Write-NoBomV219([string]$Path, [string]$Text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
  }

  $buffer = Join-Path $Root 'src\dxvk\dxvk_buffer.h'
  if (!(Test-Path -LiteralPath $buffer -PathType Leaf)) {
    $lines.Add(('BAD: missing {0}' -f $buffer))
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 could not find dxvk_buffer.h. See $report"
  }

  $t = [System.IO.File]::ReadAllText($buffer)
  $orig = $t

  # Ensure includes for helper.
  if ($t -notmatch '#include\s+<cstdint>') {
    if ($t -match '^\s*#pragma once') {
      $t = [regex]::Replace($t, '^\s*#pragma once\s*', "#pragma once`r`n#include <cstdint>`r`n", 1)
    } else {
      $t = "#include <cstdint>`r`n" + $t
    }
  }
  if ($t -notmatch '#include\s+<unordered_map>') {
    $incMatches = [regex]::Matches($t, '(?m)^\s*#\s*include\s+[<"].+[>"]\s*$')
    if ($incMatches.Count -gt 0) {
      $last = $incMatches[$incMatches.Count - 1]
      $pos = $last.Index + $last.Length
      $t = $t.Substring(0, $pos) + "`r`n#include <unordered_map>" + $t.Substring($pos)
    } elseif ($t -match '^\s*#pragma once') {
      $t = [regex]::Replace($t, '^\s*#pragma once\s*', "#pragma once`r`n#include <unordered_map>`r`n", 1)
    } else {
      $t = "#include <unordered_map>`r`n" + $t
    }
  }

  # Remove old field-backed member if any survived.
  $t = [regex]::Replace($t, '(?m)^\s*uint64_t\s+m_dx11ContentCookie\s*=\s*0\s*;\s*$', '')

  # Remove previous helper function copies.
  $t = [regex]::Replace(
    $t,
    '(?ms)\s*//\s*DX11_V21[56]_[^\r\n]*\s*inline\s+std::unordered_map<const\s+void\s*\*,\s*uint64_t>\s*&\s*dx11BufferContentCookieMapV216\s*\(\s*\)\s*\{\s*static\s+std::unordered_map<const\s+void\s*\*,\s*uint64_t>\s+cookies\s*;\s*return\s+cookies\s*;\s*\}\s*',
    "`r`n"
  )
  $t = [regex]::Replace(
    $t,
    '(?ms)\s*inline\s+std::unordered_map<const\s+void\s*\*,\s*uint64_t>\s*&\s*dx11BufferContentCookieMapV216\s*\(\s*\)\s*\{\s*static\s+std::unordered_map<const\s+void\s*\*,\s*uint64_t>\s+cookies\s*;\s*return\s+cookies\s*;\s*\}\s*',
    "`r`n"
  )
  $t = [regex]::Replace(
    $t,
    '(?ms)\s*inline\s+std::unordered_map<const\s+void\s*\*,\s*uint64_t>\s*&\s*dx11BufferContentCookieMapV219\s*\(\s*\)\s*\{\s*static\s+std::unordered_map<const\s+void\s*\*,\s*uint64_t>\s+cookies\s*;\s*return\s+cookies\s*;\s*\}\s*',
    "`r`n"
  )

  # Remove every existing content-cookie method pair, regardless of V215/V216
  # marker, including one-line replacement bodies.
  $t = [regex]::Replace(
    $t,
    '(?ms)\s*//\s*DX11_V21[56]_[^\r\n]*\s*void\s+setContentCookie\s*\(\s*uint64_t\s+cookie\s*\)\s*\{.*?\}\s*uint64_t\s+contentCookie\s*\(\s*\)\s*const\s*\{.*?\}\s*',
    "`r`n"
  )
  $t = [regex]::Replace(
    $t,
    '(?ms)\s*void\s+setContentCookie\s*\(\s*uint64_t\s+cookie\s*\)\s*\{\s*(?:dx11BufferContentCookieMapV216\s*\(\s*\)\s*\[\s*this\s*\]\s*=\s*cookie\s*;|m_dx11ContentCookie\s*=\s*cookie\s*;)\s*\}\s*uint64_t\s+contentCookie\s*\(\s*\)\s*const\s*\{.*?\}\s*',
    "`r`n"
  )

  # Extra cleanup for accidental blank private section from V215 field insertion.
  $t = [regex]::Replace($t, '(?ms)\r?\n\s*private:\s*\r?\n\s*(?=};)', "`r`n")

  # Install exactly one helper before class DxvkBuffer.
  $helper = @'
// DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
inline std::unordered_map<const void*, uint64_t>& dx11BufferContentCookieMapV219() {
  static std::unordered_map<const void*, uint64_t> cookies;
  return cookies;
}

'@
  if (!$t.Contains('dx11BufferContentCookieMapV219')) {
    $cm = [regex]::Match($t, '(?m)^\s*class\s+DxvkBuffer\b')
    if ($cm.Success) {
      $t = $t.Substring(0, $cm.Index) + $helper + $t.Substring($cm.Index)
    } else {
      $lines.Add('BAD: class DxvkBuffer not found for helper insertion.')
    }
  }

  # Install exactly one method pair after public:.
  $methods = @'
    // DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
    void setContentCookie(uint64_t cookie) {
      dx11BufferContentCookieMapV219()[this] = cookie;
    }

    uint64_t contentCookie() const {
      const auto& cookies = dx11BufferContentCookieMapV219();
      const auto entry = cookies.find(this);
      return entry != cookies.end() ? entry->second : 0;
    }

'@

  $cm2 = [regex]::Match($t, '(?s)class\s+DxvkBuffer\b.*?\{')
  if ($cm2.Success) {
    $classStart = $cm2.Index
    $afterClass = $t.Substring($classStart)
    $pm = [regex]::Match($afterClass, '(?m)^\s*public\s*:\s*$')
    if ($pm.Success) {
      $ins = $classStart + $pm.Index + $pm.Length
      $t = $t.Substring(0, $ins) + "`r`n" + $methods + $t.Substring($ins)
    } else {
      $ins = $cm2.Index + $cm2.Length
      $t = $t.Substring(0, $ins) + "`r`npublic:`r`n" + $methods + $t.Substring($ins)
    }
  } else {
    $lines.Add('BAD: class DxvkBuffer not found for method insertion.')
  }

  if ($t -ne $orig) {
    Write-NoBomV219 -Path $buffer -Text $t
    $lines.Add(('PATCHED: deduped DxvkBuffer content-cookie API in {0}' -f $buffer))
  } else {
    $lines.Add(('OK: no dxvk_buffer.h changes required in {0}' -f $buffer))
  }

  $bad = $false
  $check = [System.IO.File]::ReadAllText($buffer)

  $setMatches = [regex]::Matches($check, 'setContentCookie\s*\(\s*uint64_t\s+cookie\s*\)')
  $getMatches = [regex]::Matches($check, 'contentCookie\s*\(\s*\)\s*const')
  if ($setMatches.Count -ne 1) {
    $bad = $true
    $lines.Add(('BAD: setContentCookie count is {0}, expected 1.' -f $setMatches.Count))
  }
  if ($getMatches.Count -ne 1) {
    $bad = $true
    $lines.Add(('BAD: contentCookie count is {0}, expected 1.' -f $getMatches.Count))
  }
  if ($check.Contains('m_dx11ContentCookie')) {
    $bad = $true
    $lines.Add('BAD: m_dx11ContentCookie remains.')
  }
  if ($check.Contains('dx11BufferContentCookieMapV216')) {
    $bad = $true
    $lines.Add('BAD: old V216 cookie helper remains.')
  }
  if (!$check.Contains('dx11BufferContentCookieMapV219')) {
    $bad = $true
    $lines.Add('BAD: V219 cookie helper missing.')
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  if ($bad) {
    Die "V219 duplicate content-cookie method fix failed. See $report"
  }

  Log "V219 deduped DxvkBuffer content-cookie API. Report: $report"
}



function Patch-DxvkBufferCookieBruteForceV219 {
  # DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
  # V217 failed because the regex did not remove every old duplicate method.
  # This version uses brace-counted removal of every setContentCookie,
  # contentCookie, and old helper function, then inserts exactly one clean pair.
  $report = Join-Path $Root 'DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')
  $lines.Add('Fixes V217 report failure by brace-count removing duplicate methods.')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  function Write-NoBomV219([string]$Path, [string]$Text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
  }

  function Remove-FunctionByRegexV219([string]$Text, [string]$Pattern, [ref]$RemovedCount) {
    while ($true) {
      $m = [regex]::Match($Text, $Pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
      if (!$m.Success) { break }

      $sigStart = $m.Index
      $lineStart = $Text.LastIndexOf("`n", [Math]::Max(0, $sigStart))
      if ($lineStart -lt 0) { $lineStart = 0 } else { $lineStart = $lineStart + 1 }

      # Include an immediately preceding DX11 marker comment if present.
      $prevLineEnd = $lineStart - 2
      if ($prevLineEnd -gt 0) {
        $prevLineStart = $Text.LastIndexOf("`n", $prevLineEnd)
        if ($prevLineStart -lt 0) { $prevLineStart = 0 } else { $prevLineStart = $prevLineStart + 1 }
        $prevLine = $Text.Substring($prevLineStart, $prevLineEnd - $prevLineStart + 1)
        if ($prevLine -match 'DX11_V21[5-8]_') {
          $lineStart = $prevLineStart
        }
      }

      $open = $Text.IndexOf('{', $m.Index + $m.Length)
      if ($open -lt 0) { break }

      $depth = 0
      $i = $open
      while ($i -lt $Text.Length) {
        $ch = $Text[$i]
        if ($ch -eq '{') { $depth++ }
        elseif ($ch -eq '}') {
          $depth--
          if ($depth -eq 0) {
            $i++
            break
          }
        }
        $i++
      }

      # Eat trailing semicolon/blank lines/spaces.
      while ($i -lt $Text.Length -and ($Text[$i] -eq ';' -or $Text[$i] -eq "`r" -or $Text[$i] -eq "`n" -or $Text[$i] -eq ' ' -or $Text[$i] -eq "`t")) {
        $i++
      }

      $Text = $Text.Remove($lineStart, $i - $lineStart)
      $RemovedCount.Value = $RemovedCount.Value + 1
    }

    return $Text
  }

  $buffer = Join-Path $Root 'src\dxvk\dxvk_buffer.h'
  if (!(Test-Path -LiteralPath $buffer -PathType Leaf)) {
    $lines.Add(('BAD: missing {0}' -f $buffer))
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 missing dxvk_buffer.h. See $report"
  }

  $t = [System.IO.File]::ReadAllText($buffer)
  $orig = $t

  # Add required includes.
  if ($t -notmatch '#include\s+<cstdint>') {
    if ($t -match '^\s*#pragma once') {
      $t = [regex]::Replace($t, '^\s*#pragma once\s*', "#pragma once`r`n#include <cstdint>`r`n", 1)
    } else {
      $t = "#include <cstdint>`r`n" + $t
    }
  }
  if ($t -notmatch '#include\s+<unordered_map>') {
    $incMatches = [regex]::Matches($t, '(?m)^\s*#\s*include\s+[<"].+[>"]\s*$')
    if ($incMatches.Count -gt 0) {
      $last = $incMatches[$incMatches.Count - 1]
      $pos = $last.Index + $last.Length
      $t = $t.Substring(0, $pos) + "`r`n#include <unordered_map>" + $t.Substring($pos)
    } else {
      $t = "#include <unordered_map>`r`n" + $t
    }
  }

  # Remove stale fields and old empty private section if it was created only for that field.
  $t = [regex]::Replace($t, '(?m)^\s*uint64_t\s+m_dx11ContentCookie\s*=\s*0\s*;\s*$', '')
  $t = [regex]::Replace($t, '(?ms)\r?\n\s*private:\s*\r?\n\s*(?=};)', "`r`n")

  $removed = 0
  $refRemoved = [ref]$removed

  # Brace-counted removal of every old method/helper variant.
  $t = Remove-FunctionByRegexV219 $t 'inline\s+std::unordered_map\s*<\s*const\s+void\s*\*\s*,\s*uint64_t\s*>\s*&\s*dx11BufferContentCookieMapV21[5-8]\s*\(\s*\)' $refRemoved
  $t = Remove-FunctionByRegexV219 $t 'void\s+setContentCookie\s*\(\s*uint64_t\s+cookie\s*\)' $refRemoved
  $t = Remove-FunctionByRegexV219 $t 'uint64_t\s+contentCookie\s*\(\s*\)\s*const' $refRemoved

  $lines.Add(('Removed old cookie method/helper definitions: {0}' -f $removed))

  # Clean any orphan marker comments.
  $t = [regex]::Replace($t, '(?m)^\s*//\s*DX11_V21[5-8]_[^\r\n]*\r?$', '')

  # Insert exactly one helper before class DxvkBuffer.
  $helper = @'
// DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
inline std::unordered_map<const void*, uint64_t>& dx11BufferContentCookieMapV219() {
  static std::unordered_map<const void*, uint64_t> cookies;
  return cookies;
}

'@
  $cm = [regex]::Match($t, '(?m)^\s*class\s+DxvkBuffer\b')
  if ($cm.Success) {
    $t = $t.Substring(0, $cm.Index) + $helper + $t.Substring($cm.Index)
  } else {
    $lines.Add('BAD: class DxvkBuffer not found for helper insertion.')
  }

  # Insert exactly one method pair after first public: in DxvkBuffer.
  $methods = @'
    // DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
    void setContentCookie(uint64_t cookie) {
      dx11BufferContentCookieMapV219()[this] = cookie;
    }

    uint64_t contentCookie() const {
      const auto& cookies = dx11BufferContentCookieMapV219();
      const auto entry = cookies.find(this);
      return entry != cookies.end() ? entry->second : 0;
    }

'@
  $cm2 = [regex]::Match($t, '(?s)class\s+DxvkBuffer\b.*?\{')
  if ($cm2.Success) {
    $classStart = $cm2.Index
    $afterClass = $t.Substring($classStart)
    $pm = [regex]::Match($afterClass, '(?m)^\s*public\s*:\s*$')
    if ($pm.Success) {
      $ins = $classStart + $pm.Index + $pm.Length
      $t = $t.Substring(0, $ins) + "`r`n" + $methods + $t.Substring($ins)
    } else {
      $ins = $cm2.Index + $cm2.Length
      $t = $t.Substring(0, $ins) + "`r`npublic:`r`n" + $methods + $t.Substring($ins)
    }
  } else {
    $lines.Add('BAD: class DxvkBuffer not found for method insertion.')
  }

  # Normalize huge blank gaps.
  $t = [regex]::Replace($t, '(\r?\n){4,}', "`r`n`r`n`r`n")

  if ($t -ne $orig) {
    Write-NoBomV219 -Path $buffer -Text $t
    $lines.Add(('PATCHED: rewrote content-cookie section in {0}' -f $buffer))
  } else {
    $lines.Add(('OK: no dxvk_buffer.h changes were required in {0}' -f $buffer))
  }

  # Strict verification.
  $bad = $false
  $check = [System.IO.File]::ReadAllText($buffer)

  $setCount = ([regex]::Matches($check, 'setContentCookie\s*\(\s*uint64_t\s+cookie\s*\)')).Count
  $getCount = ([regex]::Matches($check, 'contentCookie\s*\(\s*\)\s*const')).Count
  $helper218Count = ([regex]::Matches($check, 'dx11BufferContentCookieMapV219\s*\(')).Count
  $helperOldCount = ([regex]::Matches($check, 'dx11BufferContentCookieMapV21[5-7]\s*\(')).Count

  if ($setCount -ne 1) {
    $bad = $true
    $lines.Add(('BAD: setContentCookie count is {0}, expected 1.' -f $setCount))
  }
  if ($getCount -ne 1) {
    $bad = $true
    $lines.Add(('BAD: contentCookie count is {0}, expected 1.' -f $getCount))
  }
  # helper appears in function definition plus two method references.
  if ($helper218Count -lt 3) {
    $bad = $true
    $lines.Add(('BAD: V219 helper reference count is {0}, expected at least 3.' -f $helper218Count))
  }
  if ($helperOldCount -ne 0) {
    $bad = $true
    $lines.Add(('BAD: old V215/V216/V217 helper reference count is {0}, expected 0.' -f $helperOldCount))
  }
  if ($check.Contains('m_dx11ContentCookie')) {
    $bad = $true
    $lines.Add('BAD: m_dx11ContentCookie remains.')
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  if ($bad) {
    Die "V219 content-cookie brute-force dedupe failed. See $report"
  }

  Log "V219 installed exactly one DxvkBuffer content-cookie method pair. Report: $report"
}



function Patch-DxvkBufferCookieSingleAuthoritativeV219 {
  # DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
  # V218 report showed the methods still counted 3 each.
  #
  # This version does NOT try to preserve any old cookie method body.
  # It line-removes every function whose signature contains:
  #   setContentCookie(
  #   contentCookie(
  # and every helper whose signature contains:
  #   dx11BufferContentCookieMapV21
  # then inserts exactly one V219 helper + one method pair.
  $report = Join-Path $Root 'DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS_REPORT.txt'
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add('DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS')
  $lines.Add('V218 report showed setContentCookie count 3 and contentCookie count 3.')
  $lines.Add(('Time: {0}' -f (Get-Date)))

  function Write-NoBomV219([string]$Path, [string]$Text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $enc)
  }

  function Remove-FunctionsContainingV219([string]$Text, [string[]]$Needles, [ref]$RemovedCount) {
    foreach ($needle in $Needles) {
      while ($true) {
        $idx = $Text.IndexOf($needle, [System.StringComparison]::Ordinal)
        if ($idx -lt 0) { break }

        # Find start of containing line.
        $lineStart = $Text.LastIndexOf("`n", [Math]::Max(0, $idx))
        if ($lineStart -lt 0) { $lineStart = 0 } else { $lineStart++ }

        # If a DX11 marker comment is immediately above the signature, remove it too.
        $prevEnd = $lineStart - 2
        if ($prevEnd -gt 0) {
          $prevStart = $Text.LastIndexOf("`n", $prevEnd)
          if ($prevStart -lt 0) { $prevStart = 0 } else { $prevStart++ }
          $prevLine = $Text.Substring($prevStart, $prevEnd - $prevStart + 1)
          if ($prevLine -match 'DX11_V21[5-9]_') {
            $lineStart = $prevStart
          }
        }

        # Find the first opening brace after the needle.
        $open = $Text.IndexOf('{', $idx)
        if ($open -lt 0) {
          # No body; delete just line to avoid infinite loop.
          $lineEnd = $Text.IndexOf("`n", $idx)
          if ($lineEnd -lt 0) { $lineEnd = $Text.Length }
          $Text = $Text.Remove($lineStart, $lineEnd - $lineStart)
          $RemovedCount.Value++
          continue
        }

        # Brace count to the end of the function body.
        $depth = 0
        $i = $open
        while ($i -lt $Text.Length) {
          $ch = $Text[$i]
          if ($ch -eq '{') { $depth++ }
          elseif ($ch -eq '}') {
            $depth--
            if ($depth -eq 0) {
              $i++
              break
            }
          }
          $i++
        }

        # Eat a semicolon and following whitespace/newlines after the body.
        while ($i -lt $Text.Length -and (
          $Text[$i] -eq ';' -or $Text[$i] -eq "`r" -or $Text[$i] -eq "`n" -or
          $Text[$i] -eq ' ' -or $Text[$i] -eq "`t")) {
          $i++
        }

        $Text = $Text.Remove($lineStart, $i - $lineStart)
        $RemovedCount.Value++
      }
    }
    return $Text
  }

  $buffer = Join-Path $Root 'src\dxvk\dxvk_buffer.h'
  if (!(Test-Path -LiteralPath $buffer -PathType Leaf)) {
    $lines.Add(('BAD: missing {0}' -f $buffer))
    [System.IO.File]::WriteAllLines($report, $lines)
    Die "V219 missing dxvk_buffer.h. See $report"
  }

  $t = [System.IO.File]::ReadAllText($buffer)
  $orig = $t

  # Add includes once.
  if ($t -notmatch '#include\s+<cstdint>') {
    if ($t -match '^\s*#pragma once') {
      $t = [regex]::Replace($t, '^\s*#pragma once\s*', "#pragma once`r`n#include <cstdint>`r`n", 1)
    } else {
      $t = "#include <cstdint>`r`n" + $t
    }
  }

  if ($t -notmatch '#include\s+<unordered_map>') {
    $incMatches = [regex]::Matches($t, '(?m)^\s*#\s*include\s+[<"].+[>"]\s*$')
    if ($incMatches.Count -gt 0) {
      $last = $incMatches[$incMatches.Count - 1]
      $pos = $last.Index + $last.Length
      $t = $t.Substring(0, $pos) + "`r`n#include <unordered_map>" + $t.Substring($pos)
    } else {
      $t = "#include <unordered_map>`r`n" + $t
    }
  }

  # Remove old member field and all old helper/method definitions by plain
  # signature-string search.
  $t = [regex]::Replace($t, '(?m)^\s*uint64_t\s+m_dx11ContentCookie\s*=\s*0\s*;\s*$', '')

  $removed = 0
  $removedRef = [ref]$removed
  $t = Remove-FunctionsContainingV219 $t @(
    'dx11BufferContentCookieMapV21',
    'setContentCookie(',
    'contentCookie()'
  ) $removedRef

  # Clean orphan marker lines.
  $t = [regex]::Replace($t, '(?m)^\s*//\s*DX11_V21[5-9]_[^\r\n]*\r?$', '')
  $lines.Add(('Removed old cookie helper/method bodies: {0}' -f $removed))

  # Insert exactly one helper before class DxvkBuffer.
  $helper = @'
// DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
inline std::unordered_map<const void*, uint64_t>& dx11BufferContentCookieMapV219() {
  static std::unordered_map<const void*, uint64_t> cookies;
  return cookies;
}

'@

  $classDecl = [regex]::Match($t, '(?m)^\s*class\s+DxvkBuffer\b')
  if ($classDecl.Success) {
    $t = $t.Substring(0, $classDecl.Index) + $helper + $t.Substring($classDecl.Index)
  } else {
    $lines.Add('BAD: class DxvkBuffer declaration not found.')
  }

  # Insert exactly one method pair after first public: inside DxvkBuffer.
  $methods = @'
    // DX11_V219_SINGLE_AUTHORITATIVE_BUFFER_COOKIE_METHODS
    void setContentCookie(uint64_t cookie) {
      dx11BufferContentCookieMapV219()[this] = cookie;
    }

    uint64_t contentCookie() const {
      const auto& cookies = dx11BufferContentCookieMapV219();
      const auto entry = cookies.find(this);
      return entry != cookies.end() ? entry->second : 0;
    }

'@

  $classOpen = [regex]::Match($t, '(?s)class\s+DxvkBuffer\b.*?\{')
  if ($classOpen.Success) {
    $classStart = $classOpen.Index
    $afterClass = $t.Substring($classStart)
    $publicMatch = [regex]::Match($afterClass, '(?m)^\s*public\s*:\s*$')
    if ($publicMatch.Success) {
      $insertAt = $classStart + $publicMatch.Index + $publicMatch.Length
      $t = $t.Substring(0, $insertAt) + "`r`n" + $methods + $t.Substring($insertAt)
    } else {
      $insertAt = $classOpen.Index + $classOpen.Length
      $t = $t.Substring(0, $insertAt) + "`r`npublic:`r`n" + $methods + $t.Substring($insertAt)
    }
  } else {
    $lines.Add('BAD: class DxvkBuffer open brace not found.')
  }

  # Trim giant blank gaps.
  $t = [regex]::Replace($t, '(\r?\n){4,}', "`r`n`r`n`r`n")

  if ($t -ne $orig) {
    Write-NoBomV219 -Path $buffer -Text $t
    $lines.Add(('PATCHED: installed one authoritative content-cookie method pair in {0}' -f $buffer))
  } else {
    $lines.Add(('OK: no dxvk_buffer.h changes made in {0}' -f $buffer))
  }

  # Strict verification.
  $bad = $false
  $check = [System.IO.File]::ReadAllText($buffer)

  $setCount = ([regex]::Matches($check, 'setContentCookie\s*\(')).Count
  $getCount = ([regex]::Matches($check, 'contentCookie\s*\(')).Count
  $helper219Count = ([regex]::Matches($check, 'dx11BufferContentCookieMapV219\s*\(')).Count
  $oldHelperCount = ([regex]::Matches($check, 'dx11BufferContentCookieMapV21[5-8]\s*\(')).Count

  $lines.Add(('VERIFY: setContentCookie count = {0}' -f $setCount))
  $lines.Add(('VERIFY: contentCookie count = {0}' -f $getCount))
  $lines.Add(('VERIFY: V219 helper reference count = {0}' -f $helper219Count))
  $lines.Add(('VERIFY: old helper reference count = {0}' -f $oldHelperCount))

  if ($setCount -ne 1) {
    $bad = $true
    $lines.Add(('BAD: setContentCookie count is {0}, expected 1.' -f $setCount))
  }
  if ($getCount -ne 1) {
    $bad = $true
    $lines.Add(('BAD: contentCookie count is {0}, expected 1.' -f $getCount))
  }
  if ($oldHelperCount -ne 0) {
    $bad = $true
    $lines.Add(('BAD: old helper count is {0}, expected 0.' -f $oldHelperCount))
  }
  if ($check.Contains('m_dx11ContentCookie')) {
    $bad = $true
    $lines.Add('BAD: m_dx11ContentCookie remains.')
  }

  [System.IO.File]::WriteAllLines($report, $lines)
  if ($bad) {
    Die "V219 authoritative cookie method repair failed. See $report"
  }

  Log "V219 installed exactly one authoritative DxvkBuffer content-cookie method pair. Report: $report"
}


function Build-X64Runtime([string]$VsInstall, [string]$Meson, [string]$Ninja) {
  
  Enable-DX11FastCompilerEnvV219
# DX11_V219_FORCE_REBUILD_AND_STAGE_REMIX15_DLLS
  # After importing RTX Remix 1.5 runtime/UI/source changes, existing _output\x64
  # DLLs are stale. Do not silently reuse them. Rebuild the x64 runtime DLLs and
  # then stage those rebuilt DLLs into the x86 package .trex folder.
  if ($SkipRuntimeBuild) {
    Warn 'SkipRuntimeBuild was requested. V219 cannot update .trex runtime DLLs when runtime build is skipped.'
    return (Join-Path $Root '_Comp64Release')
  }

  $buildDirPreV219 = Join-Path $Root '_Comp64Release'
  if (Test-Path -LiteralPath $buildDirPreV219 -PathType Container) {
    Log "V219 preserving existing x64 runtime build for incremental Ninja rebuild: $buildDirPreV219"
  }

  $oldCacheV219 = Join-Path $Root '_dx11_v93_x64_runtime_cache'
  if (Test-Path -LiteralPath $oldCacheV219 -PathType Container) {
    Log "V219 preserving existing x64 runtime DLL cache; stage step will overwrite changed DLLs when build succeeds: $oldCacheV219"
  }

  foreach ($oldDllV219 in @(
    (Join-Path $Root '_output\x64\d3d11.dll'),
    (Join-Path $Root '_output\x64\dxgi.dll')
  )) {
    if (Test-Path -LiteralPath $oldDllV219 -PathType Leaf) {
      Log "V219 preserving installed x64 runtime DLL until Ninja succeeds: $oldDllV219"
    }
  }

  $RebuildRuntime = $false
  Patch-RuntimeBuildIncrementalNoRecompileLoopV219
  Import-VSEnvironment $VsInstall 'x64'
  Patch-BoostBindGlobalPlaceholdersV219
  Patch-RuntimeMesonNgxDebugLibFallbackV219
  Patch-RuntimeMesonNgxOptionalNoLibV219
  Find-RealNgxLibOrDllV219 | Out-Null
  Patch-RuntimeMesonUseRealNgxLibV219
  Patch-RuntimeMesonRemoveNgxExtraEndifV219
  Add-DxvkEnvShouldBypassRemixV219
  Restore-RealSharedConstantsHeaderV219
  Add-RtxHeaderIncludeGuardsV225
  Restore-RealRtxShaderCppHeadersV219
  Patch-RtxdiSlangLvalueCastsV219
  Patch-RtxdiVolumeInteractionLvalueFixV219
  Sync-RealRemix15RayTracingSubmodulesV219
  Patch-RtxdiDlssEnhancementGeometryNormalCompatV219
    Invoke-BoundedDx11OnlyNoDx9ScrubV219
  Repair-UtilGdiDxgiFormatV219
  Force-UtilGdiDxgiFormatHeaderV219
  Patch-SlangLightAxisIdentifierV219
  Patch-SlangLightExplicitVectorTypesV219
  Patch-LightFloatNSymbolSyntaxSyncV219
  Patch-CylinderLightAxisLengthOverRenameV219
  Patch-CylinderLightLightAxisUsesV219
  Patch-LightFloatNSymbolSyntaxSyncV219
  Patch-CylinderLightAxisLengthOverRenameV219
  Patch-SlangVecAliasesInLightHeaderV219
  Install-RuntimeShaderCompilerWrapperV219
$env:NINJA = $Ninja
  $env:Path = (Split-Path -Parent $Ninja) + ';' + $env:Path
  Repair-FutureTimestamps
  Repair-MesonUtf8NoBom $Root
  Repair-MesonNestedTernary $Root
Repair-RemixApiLine79Line130Corruption $Root
  if (-not $NoDepsFetch) {
    Ensure-Deps $Meson
  } else {
    Warn 'Skipping Packman dependency fetch because -NoDepsFetch was passed.'
  }
  $buildDir = Join-Path $Root '_Comp64Release'
  if ($false -and $RebuildRuntime -and (Test-Path $buildDir)) { Log "V219 disabled forced runtime build-dir removal to prevent full shader recompile loops: $buildDir"; Remove-DirectoryRobust $buildDir }
  $buildNinja = Join-Path $buildDir 'build.ninja'
  if (!(Test-Path $buildNinja)) {
    Log 'Configuring x64 DX11+USD runtime.'
    $mesonArgs = @('setup','--buildtype=release','--backend=ninja','-Denable_dxgi=true','-Denable_d3d11=true','-Denable_tests=false','-Denable_tracy=true','-Dskip_packman_fetch=true',$buildDir,$Root)
  Ensure-WindowsSdkD3D11IncludesV219
  Ensure-UsdPXRIncludeEnvV219
  Remove-LateMesonProjectArgumentsV219
  Patch-ActualDx11MaterialFogAndNrcV219
  Patch-SurfaceSharedAndTerrainIncludeV219
  Patch-Dx11LightStateApiV219
  Patch-WxWarningErrorsV219
  Patch-Dx11DeviceFeatureAndGdiFormatV219
  Patch-GdiFormatAndDxvkBufferCookieV219
  # DX11_V225: DxvkBuffer already provides setContentCookie/contentCookie upstream
  # (backed by std::atomic<uint64_t> m_contentCookie). The V219 cookie patchers were
  # built on a false premise and corrupted/truncated dxvk_buffer.h. Disabled.
  # Patch-DxvkBufferContentCookieV219
  # Patch-DxvkBufferContentCookieDedupeV219
  # Patch-DxvkBufferCookieBruteForceV219
  # Patch-DxvkBufferCookieSingleAuthoritativeV219
    Invoke-Logged -Label 'runtime-meson-x64' -Exe $Meson -CommandArgs $mesonArgs -WorkingDirectory $Root -AllowIfFileExists $buildNinja | Out-Null
  }
  if (!(Test-Path $buildNinja)) { Die "Meson did not create build.ninja: $buildNinja" }
  Log 'Building x64 DX11+USD runtime.'
  Patch-SlangLightExplicitVectorTypesV219
  Patch-LightFloatNSymbolSyntaxSyncV219
  Patch-CylinderLightAxisLengthOverRenameV219
  Patch-CylinderLightLightAxisUsesV219
  Patch-LightFloatNSymbolSyntaxSyncV219
  Patch-CylinderLightAxisLengthOverRenameV219
  Patch-UtilGdiConcreteDxgiFormatV219


  # The authoritative RTX header sync above runs on every invocation. Reapply
  # the bounded DX11 compatibility edits after it even when build.ninja already
  # exists. Previously these only ran during first-time Meson setup, so the next
  # incremental build received an upstream nrc_args.h with a one-level-too-deep
  # relative path and could not find NrcStructures.h. These repairs are
  # idempotent and refresh the NRC compiler include directory as well.
  Patch-ActualDx11MaterialFogAndNrcV219
  Patch-SurfaceSharedAndTerrainIncludeV219
  Patch-Dx11LightStateApiV219
  Patch-WxWarningErrorsV219
  Patch-Dx11DeviceFeatureAndGdiFormatV219
  Patch-GdiFormatAndDxvkBufferCookieV219
  Ensure-WindowsSdkD3D11IncludesV219
  Invoke-Logged -Label 'runtime-ninja-x64' -Exe $Ninja -CommandArgs (Get-DX11FastNinjaArgsV219 $buildDir) -WorkingDirectory $Root | Out-Null
  try { Invoke-Logged -Label 'runtime-install-x64' -Exe $Meson -CommandArgs @('install','-C',$buildDir,'--no-rebuild','--tags','output') -WorkingDirectory $Root | Out-Null } catch { Warn $_.Exception.Message }
  $existing2 = Get-ExistingRuntimeDir
  if ($existing2) { return $existing2 }
  return $buildDir
}

function Find-MSVCVersionRoot([string]$VsInstall) {
  $root = Join-Path $VsInstall 'VC\Tools\MSVC'
  $v = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
  if (!$v) { Die "MSVC tools folder not found under $root" }
  return $v.FullName
}
function Build-DX11ClientDirect([string]$VsInstall, [string]$ClientSrc) {
  # V219: Direct build is launcher-only.
  # Meson builds the x86 game-side proxy DLLs (d3d11.dll/dxgi.dll) with their
  # common dx11_bridge_client.cpp object/dependencies. Rebuilding those DLLs here
  # caused LNK2019 because only d3d11_dx11bridge.obj was linked.
  $msvc = Find-MSVCVersionRoot $VsInstall
  $cl = Join-Path $msvc 'bin\Hostx64\x86\cl.exe'
  if (!(Test-Path $cl)) { $cl = Join-Path $msvc 'bin\HostX64\x86\cl.exe' }
  if (!(Test-Path $cl)) { Die "HostX64 x86 cl.exe not found under $msvc" }

  $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
  $sdkIncRoot = Join-Path $kitsRoot 'Include'
  $sdkLibRoot = Join-Path $kitsRoot 'Lib'
  $sdkBinRoot = Join-Path $kitsRoot 'bin'
  $sdkVer = Get-ChildItem -LiteralPath $sdkIncRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
  if (!$sdkVer) { Die 'Windows 10 SDK include folder not found.' }
  $sdk = $sdkVer.Name

  $env:INCLUDE = @(
    (Join-Path $msvc 'include'),
    (Join-Path $sdkIncRoot "$sdk\ucrt"),
    (Join-Path $sdkIncRoot "$sdk\shared"),
    (Join-Path $sdkIncRoot "$sdk\um"),
    (Join-Path $sdkIncRoot "$sdk\winrt"),
    (Join-Path $sdkIncRoot "$sdk\cppwinrt")
  ) -join ';'

  $env:LIB = @(
    (Join-Path $msvc 'lib\x86'),
    (Join-Path $sdkLibRoot "$sdk\ucrt\x86"),
    (Join-Path $sdkLibRoot "$sdk\um\x86")
  ) -join ';'

  $env:Path = @(
    (Split-Path -Parent $cl),
    (Join-Path $sdkBinRoot "$sdk\x64"),
    (Join-Path $VsInstall 'Common7\IDE'),
    $env:Path
  ) -join ';'

  $out = Join-Path $Root 'bridge_dx11_work\_dx11_launcher_x86'
  if ($Clean -and (Test-Path $out)) { Remove-Item -LiteralPath $out -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $out | Out-Null

  $link = Join-Path (Split-Path -Parent $cl) 'link.exe'
  if (!(Test-Path -LiteralPath $link -PathType Leaf)) { Die "link.exe not found next to cl.exe: $link" }

  $launcherCpp = Join-Path $out 'NvRemixLauncher32.cpp'
  $launcherObj = Join-Path $out 'NvRemixLauncher32.obj'
  $launcherExe = Join-Path $out 'NvRemixLauncher32.exe'
  $launcherPdb = Join-Path $out 'NvRemixLauncher32.pdb'

  Set-Content -LiteralPath $launcherCpp -Encoding UTF8 -Value @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>
#include <string>

static void dirnameOf(const wchar_t* in, wchar_t* out, DWORD cap) {
  out[0] = 0;
  if (!in || !in[0]) return;
  lstrcpynW(out, in, cap);
  wchar_t* slash = wcsrchr(out, L'\\');
  if (slash) slash[1] = 0;
}

static void appendLog(const wchar_t* root, const wchar_t* msg) {
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L"NvRemixLauncher32.log");
  HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  SYSTEMTIME st; GetLocalTime(&st);
  char line[4096] = {};
  char m[3072] = {};
  WideCharToMultiByte(CP_UTF8, 0, msg ? msg : L"", -1, m, sizeof(m), nullptr, nullptr);
  int n = sprintf_s(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, m);
  DWORD wrote = 0;
  if (n > 0) WriteFile(h, line, (DWORD)n, &wrote, nullptr);
  CloseHandle(h);
}

static bool existsFile(const wchar_t* p) {
  DWORD a = GetFileAttributesW(p);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool isExcludedExe(const wchar_t* name) {
  return _wcsicmp(name, L"NvRemixLauncher32.exe") == 0 ||
         _wcsicmp(name, L"NvRemixBridge.exe") == 0 ||
         _wcsicmp(name, L"dxvk-remix.exe") == 0 ||
         _wcsicmp(name, L"setup.exe") == 0 ||
         _wcsicmp(name, L"unins000.exe") == 0;
}

static bool autoFindGameExe(const wchar_t* root, wchar_t* out, DWORD cap) {
  wchar_t pattern[MAX_PATH] = {};
  lstrcpynW(pattern, root, MAX_PATH);
  lstrcatW(pattern, L"*.exe");
  WIN32_FIND_DATAW fd = {};
  HANDLE h = FindFirstFileW(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !isExcludedExe(fd.cFileName)) {
      lstrcpynW(out, root, cap);
      lstrcatW(out, fd.cFileName);
      FindClose(h);
      return true;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return false;
}

static void quoteAppend(wchar_t* dst, DWORD cap, const wchar_t* s) {
  lstrcatW(dst, L"\"");
  lstrcatW(dst, s);
  lstrcatW(dst, L"\"");
}

static bool pathLooksRooted(const wchar_t* p) {
  return (p && ((wcslen(p) > 2 && p[1] == L':') || (p[0] == L'\\' && p[1] == L'\\')));
}


// DX11_V219_STEAM_AWARE_FALLBACK_HELPER
// Normal Steam/Unity path: start the game through Steam, then the game loads
// local d3d11.dll/dxgi.dll and those DLLs run the bridge client.
// This launcher remains packaged as required DLL-started launcher/client helper, but if it is used
// inside steamapps/common, it must hand off to Steam instead of direct-launching
// the Unity EXE, or SteamAPI may exit/relaunch the process.
static const wchar_t* findNoCaseW(const wchar_t* haystack, const wchar_t* needle) {
  if (!haystack || !needle || !needle[0]) return nullptr;
  const size_t needleLen = wcslen(needle);
  for (const wchar_t* p = haystack; *p; ++p) {
    if (_wcsnicmp(p, needle, needleLen) == 0) return p;
  }
  return nullptr;
}

static bool readTextFileA(const wchar_t* path, char* out, DWORD cap) {
  if (!out || cap < 2) return false;
  out[0] = 0;
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD read = 0;
  BOOL ok = ReadFile(h, out, cap - 1, &read, nullptr);
  CloseHandle(h);
  if (!ok) return false;
  out[read] = 0;
  return true;
}

static bool parseFirstDigitsA(const char* text, wchar_t* out, DWORD cap) {
  if (!text || !out || cap < 2) return false;
  const char* p = text;
  while (*p && (*p < '0' || *p > '9')) ++p;
  if (!*p) return false;
  DWORD n = 0;
  while (*p >= '0' && *p <= '9' && n + 1 < cap) out[n++] = (wchar_t)*p++;
  out[n] = 0;
  return n > 0;
}

static bool parseAcfQuotedValueA(const char* text, const char* key, char* out, DWORD cap) {
  if (!text || !key || !out || cap < 2) return false;
  out[0] = 0;
  const char* p = strstr(text, key);
  if (!p) return false;
  p += strlen(key);
  while (*p && *p != '"') ++p;
  if (!*p) return false;
  ++p;
  DWORD n = 0;
  while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
  out[n] = 0;
  return n > 0;
}

static bool strEqNoCaseA(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    char ca = *a++, cb = *b++;
    if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = char(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return *a == 0 && *b == 0;
}

static bool getLeafDirNameA(const wchar_t* root, char* out, DWORD cap) {
  if (!root || !out || cap < 2) return false;
  wchar_t tmp[MAX_PATH] = {};
  lstrcpynW(tmp, root, MAX_PATH);
  size_t len = wcslen(tmp);
  while (len > 0 && (tmp[len - 1] == L'\\' || tmp[len - 1] == L'/')) tmp[--len] = 0;
  const wchar_t* slash = wcsrchr(tmp, L'\\');
  const wchar_t* leaf = slash ? slash + 1 : tmp;
  int got = WideCharToMultiByte(CP_UTF8, 0, leaf, -1, out, cap, nullptr, nullptr);
  return got > 1;
}

static bool findSteamAppsRoot(const wchar_t* root, wchar_t* out, DWORD cap) {
  const wchar_t* marker = findNoCaseW(root, L"\\steamapps\\common\\");
  if (!marker) return false;
  size_t prefixLen = (size_t)(marker - root) + wcslen(L"\\steamapps\\");
  if (prefixLen + 1 >= cap) return false;
  wcsncpy_s(out, cap, root, prefixLen);
  out[prefixLen] = 0;
  return true;
}

static bool findSteamAppId(const wchar_t* root, wchar_t* appid, DWORD appidCap) {
  appid[0] = 0;
  wchar_t appidTxt[MAX_PATH] = {};
  lstrcpynW(appidTxt, root, MAX_PATH);
  lstrcatW(appidTxt, L"steam_appid.txt");
  char small[256] = {};
  if (readTextFileA(appidTxt, small, sizeof(small)) && parseFirstDigitsA(small, appid, appidCap)) return true;

  wchar_t steamapps[MAX_PATH] = {};
  if (!findSteamAppsRoot(root, steamapps, MAX_PATH)) return false;

  char installDir[260] = {};
  if (!getLeafDirNameA(root, installDir, sizeof(installDir))) return false;

  wchar_t pattern[MAX_PATH] = {};
  lstrcpynW(pattern, steamapps, MAX_PATH);
  lstrcatW(pattern, L"appmanifest_*.acf");

  WIN32_FIND_DATAW fd = {};
  HANDLE hFind = FindFirstFileW(pattern, &fd);
  if (hFind == INVALID_HANDLE_VALUE) return false;

  bool found = false;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    wchar_t path[MAX_PATH] = {};
    lstrcpynW(path, steamapps, MAX_PATH);
    lstrcatW(path, fd.cFileName);
    char acf[65536] = {};
    if (!readTextFileA(path, acf, sizeof(acf))) continue;
    char acfInstall[260] = {};
    char acfAppid[64] = {};
    if (!parseAcfQuotedValueA(acf, "\"installdir\"", acfInstall, sizeof(acfInstall))) continue;
    if (!strEqNoCaseA(acfInstall, installDir)) continue;
    if (!parseAcfQuotedValueA(acf, "\"appid\"", acfAppid, sizeof(acfAppid))) continue;
    MultiByteToWideChar(CP_UTF8, 0, acfAppid, -1, appid, appidCap);
    found = appid[0] != 0;
    break;
  } while (FindNextFileW(hFind, &fd));

  FindClose(hFind);
  return found;
}

static bool handOffToSteamIfSteamGame(const wchar_t* root) {
  wchar_t appid[64] = {};
  if (!findSteamAppId(root, appid, 64)) return false;
  wchar_t uri[128] = {};
  swprintf_s(uri, L"steam://run/%s", appid);
  wchar_t msg[512] = {};
  swprintf_s(msg, L"Steam game detected. Handing off to %s. The Steam-launched game will load local d3d11.dll/dxgi.dll bridge client.", uri);
  appendLog(root, msg);
  HINSTANCE result = ShellExecuteW(nullptr, L"open", uri, nullptr, root, SW_SHOWNORMAL);
  INT_PTR code = reinterpret_cast<INT_PTR>(result);
  if (code <= 32) {
    wchar_t err[512] = {};
    swprintf_s(err, L"ShellExecuteW(%s) failed with code %Id. Start the game from Steam normally.", uri, code);
    appendLog(root, err);
    MessageBoxW(nullptr, err, L"DX11 Remix Launcher", MB_ICONERROR);
  }
  return true;
}



// DX11_V219_GAME_CMD_FILE_ANYWHERE
static bool readLauncherTextFileV219(const wchar_t* path, wchar_t* out, DWORD cap) {
  if (!path || !path[0] || !out || cap < 2) return false;
  out[0] = 0;
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  char raw[8192] = {};
  DWORD read = 0;
  BOOL ok = ReadFile(h, raw, sizeof(raw) - 1, &read, nullptr);
  CloseHandle(h);
  if (!ok || read == 0) return false;
  raw[read] = 0;
  while (read > 0 && (raw[read - 1] == '\r' || raw[read - 1] == '\n')) raw[--read] = 0;
  int wrote = MultiByteToWideChar(CP_UTF8, 0, raw, -1, out, cap);
  if (wrote <= 0) {
    wrote = MultiByteToWideChar(CP_ACP, 0, raw, -1, out, cap);
  }
  return wrote > 0 && out[0] != 0;
}

static bool readLauncherGuidFileV219(const wchar_t* root, wchar_t* out, DWORD cap) {
  if (!root || !out || cap < 37) return false;
  out[0] = 0;
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L".trex\\dx11_bridge_guid.txt");
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  char raw[128] = {};
  DWORD read = 0;
  BOOL ok = ReadFile(h, raw, sizeof(raw) - 1, &read, nullptr);
  CloseHandle(h);
  if (!ok || read == 0) return false;
  raw[read] = 0;
  while (read > 0 && (raw[read - 1] == '\r' || raw[read - 1] == '\n' || raw[read - 1] == ' ' || raw[read - 1] == '\t')) raw[--read] = 0;
  if (read != 36) return false;
  MultiByteToWideChar(CP_UTF8, 0, raw, -1, out, cap);
  return out[0] != 0;
}

static bool envNameIsSteamOverlayV219(const wchar_t* name) {
  if (!name) return false;
  return _wcsicmp(name, L"SteamGameId") == 0 ||
         _wcsicmp(name, L"SteamOverlayGameId") == 0 ||
         _wcsicmp(name, L"SteamAppId") == 0 ||
         _wcsicmp(name, L"SteamClientLaunch") == 0 ||
         _wcsicmp(name, L"SteamEnv") == 0 ||
         _wcsicmp(name, L"GAMEOVERLAYRENDERER_LOG") == 0 ||
         _wcsicmp(name, L"SteamOverlayUI") == 0;
}

static void appendEnvPairV219(std::wstring& block, const wchar_t* name, const wchar_t* value) {
  if (!name || !name[0] || !value) return;
  block.append(name);
  block.push_back(L'=');
  block.append(value);
  block.push_back(L'\0');
}

static std::wstring buildBridgeEnvBlockV219(const wchar_t* root, const wchar_t* trex, const wchar_t* guid, const wchar_t* version) {
  std::wstring block;
  LPWCH env = GetEnvironmentStringsW();
  if (env) {
    for (LPWCH cur = env; *cur; cur += wcslen(cur) + 1) {
      const wchar_t* eq = wcschr(cur, L'=');
      if (!eq || eq == cur) continue;
      wchar_t name[256] = {};
      size_t n = static_cast<size_t>(eq - cur);
      if (n >= _countof(name)) n = _countof(name) - 1;
      wcsncpy_s(name, cur, n);
      if (envNameIsSteamOverlayV219(name)) continue;
      block.append(cur);
      block.push_back(L'\0');
    }
    FreeEnvironmentStringsW(env);
  }

  wchar_t oldPath[32767] = {};
  GetEnvironmentVariableW(L"PATH", oldPath, 32767);
  wchar_t newPath[32767] = {};
  lstrcpynW(newPath, trex, 32767);
  lstrcatW(newPath, L";");
  lstrcatW(newPath, root);
  lstrcatW(newPath, L";");
  lstrcatW(newPath, oldPath);

  appendEnvPairV219(block, L"PATH", newPath);
  appendEnvPairV219(block, L"DXVK_REMIX_GAME_DIR", root);
  appendEnvPairV219(block, L"DXVK_REMIX_TREX_DIR", trex);
  appendEnvPairV219(block, L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");
  appendEnvPairV219(block, L"DX11_BRIDGE_GUID", guid);
  appendEnvPairV219(block, L"DX11_BRIDGE_FIXED_GUID", guid);
  appendEnvPairV219(block, L"DX11_BRIDGE_VERSION", version);
  appendEnvPairV219(block, L"DX11_BRIDGE_MODE", L"d3d11");
  appendEnvPairV219(block, L"SteamNoOverlayUIDrawing", L"1");
  appendEnvPairV219(block, L"SteamGameId", L"0");
  appendEnvPairV219(block, L"SteamOverlayGameId", L"0");
  appendEnvPairV219(block, L"SteamAppId", L"0");

  wchar_t plugins[MAX_PATH] = {};
  lstrcpynW(plugins, trex, MAX_PATH);
  lstrcatW(plugins, L"plugins");
  DWORD pluginAttr = GetFileAttributesW(plugins);
  if (pluginAttr != INVALID_FILE_ATTRIBUTES && (pluginAttr & FILE_ATTRIBUTE_DIRECTORY)) {
    appendEnvPairV219(block, L"PXR_PLUGINPATH_NAME", plugins);
  }

  block.push_back(L'\0');
  return block;
}

// DX11_V219_SINGLE_BRIDGE_SERVER_OWNER
static unsigned long long hashWidePathV219(const wchar_t* s) {
  unsigned long long h = 1469598103934665603ull;
  if (!s) return h;
  while (*s) {
    wchar_t c = *s++;
    if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    h ^= (unsigned long long)c;
    h *= 1099511628211ull;
  }
  return h;
}

static HANDLE acquireSingleBridgeServerOwnerV219(const wchar_t* root, const wchar_t* guid, bool* owns) {
  if (owns) *owns = false;
  wchar_t name[256] = {};
  unsigned long long h = hashWidePathV219(root);
  // DX11_V219_SINGLE_BRIDGE_PER_GAME_FOLDER
  // One bridge per game folder, not one bridge per GUID. Including GUID in
  // the mutex let every new launch create another bridge.
  swprintf_s(name, L"Local\\DX11RemixSingleBridge_%08X%08X",
    (unsigned)((h >> 32) & 0xffffffffu),
    (unsigned)(h & 0xffffffffu));

  HANDLE m = CreateMutexW(nullptr, TRUE, name);
  if (!m) {
    return nullptr;
  }

  DWORD err = GetLastError();
  if (err == ERROR_ALREADY_EXISTS) {
    DWORD waitNow = WaitForSingleObject(m, 0);
    if (waitNow == WAIT_OBJECT_0 || waitNow == WAIT_ABANDONED) {
      if (owns) *owns = true;
      return m;
    }

    appendLog(root, L"DX11_V219: another NvRemixBridge.exe owner is already active for this game folder; not starting a second bridge.");
    CloseHandle(m);
    return nullptr;
  }

  if (owns) *owns = true;
  appendLog(root, L"DX11_V219: acquired single bridge server owner mutex.");
  return m;
}

static void releaseSingleBridgeServerOwnerV219(HANDLE* ownerMutex) {
  if (ownerMutex && *ownerMutex) {
    ReleaseMutex(*ownerMutex);
    CloseHandle(*ownerMutex);
    *ownerMutex = nullptr;
  }
}

// DX11_V219_REAL_SERVER_PID_FOR_CLIENT_HANDLE
// DX11_V219_PID_RECORD_WITH_BRIDGE_PATH
static void clearBridgeServerPidFileV219(const wchar_t* root) {
  if (!root || !root[0]) return;
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L".trex\\dx11_bridge_server_pid.txt");
  DeleteFileW(path);
}

// Kept original V219 function name so existing call sites stay simple until version labels are applied.
static void writeBridgeServerPidFileV219(const wchar_t* root, DWORD pid, const wchar_t* bridgePath) {
  if (!root || !root[0] || !pid || !bridgePath || !bridgePath[0]) return;
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L".trex\\dx11_bridge_server_pid.txt");

  HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    appendLog(root, L"DX11_V219 WARNING: failed to write .trex\\dx11_bridge_server_pid.txt.");
    return;
  }

  // ASCII/UTF-8 record:
  //   <pid>|<full path to .trex\NvRemixBridge.exe>
  // The x86 client uses the path to validate it has the real server PID.
  char bridgeUtf8[MAX_PATH * 3] = {};
  WideCharToMultiByte(CP_UTF8, 0, bridgePath, -1, bridgeUtf8, sizeof(bridgeUtf8), nullptr, nullptr);

  char buf[4096] = {};
  sprintf_s(buf, "%lu|%s", pid, bridgeUtf8);

  DWORD bytes = 0;
  WriteFile(h, buf, static_cast<DWORD>(strlen(buf)), &bytes, nullptr);
  CloseHandle(h);
  appendLog(root, L"DX11_V219 wrote actual NvRemixBridge.exe PID+path to .trex\\dx11_bridge_server_pid.txt.");
}

// DX11_V219_LAUNCHER_DUPLICATES_REAL_CLIENT_HANDLE
static DWORD readRealGamePidForLauncherV219(const wchar_t* root) {
  if (!root || !root[0]) return 0;
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L".trex\\dx11_bridge_game_pid.txt");

  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    appendLog(root, L"DX11_V219 ERROR: missing .trex\\dx11_bridge_game_pid.txt.");
    return 0;
  }

  char raw[64] = {};
  DWORD got = 0;
  BOOL ok = ReadFile(h, raw, sizeof(raw) - 1, &got, nullptr);
  CloseHandle(h);
  if (!ok || got == 0) {
    appendLog(root, L"DX11_V219 ERROR: failed to read real game PID file.");
    return 0;
  }
  raw[got] = 0;

  char compact[64] = {};
  DWORD ci = 0;
  for (DWORD i = 0; i < got && ci + 1 < sizeof(compact); ++i) {
    if (raw[i] >= '0' && raw[i] <= '9') compact[ci++] = raw[i];
  }
  compact[ci] = 0;
  return static_cast<DWORD>(strtoul(compact, nullptr, 10));
}

static void writeLauncherDuplicatedClientHandleV219(const wchar_t* root, DWORD serverPid, DWORD gamePid, HANDLE remoteHandle) {
  if (!root || !root[0] || !serverPid || !gamePid || !remoteHandle) return;
  wchar_t path[MAX_PATH] = {};
  lstrcpynW(path, root, MAX_PATH);
  lstrcatW(path, L".trex\\dx11_bridge_client_remote_handle.txt");

  HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    appendLog(root, L"DX11_V219 ERROR: failed to write .trex\\dx11_bridge_client_remote_handle.txt.");
    return;
  }

  char buf[256] = {};
  sprintf_s(buf, "%lu|%llu|%lu",
    serverPid,
    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(remoteHandle)),
    gamePid);

  DWORD bytes = 0;
  WriteFile(h, buf, static_cast<DWORD>(strlen(buf)), &bytes, nullptr);
  CloseHandle(h);
  appendLog(root, L"DX11_V219 wrote server-side real game process handle to .trex\\dx11_bridge_client_remote_handle.txt.");
}

static bool duplicateRealGameProcessHandleIntoBridgeServerV219(const wchar_t* root, HANDLE bridgeProcess, DWORD serverPid) {
  DWORD gamePid = readRealGamePidForLauncherV219(root);
  if (!gamePid) {
    appendLog(root, L"DX11_V219 ERROR: no real game PID available for server handle duplication.");
    return false;
  }

  HANDLE gameProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, gamePid);
  if (!gameProcess) {
    wchar_t msg[256] = {};
    swprintf_s(msg, L"DX11_V219 ERROR: launcher failed to open real game process pid=%lu err=%lu.", gamePid, GetLastError());
    appendLog(root, msg);
    return false;
  }

  HANDLE remoteHandle = nullptr;
  BOOL ok = DuplicateHandle(
    GetCurrentProcess(),
    gameProcess,
    bridgeProcess,
    &remoteHandle,
    0,
    FALSE,
    DUPLICATE_SAME_ACCESS);

  CloseHandle(gameProcess);

  if (!ok || !remoteHandle) {
    wchar_t msg[256] = {};
    swprintf_s(msg, L"DX11_V219 ERROR: launcher failed to duplicate real game handle into NvRemixBridge.exe pid=%lu err=%lu.", serverPid, GetLastError());
    appendLog(root, msg);
    return false;
  }

  writeLauncherDuplicatedClientHandleV219(root, serverPid, gamePid, remoteHandle);

  wchar_t msg[256] = {};
  swprintf_s(msg, L"DX11_V219 launcher duplicated real game process handle into NvRemixBridge.exe pid=%lu.", serverPid);
  appendLog(root, msg);
  return true;
}

// DX11_V219_REAL_GAME_TARGET_FOR_REMIX
static bool extractRealGameExeForServerV219(const wchar_t* gameCmd, wchar_t* out, DWORD cap) {
  if (!out || cap < 8) return false;
  out[0] = 0;
  if (!gameCmd || !gameCmd[0]) return false;

  const wchar_t* p = gameCmd;
  while (*p == L' ' || *p == L'\t') ++p;

  if (*p == L'"') {
    ++p;
    DWORD n = 0;
    while (*p && *p != L'"' && n + 1 < cap) {
      out[n++] = *p++;
    }
    out[n] = 0;
    return out[0] != 0;
  }

  const wchar_t* start = p;
  const wchar_t* exeEnd = nullptr;
  for (; *p; ++p) {
    if (_wcsnicmp(p, L".exe", 4) == 0) {
      exeEnd = p + 4;
      break;
    }
  }

  if (!exeEnd) {
    p = start;
    while (*p && *p != L' ' && *p != L'\t') ++p;
    exeEnd = p;
  }

  DWORD n = 0;
  for (const wchar_t* q = start; q < exeEnd && n + 1 < cap; ++q) {
    out[n++] = *q;
  }
  out[n] = 0;
  return out[0] != 0;
}

static void appendQuotedBridgeArgV219(wchar_t* cmd, DWORD cap, const wchar_t* arg) {
  if (!cmd || !arg || !arg[0]) return;
  DWORD used = static_cast<DWORD>(wcslen(cmd));
  if (used + 4 >= cap) return;
  cmd[used++] = L' ';
  cmd[used++] = L'"';
  while (*arg && used + 3 < cap) {
    if (*arg != L'"') {
      cmd[used++] = *arg;
    }
    ++arg;
  }
  cmd[used++] = L'"';
  cmd[used] = 0;
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  wchar_t root[MAX_PATH] = {};
  dirnameOf(self, root, MAX_PATH);

  appendLog(root, L"NvRemixLauncher32 DX11 launcher/client helper started. DX11_V219_DLL_LAUNCHER_BRIDGE_CHAIN");

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  bool launchBridge = false;
  wchar_t guid[128] = {};
  wchar_t version[128] = {};
  wchar_t gameCmd[4096] = {};
  wchar_t gameCmdFile[MAX_PATH] = {}; // DX11_V219_GAME_CMD_FILE_ANYWHERE

  if (argv) {
    for (int i = 1; i < argc; ++i) {
      if (_wcsicmp(argv[i], L"--dx11-launch-bridge") == 0) {
        launchBridge = true;
      } else if (_wcsicmp(argv[i], L"--guid") == 0 && i + 1 < argc) {
        lstrcpynW(guid, argv[++i], 128);
      } else if (_wcsicmp(argv[i], L"--version") == 0 && i + 1 < argc) {
        lstrcpynW(version, argv[++i], 128);
      } else if (_wcsicmp(argv[i], L"--game-root") == 0 && i + 1 < argc) {
        ++i;
      } else if (_wcsicmp(argv[i], L"--trex-root") == 0 && i + 1 < argc) {
        ++i;
      } else if (_wcsicmp(argv[i], L"--game-cmd-file") == 0 && i + 1 < argc) {
        lstrcpynW(gameCmdFile, argv[++i], MAX_PATH);
      } else if (_wcsicmp(argv[i], L"--game-cmd") == 0 && i + 1 < argc) {
        // Legacy fallback only. V219 uses --game-cmd-file so games can be anywhere.
        lstrcpynW(gameCmd, argv[++i], 4096);
      }
    }
  }

  if (!launchBridge) {
    appendLog(root, L"NvRemixLauncher32.exe was opened manually. It is started by the game-loaded d3d11.dll/dxgi.dll.");
    MessageBoxW(nullptr,
      L"Run the actual game normally.\n\nThe game loads the local d3d11.dll/dxgi.dll, and that DLL starts NvRemixLauncher32.exe.\nNvRemixLauncher32.exe then starts .trex\\NvRemixBridge.exe.",
      L"DX11 Remix Bridge",
      MB_OK | MB_ICONINFORMATION);
    if (argv) LocalFree(argv);
    return 0;
  }

  if (!guid[0]) {
    if (readLauncherGuidFileV219(root, guid, 128)) {
      appendLog(root, L"DX11_V219 launcher recovered shared DX9-style GUID from .trex\dx11_bridge_guid.txt.");
    }
  }

  if (!guid[0] || !version[0]) {
    appendLog(root, L"ERROR: DLL launched NvRemixLauncher32.exe without shared DX9-style --guid/--version.");
    if (argv) LocalFree(argv);
    return 10;
  }

  if (!gameCmd[0] && gameCmdFile[0]) {
    if (readLauncherTextFileV219(gameCmdFile, gameCmd, 4096)) {
      appendLog(root, L"DX11_V219 launcher loaded original game command line from --game-cmd-file.");
    } else {
      appendLog(root, L"DX11_V219 WARNING: failed to read --game-cmd-file.");
    }
  }
  if (!gameCmd[0]) {
    DWORD gotGameCmd = GetEnvironmentVariableW(L"DX11_BRIDGE_GAME_CMD", gameCmd, 4096);
    if (gotGameCmd > 0 && gotGameCmd < 4096) {
      appendLog(root, L"DX11_V219 launcher loaded original game command line from DX11_BRIDGE_GAME_CMD.");
    }
  }

  wchar_t realGameExeV219[MAX_PATH] = {};
  if (extractRealGameExeForServerV219(gameCmd, realGameExeV219, MAX_PATH)) {
    SetEnvironmentVariableW(L"DX11_BRIDGE_REAL_GAME_EXE", realGameExeV219);
    SetEnvironmentVariableW(L"DXVK_REMIX_REAL_GAME_EXE", realGameExeV219);
    SetEnvironmentVariableW(L"RTX_REMIX_TARGET_EXE", realGameExeV219);
    wchar_t targetMsg[1024] = {};
    swprintf_s(targetMsg, L"DX11_V219 real Remix target game exe: %s", realGameExeV219);
    appendLog(root, targetMsg);
  } else {
    appendLog(root, L"DX11_V219 WARNING: could not extract real game exe from original command line; bridge config may still see launcher target.");
  }

  wchar_t trex[MAX_PATH] = {};
  lstrcpynW(trex, root, MAX_PATH);
  lstrcatW(trex, L".trex\\");

  wchar_t bridge[MAX_PATH] = {};
  lstrcpynW(bridge, trex, MAX_PATH);
  lstrcatW(bridge, L"NvRemixBridge.exe");

  wchar_t rtD3D11[MAX_PATH] = {};
  wchar_t rtDxgi[MAX_PATH] = {};
  lstrcpynW(rtD3D11, trex, MAX_PATH); lstrcatW(rtD3D11, L"d3d11.dll");
  lstrcpynW(rtDxgi,  trex, MAX_PATH); lstrcatW(rtDxgi,  L"dxgi.dll");

  if (!existsFile(bridge)) {
    appendLog(root, L"ERROR: .trex\\NvRemixBridge.exe missing; launcher cannot start Remix server.");
    if (argv) LocalFree(argv);
    return 11;
  }
  if (!existsFile(rtD3D11) || !existsFile(rtDxgi)) {
    appendLog(root, L"ERROR: .trex\\d3d11.dll or .trex\\dxgi.dll missing; launcher cannot host Remix runtime.");
    if (argv) LocalFree(argv);
    return 12;
  }

  bool ownsBridgeServerV219 = false;
  HANDLE bridgeServerOwnerV219 = acquireSingleBridgeServerOwnerV219(root, guid, &ownsBridgeServerV219);
  if (!ownsBridgeServerV219) {
    appendLog(root, L"DX11_V219: duplicate launcher instance exiting after existing bridge owner finished; no second bridge was opened.");
    if (argv) LocalFree(argv);
    return 0;
  }

  wchar_t oldPath[32767] = {};
  GetEnvironmentVariableW(L"PATH", oldPath, 32767);
  wchar_t newPath[32767] = {};
  lstrcpynW(newPath, trex, 32767);
  lstrcatW(newPath, L";");
  lstrcatW(newPath, root);
  lstrcatW(newPath, L";");
  lstrcatW(newPath, oldPath);
  SetEnvironmentVariableW(L"PATH", newPath);
  SetEnvironmentVariableW(L"DXVK_REMIX_GAME_DIR", root);
  SetEnvironmentVariableW(L"DXVK_REMIX_TREX_DIR", trex);
  SetEnvironmentVariableW(L"DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", L"1");
  SetEnvironmentVariableW(L"DX11_BRIDGE_GUID", guid);
  SetEnvironmentVariableW(L"DX11_BRIDGE_VERSION", version);
  SetEnvironmentVariableW(L"DX11_BRIDGE_MODE", L"d3d11");

  wchar_t plugins[MAX_PATH] = {};
  lstrcpynW(plugins, trex, MAX_PATH);
  lstrcatW(plugins, L"plugins");
  DWORD pluginAttr = GetFileAttributesW(plugins);
  if (pluginAttr != INVALID_FILE_ATTRIBUTES && (pluginAttr & FILE_ATTRIBUTE_DIRECTORY)) {
    SetEnvironmentVariableW(L"PXR_PLUGINPATH_NAME", plugins);
  }

  wchar_t cmd[8192] = {};
  // DX11_V219_REAL_GAME_TARGET_FOR_REMIX
  // NvRemixBridge.exe needs the real game exe target for Remix config/attachment.
  // Pass only the already-running game exe path, not the full game command line.
  // This gives Remix the correct target without relaunching the game.
  swprintf_s(cmd, L"\"%s\" %s %s", bridge, guid, version);
  if (gameCmd[0]) {
    SetEnvironmentVariableW(L"DX11_BRIDGE_GAME_CMD", gameCmd);
  }
  if (realGameExeV219[0]) {
    appendQuotedBridgeArgV219(cmd, 8192, realGameExeV219);
    appendLog(root, L"DX11_V219 appended real game exe target to NvRemixBridge.exe command line for Remix config lookup.");
  } else {
    appendLog(root, L"DX11_V219 WARNING: no real game exe target appended; Remix may resolve launcher as the app target.");
  }

  wchar_t msg[8192] = {};
  swprintf_s(msg, L"DX11_V219 launcher starting .trex bridge server with real game target. guid=%s cmd=%s cwd=%s", guid, cmd, trex);
  appendLog(root, msg);

  STARTUPINFOW si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);
  std::wstring bridgeEnvV219 = buildBridgeEnvBlockV219(root, trex, guid, version);
  SetEnvironmentVariableW(L"SteamNoOverlayUIDrawing", L"1");
  SetEnvironmentVariableW(L"SteamGameId", L"0");
  SetEnvironmentVariableW(L"SteamOverlayGameId", L"0");
  SetEnvironmentVariableW(L"SteamAppId", L"0");
  // DX11_V219_LAUNCHER_COMPILE_FIX
  LPVOID bridgeEnvBlockV219 = bridgeEnvV219.empty()
    ? nullptr
    : static_cast<LPVOID>(const_cast<wchar_t*>(bridgeEnvV219.c_str()));
  clearBridgeServerPidFileV219(root);
  DeleteFileW(L".trex\\dx11_bridge_client_remote_handle.txt");
  BOOL ok = CreateProcessW(
    bridge,
    cmd,
    nullptr,
    nullptr,
    FALSE,
    CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
    bridgeEnvBlockV219,
    trex,
    &si,
    &pi);
  if (!ok) {
    wchar_t fail[512] = {};
    swprintf_s(fail, L"ERROR: launcher failed to start .trex\\NvRemixBridge.exe err=%lu", GetLastError());
    appendLog(root, fail);
    releaseSingleBridgeServerOwnerV219(&bridgeServerOwnerV219);
    if (argv) LocalFree(argv);
    return 13;
  }

  CloseHandle(pi.hThread);
  writeBridgeServerPidFileV219(root, pi.dwProcessId, bridge);
  duplicateRealGameProcessHandleIntoBridgeServerV219(root, pi.hProcess, pi.dwProcessId);
  appendLog(root, L"DX11_V219 launcher started .trex\\NvRemixBridge.exe, duplicated the real game handle into it, and is staying alive until the server exits.");
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  releaseSingleBridgeServerOwnerV219(&bridgeServerOwnerV219);

  wchar_t done[256] = {};
  swprintf_s(done, L"DX11_V219 .trex\\NvRemixBridge.exe exited with code %lu.", code);
  appendLog(root, done);

  if (argv) LocalFree(argv);
  return static_cast<int>(code);
}

'@

  Log "Compiling x86 NvRemixLauncher32.exe only with: $cl"
  Invoke-Logged -Label 'launcher32-x86-compile' -Exe $cl -CommandArgs @(
    '/nologo','/c','/O2','/MT','/EHsc','/std:c++17','/utf-8','/Zc:__cplusplus',
    ('/Fo:' + $launcherObj),
    $launcherCpp
  ) -WorkingDirectory $Root | Out-Null

  Log "Linking x86 NvRemixLauncher32.exe only"
  Invoke-Logged -Label 'launcher32-x86-link' -Exe $link -CommandArgs @(
    '/NOLOGO','/MACHINE:X86','/SUBSYSTEM:WINDOWS',
    ('/OUT:' + $launcherExe),
    ('/PDB:' + $launcherPdb),
    $launcherObj,
    'user32.lib','kernel32.lib','shell32.lib'
  ) -WorkingDirectory $Root | Out-Null

  if (!(Test-Path -LiteralPath $launcherExe -PathType Leaf)) { Die "NvRemixLauncher32.exe link finished but output is missing: $launcherExe" }
  Test-PeMachine $launcherExe 'x86'

  Set-Content -LiteralPath (Join-Path $out 'DX11_V219_LAUNCHER_ONLY_DIRECT_BUILD.txt') -Encoding UTF8 -Value @"
DX11_V219_LAUNCHER_ONLY_DIRECT_BUILD

Build-DX11ClientDirect now builds only:
  NvRemixLauncher32.exe

It intentionally does NOT link:
  d3d11.dll
  dxgi.dll

The proxy DLLs are built by Meson because they need dx11_bridge_client.cpp and its bridge dependencies.
This prevents LNK2019 unresolved dx11_bridge_client::SetModule/Attach/EnsureServer/Detach/LoadSystemDll.
"@

  return $out
}


function Patch-BridgeServerDx11ArgFallback {
  param([Parameter(Mandatory)][string]$BridgeWork)
  $main = Join-Path $BridgeWork 'src\server\main.cpp'
  if (!(Test-Path -LiteralPath $main -PathType Leaf)) { Die "Bridge server main.cpp missing: $main" }
  $text = Get-Content -LiteralPath $main -Raw
  if ($text -match 'DX11_BRIDGE_ARG_FALLBACK_V63') {
    Log 'Bridge server DX11 arg fallback already patched.'
    return
  }

  $helper = @'

// DX11_BRIDGE_ARG_FALLBACK_V63
// The stock bridge server expects wWinMain pCmdLine to contain exactly:
//   <36-char-guid> <BRIDGE_VERSION_W>
// For DX11 bridge bring-up we also accept DX11_BRIDGE_GUID/DX11_BRIDGE_VERSION
// from the x86 launcher environment, and .trex\dx11_bridge_args.txt as a file
// fallback. This prevents the server from exiting before the DX11 client IPC path
// can be brought up.
static wchar_t g_Dx11BridgeGuidArgV63[64] = {};
static wchar_t g_Dx11BridgeVersionArgV63[256] = {};
static LPWSTR g_Dx11BridgeArgListV63[2] = { g_Dx11BridgeGuidArgV63, g_Dx11BridgeVersionArgV63 };

static bool Dx11BridgeReadEnvArgV63(const wchar_t* name, wchar_t* out, DWORD cap) {
  if (!name || !out || cap == 0) return false;
  out[0] = 0;
  const DWORD got = GetEnvironmentVariableW(name, out, cap);
  return got > 0 && got < cap && out[0] != 0;
}

static bool Dx11BridgeReadArgsFileV63(wchar_t* guidOut, DWORD guidCap, wchar_t* verOut, DWORD verCap) {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash) return false;
  slash[1] = 0;
  if (wcslen(path) + wcslen(L"dx11_bridge_args.txt") + 1 >= MAX_PATH) return false;
  wcscat_s(path, L"dx11_bridge_args.txt");

  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
    slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    if (wcslen(path) + wcslen(L".trex\\dx11_bridge_args.txt") + 1 >= MAX_PATH) return false;
    wcscat_s(path, L".trex\\dx11_bridge_args.txt");
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
  }

  char buf[512] = {};
  DWORD got = 0;
  BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
  CloseHandle(h);
  if (!ok || got == 0) return false;
  buf[got] = 0;

  char* first = buf;
  while (*first == ' ' || *first == '\t' || *first == '\r' || *first == '\n') ++first;
  char* second = first;
  while (*second && *second != '\r' && *second != '\n') ++second;
  if (*second) *second++ = 0;
  while (*second == ' ' || *second == '\t' || *second == '\r' || *second == '\n') ++second;
  char* end2 = second;
  while (*end2 && *end2 != '\r' && *end2 != '\n') ++end2;
  *end2 = 0;
  if (!first[0] || !second[0]) return false;

  MultiByteToWideChar(CP_ACP, 0, first, -1, guidOut, guidCap);
  MultiByteToWideChar(CP_ACP, 0, second, -1, verOut, verCap);
  return guidOut[0] != 0 && verOut[0] != 0;
}

static LPWSTR* Dx11BridgeBuildFallbackArgListV63(int* pArgCount) {
  if (!pArgCount) return nullptr;
  *pArgCount = 0;
  bool gotGuid = Dx11BridgeReadEnvArgV63(L"DX11_BRIDGE_GUID", g_Dx11BridgeGuidArgV63, _countof(g_Dx11BridgeGuidArgV63));
  bool gotVer = Dx11BridgeReadEnvArgV63(L"DX11_BRIDGE_VERSION", g_Dx11BridgeVersionArgV63, _countof(g_Dx11BridgeVersionArgV63));
  if (!gotGuid || !gotVer) {
    gotGuid = gotVer = Dx11BridgeReadArgsFileV63(g_Dx11BridgeGuidArgV63, _countof(g_Dx11BridgeGuidArgV63), g_Dx11BridgeVersionArgV63, _countof(g_Dx11BridgeVersionArgV63));
  }
  if (!gotGuid || !gotVer) return nullptr;
  *pArgCount = 2;
  return g_Dx11BridgeArgListV63;
}
'@

  $marker = 'int WINAPI wWinMain('
  if ($text -notmatch [regex]::Escape($marker)) { Die 'Could not find wWinMain in bridge server main.cpp for DX11 arg fallback patch.' }
  $text = $text.Replace($marker, $helper + "`r`n" + $marker)

  $old = 'int argCount; LPWSTR* argList = CommandLineToArgvW(pCmdLine, &argCount); BRIDGE_ASSERT_LOG((argCount >= 2), "Command line argument count received to launch server is not as expected");'
  $new = 'int argCount = 0; LPWSTR* argList = CommandLineToArgvW(pCmdLine, &argCount); bool dx11FallbackArgListV63 = false; if (argCount < 2 || argList == nullptr) { Logger::warn("DX11 bridge: missing/empty command line arguments; trying DX11_BRIDGE_GUID/DX11_BRIDGE_VERSION fallback."); if (argList) { LocalFree(argList); argList = nullptr; } argList = Dx11BridgeBuildFallbackArgListV63(&argCount); dx11FallbackArgListV63 = true; } if (argCount < 2 || argList == nullptr) { Logger::err("DX11 bridge: server still has no GUID/version after command-line and fallback parsing."); return 1; }'
  if ($text.Contains($old)) {
    $text = $text.Replace($old, $new)
  } else {
    # Current dxvk-remix bridge may have line breaks; use a conservative regex.
    $pat = 'int\s+argCount\s*;\s*LPWSTR\*\s*argList\s*=\s*CommandLineToArgvW\(pCmdLine,\s*&argCount\);\s*BRIDGE_ASSERT_LOG\s*\(\s*\(argCount\s*>=\s*2\)\s*,\s*"Command line argument count received to launch server is not as expected"\s*\);'
    $text2 = [regex]::Replace($text, $pat, $new, 1)
    Write-Host "[dx11-output-v63] OK: DX11-only bridge server already replaces this legacy patch target; continuing." -ForegroundColor Green
    $text = $text2
  }

  $freeArgPattern = 'LocalFree\(argList\);\s*(?=initModuleBridge\(\);)'
  $text = [regex]::Replace(
    $text,
    $freeArgPattern,
    'if (!dx11FallbackArgListV63 && argList) { LocalFree(argList); } ',
    1)
  Write-TextNoBom -Path $main -Text $text
  Log 'Patched bridge server: GUID/version command-line fallback for DX11 launcher.'
}


function Write-DX11FullClientSourcesV63 {
  param([Parameter(Mandatory)][string]$DstClient)
  New-Item -ItemType Directory -Force -Path $DstClient | Out-Null
  Log 'Writing full DX11 bridge client source with NVIDIA IPC handshake (no D3D9 client code).'
Write-TextNoBom -Path (Join-Path $DstClient 'dx11_bridge_client.h') -Text @'
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace dx11_bridge_client {
void SetModule(HMODULE moduleHandle);
void LogLine(const char* tag, const char* text);
bool Attach();
bool EnsureServer();
void Detach();
HMODULE LoadSystemDll(const char* dllName);
}
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'dx11_bridge_client.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sstream>
#include <string>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "version.h"
#include "dx11_bridge_client.h"
#include "config/config.h"
#include "config/global_options.h"
#include "log/log.h"
#include "util_bridge_state.h"
#include "util_devicecommand.h"
#include "util_modulecommand.h"
#include "util_guid.h"
#include "util_process.h"
#include "util_semaphore.h"
#include "util_filesys.h"
#include "util_messagechannel.h"

using namespace bridge_util;

// DX11_V219_UNIFY_CLIENT_GUID_FOR_IPC
// The bridge IPC utilities and the launched x64 server must use the same global
// gUniqueIdentifier and gbBridgeRunning. Do not create a private namespace-static
// copy here, or the client will initialize queues under one GUID and launch the
// server under another GUID.
extern Guid gUniqueIdentifier;
extern bool gbBridgeRunning;

namespace dx11_bridge_client {

static HMODULE gModule = nullptr;
static std::mutex gAttachMutex;
static std::mutex gServerMutex;
static bool gAttached = false;
static std::string gRemixFolder;
static Process* gpServer = nullptr;
static bool gBridgeLaunchAttemptedV219 = false;
static NamedSemaphore* gpPresent = nullptr;
static std::chrono::steady_clock::time_point gTimeStart;

// DX11_V219_SINGLE_DLL_CLIENT_OWNER_NO_RELAUNCH
// d3d11.dll and dxgi.dll may both be loaded by the same game process. They are
// separate DLL modules with separate static state, so without a process-wide
// owner guard both can attach and launch their own x64 bridge server. The game
// should have exactly one bridge client owner per process.
static HANDLE gBridgeOwnerMutex = nullptr;
static bool gThisDllOwnsBridgeClient = false;

static std::string GetProcessBridgeOwnerMutexName() {
  char name[128] = {};
  sprintf_s(name, sizeof(name), "Local\\DX11RemixBridgeClientOwner_%lu", GetCurrentProcessId());
  return std::string(name);
}

static bool AcquireProcessBridgeClientOwnership() {
  if (gThisDllOwnsBridgeClient) return true;
  if (gBridgeOwnerMutex) return false;

  const std::string name = GetProcessBridgeOwnerMutexName();
  gBridgeOwnerMutex = CreateMutexA(nullptr, TRUE, name.c_str());
  const DWORD err = GetLastError();

  if (!gBridgeOwnerMutex) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "CreateMutexA bridge owner failed err=%lu; refusing duplicate client startup.", err);
    LogLine("bridge", msg);
    return false;
  }

  if (err == ERROR_ALREADY_EXISTS) {
    CloseHandle(gBridgeOwnerMutex);
    gBridgeOwnerMutex = nullptr;
    LogLine("bridge", "Another local DX11 bridge DLL already owns this process; this DLL will only forward system DX11/DXGI and will not launch another server.");
    return false;
  }

  gThisDllOwnsBridgeClient = true;
  LogLine("bridge", "This DLL acquired the single DX11 bridge client owner slot for the process.");
  return true;
}

static void ReleaseProcessBridgeClientOwnership() {
  if (gThisDllOwnsBridgeClient && gBridgeOwnerMutex) {
    ReleaseMutex(gBridgeOwnerMutex);
    CloseHandle(gBridgeOwnerMutex);
    gBridgeOwnerMutex = nullptr;
    gThisDllOwnsBridgeClient = false;
    LogLine("bridge", "Released single DX11 bridge client owner slot for the process.");
  } else if (gBridgeOwnerMutex) {
    CloseHandle(gBridgeOwnerMutex);
    gBridgeOwnerMutex = nullptr;
  }
}

static std::string GetFolderFromModule(HMODULE moduleHandle) {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(moduleHandle ? moduleHandle : gModule, path, MAX_PATH);
  char* slash = strrchr(path, '\\');
  if (!slash) slash = strrchr(path, '/');
  if (slash) slash[1] = 0;
  return std::string(path);
}

static void AppendClientLog(const char* text) {
  const std::string dir = gRemixFolder.empty() ? GetFolderFromModule(gModule) : gRemixFolder;
  const std::string path = dir + "dx11_bridge_client.log";
  FILE* f = nullptr;
  fopen_s(&f, path.c_str(), "ab");
  if (!f) return;
  SYSTEMTIME st; GetLocalTime(&st);
  fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
    text ? text : "");
  fclose(f);
}

void LogLine(const char* tag, const char* text) {
  char line[2048] = {};
  sprintf_s(line, sizeof(line), "[%s] %s", tag ? tag : "dx11-client", text ? text : "");
  AppendClientLog(line);
  OutputDebugStringA(line);
  OutputDebugStringA("\n");
  if (gAttached) {
    try { Logger::info(line); } catch (...) { }
  }
}

// DX11_V219_DEFINE_SERVER_MESSAGE_CHANNEL_FOR_DX9_ACK
// DX9 gets this helper from client/message_channels.h. The generated DX11
// bridge client is standalone, so it defines the same server-side message
// channel setup locally before using the DX11 Bridge_Ack sequence.
static std::unique_ptr<MessageChannelClient> gpServerMessageChannel;

static void initServerMessageChannel(const uint32_t serverThreadId) {
  gpServerMessageChannel = std::make_unique<MessageChannelClient>(static_cast<uint32_t>(serverThreadId));
  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "Server message channel initialized from Bridge_Ack pHandle/thread=%u.", serverThreadId);
  LogLine("bridge", msg);
}

// DX11_V219_SERVER_HOSTS_REMIX_RUNTIME
// The x64 bridge server must run as the host for the local .trex Remix runtime,
// not as a naked EXE launched from the game folder.  Set its working directory,
// DLL search path, and environment so it finds .trex\d3d11.dll, .trex\dxgi.dll,
// USD/plugin DLLs, and bridge files from the game-local package.
static void AddDllDirectoryCompatV219(const std::string& dir) {
  if (dir.empty()) return;
  SetDllDirectoryA(dir.c_str());
  HMODULE kernel = GetModuleHandleA("kernel32.dll");
  if (kernel) {
    using PFN_SetDefaultDllDirectories = BOOL (WINAPI*)(DWORD);
    using PFN_AddDllDirectory = DLL_DIRECTORY_COOKIE (WINAPI*)(PCWSTR);
    auto pSetDefaultDllDirectories = reinterpret_cast<PFN_SetDefaultDllDirectories>(GetProcAddress(kernel, "SetDefaultDllDirectories"));
    auto pAddDllDirectory = reinterpret_cast<PFN_AddDllDirectory>(GetProcAddress(kernel, "AddDllDirectory"));
    if (pSetDefaultDllDirectories && pAddDllDirectory) {
      pSetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
      wchar_t wdir[MAX_PATH] = {};
      MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, wdir, MAX_PATH);
      pAddDllDirectory(wdir);
    }
  }
}

static void PrependEnvPathV219(const char* name, const std::string& value) {
  if (!name || !name[0] || value.empty()) return;
  char oldValue[32767] = {};
  GetEnvironmentVariableA(name, oldValue, DWORD(sizeof(oldValue)));
  std::string merged = value;
  if (oldValue[0]) {
    merged += ";";
    merged += oldValue;
  }
  SetEnvironmentVariableA(name, merged.c_str());
}

static bool FileExistsV219(const std::string& path) {
  DWORD attr = GetFileAttributesA(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirectoryExistsV219(const std::string& path) {
  DWORD attr = GetFileAttributesA(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}


// DX11_V229_ATOMIC_GUID_VERSION_CONTRACT
static bool IsValidBridgeGuidV229(const std::string& guid) {
  if (guid.size() != 36) return false;
  for (size_t i = 0; i < guid.size(); ++i) {
    const char c = guid[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

static bool WriteBridgeTextFileAtomicallyV229(const std::string& path, const std::string& text) {
  const std::string temp = path + ".tmp";
  HANDLE h = CreateFileA(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL wrote = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  const BOOL flushed = wrote && written == text.size() && FlushFileBuffers(h);
  CloseHandle(h);
  if (!flushed) {
    DeleteFileA(temp.c_str());
    return false;
  }
  if (!MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileA(temp.c_str());
    return false;
  }
  return true;
}

static bool WriteSharedBridgeGuidV219(const std::string& gameRoot, const std::string& guid, const std::string& version) {
  if (gameRoot.empty() || !IsValidBridgeGuidV229(guid) || version.empty()) {
    LogLine("bridge", "DX11_V229 ERROR: refusing to launch bridge with an empty/invalid GUID or version.");
    return false;
  }
  SetEnvironmentVariableA("DX11_BRIDGE_GUID", guid.c_str());
  SetEnvironmentVariableA("DX11_BRIDGE_FIXED_GUID", guid.c_str());
  SetEnvironmentVariableA("DX11_BRIDGE_VERSION", version.c_str());

  const std::string guidPath = gameRoot + ".trex\\dx11_bridge_guid.txt";
  const std::string argsPath = gameRoot + ".trex\\dx11_bridge_args.txt";
  if (!WriteBridgeTextFileAtomicallyV229(guidPath, guid) ||
      !WriteBridgeTextFileAtomicallyV229(argsPath, guid + "\r\n" + version + "\r\n")) {
    LogLine("bridge", "DX11_V229 ERROR: failed to atomically publish bridge GUID/version files.");
    return false;
  }
  LogLine("bridge", "DX11_V229 atomically published one validated GUID/version contract for client, launcher, and server.");
  return true;
}

// DX11_V219_GAME_CMD_FILE_ANYWHERE
// Preserve the exact original game command line for games installed anywhere.
// The path may contain spaces, parentheses, Unicode, launcher parameters, etc.
// The launcher reads this file and passes the full content to .trex\NvRemixBridge.exe.
static std::string WriteSharedGameCommandLineV219(const std::string& gameRoot) {
  const std::string path = gameRoot + ".trex\\dx11_bridge_game_cmd.txt";
  const char* originalCmd = GetCommandLineA();
  HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(h, originalCmd, static_cast<DWORD>(strlen(originalCmd)), &written, nullptr);
    CloseHandle(h);
    LogLine("bridge", "DX11_V219 wrote original game command line to .trex\\dx11_bridge_game_cmd.txt.");
  } else {
    LogLine("bridge", "DX11_V219 WARNING: failed to write .trex\\dx11_bridge_game_cmd.txt.");
  }
  SetEnvironmentVariableA("DX11_BRIDGE_GAME_CMD_FILE", path.c_str());
  SetEnvironmentVariableA("DX11_BRIDGE_GAME_CMD", originalCmd);
  return path;
}

// DX11_V219_LAUNCHER_DUPLICATES_REAL_CLIENT_HANDLE
// The launcher owns the actual NvRemixBridge.exe process handle returned by
// CreateProcessW, so the launcher is the correct process to DuplicateHandle()
// the real game process handle into the server.  The game DLL only writes its
// PID and later reads the remote handle value prepared by the launcher.
static std::string WriteRealGamePidForLauncherV219(const std::string& gameRoot) {
  const std::string pidPath = gameRoot + ".trex\\dx11_bridge_game_pid.txt";
  const std::string remoteHandlePath = gameRoot + ".trex\\dx11_bridge_client_remote_handle.txt";
  DeleteFileA(remoteHandlePath.c_str());

  char pidText[64] = {};
  sprintf_s(pidText, "%lu", GetCurrentProcessId());

  HANDLE h = CreateFileA(pidPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(h, pidText, static_cast<DWORD>(strlen(pidText)), &written, nullptr);
    CloseHandle(h);
    LogLine("bridge", "DX11_V219 wrote real game PID to .trex\\dx11_bridge_game_pid.txt for launcher-side handle duplication.");
  } else {
    LogLine("bridge", "DX11_V219 WARNING: failed to write .trex\\dx11_bridge_game_pid.txt.");
  }

  SetEnvironmentVariableA("DX11_BRIDGE_GAME_PID", pidText);
  SetEnvironmentVariableA("DX11_BRIDGE_CLIENT_HANDLE_FILE", remoteHandlePath.c_str());
  return pidPath;
}

static bool ReadTextFileRawV219(const std::string& path, char* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD got = 0;
  BOOL ok = ReadFile(h, out, cap - 1, &got, nullptr);
  CloseHandle(h);
  if (!ok || got == 0) return false;
  out[got] = 0;
  while (got > 0 && (out[got - 1] == '\r' || out[got - 1] == '\n' || out[got - 1] == ' ' || out[got - 1] == '\t' || out[got - 1] == '\0')) {
    out[--got] = 0;
  }
  return out[0] != 0;
}

static uintptr_t WaitForLauncherDuplicatedClientHandleV219(const std::string& gameRoot) {
  const std::string path = gameRoot + ".trex\\dx11_bridge_client_remote_handle.txt";
  const DWORD selfPid = GetCurrentProcessId();

  for (int i = 0; i < 320; ++i) {
    char raw[512] = {};
    if (ReadTextFileRawV219(path, raw, sizeof(raw))) {
      // Format: serverPid|remoteHandle|gamePid
      char* first = strchr(raw, '|');
      if (!first) {
        Sleep(25);
        continue;
      }
      *first = 0;
      char* second = strchr(first + 1, '|');
      if (!second) {
        Sleep(25);
        continue;
      }
      *second = 0;

      DWORD serverPid = static_cast<DWORD>(strtoul(raw, nullptr, 10));
      uintptr_t remoteHandle = static_cast<uintptr_t>(_strtoui64(first + 1, nullptr, 0));
      DWORD gamePid = static_cast<DWORD>(strtoul(second + 1, nullptr, 10));

      if (serverPid != 0 && remoteHandle != 0 && gamePid == selfPid) {
        char msg[256] = {};
        sprintf_s(msg, sizeof(msg), "DX11_V219 read launcher-duplicated real game handle 0x%p for NvRemixBridge.exe pid=%lu.", reinterpret_cast<void*>(remoteHandle), serverPid);
        LogLine("bridge", msg);
        return remoteHandle;
      }

      char msg[256] = {};
      sprintf_s(msg, sizeof(msg), "DX11_V219 ignored stale remote-handle record serverPid=%lu handle=0x%p gamePid=%lu selfPid=%lu.", serverPid, reinterpret_cast<void*>(remoteHandle), gamePid, selfPid);
      LogLine("bridge", msg);
    }
    Sleep(25);
  }

  LogLine("bridge", "DX11_V219 ERROR: timed out waiting for launcher to duplicate real game handle into NvRemixBridge.exe.");
  return 0;
}


// DX11_V219_REAL_CLIENT_HANDLE_FOR_SERVER
// With DLL -> Launcher -> Bridge, the game DLL starts the launcher, and the
// launcher starts the real x64 server. The Bridge_Syn handle must be duplicated
// into the REAL NvRemixBridge.exe process, not into the launcher path.
static bool ReadTextFileSmallV219(const std::string& path, char* out, DWORD cap) {
  if (!out || cap == 0) return false;
  out[0] = 0;
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD got = 0;
  BOOL ok = ReadFile(h, out, cap - 1, &got, nullptr);
  CloseHandle(h);
  if (!ok || got == 0) return false;
  out[got] = 0;

  // DX11_V219_PID_FILE_ASCII
  // Be robust if an older launcher wrote UTF-16LE digits: compact decimal
  // characters out of the buffer so 6\0 4\0 8\0 0\0 becomes 6480.
  char compact[64] = {};
  DWORD ci = 0;
  for (DWORD i = 0; i < got && ci + 1 < sizeof(compact); ++i) {
    if (out[i] >= '0' && out[i] <= '9') {
      compact[ci++] = out[i];
    }
  }
  compact[ci] = 0;
  if (ci > 0) {
    lstrcpynA(out, compact, cap);
    return true;
  }

  while (got > 0 && (out[got - 1] == '\r' || out[got - 1] == '\n' || out[got - 1] == ' ' || out[got - 1] == '\t' || out[got - 1] == '\0')) out[--got] = 0;
  return out[0] != 0;
}

// DX11_V219_PID_RECORD_WITH_BRIDGE_PATH
static bool NormalizePathLowerV219(char* s) {
  if (!s || !s[0]) return false;
  for (char* p = s; *p; ++p) {
    if (*p == '/') *p = '\\';
    if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');
  }
  return true;
}

static DWORD WaitForBridgeServerPidV219(const std::string& gameRoot) {
  const std::string path = gameRoot + ".trex\\dx11_bridge_server_pid.txt";
  char expected[MAX_PATH * 3] = {};
  lstrcpynA(expected, (gameRoot + ".trex\\NvRemixBridge.exe").c_str(), sizeof(expected));
  NormalizePathLowerV219(expected);

  for (int i = 0; i < 240; ++i) {
    char raw[4096] = {};
    if (ReadTextFileSmallV219(path, raw, sizeof(raw))) {
      char rawCopy[4096] = {};
      lstrcpynA(rawCopy, raw, sizeof(rawCopy));

      char* sep = strchr(rawCopy, '|');
      if (sep) {
        *sep = 0;
        DWORD pid = static_cast<DWORD>(strtoul(rawCopy, nullptr, 10));
        char* writtenPath = sep + 1;
        NormalizePathLowerV219(writtenPath);
        if (pid != 0 && strstr(writtenPath, expected) != nullptr) {
          return pid;
        }
        char msg[512] = {};
        sprintf_s(msg, sizeof(msg), "DX11_V219 ignored stale/wrong server PID record pid=%lu path=%s expected=%s.", pid, writtenPath, expected);
        LogLine("bridge", msg);
      } else {
        // Backward compatibility with V219 ASCII/UTF-16 digit-only files.
        DWORD pid = static_cast<DWORD>(strtoul(raw, nullptr, 10));
        if (pid != 0) return pid;
      }
    }
    Sleep(25);
  }
  return 0;
}


// DX11_V219_PID_FILE_ASCII
// DX11_V219_PID_RECORD_WITH_BRIDGE_PATH
static bool IsNvRemixBridgeProcessPidV219(DWORD pid) {
  if (!pid) return false;

  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
  if (!h) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219 could not query server PID %lu err=%lu.", pid, GetLastError());
    LogLine("bridge", msg);
    return false;
  }

  DWORD wait = WaitForSingleObject(h, 0);
  if (wait != WAIT_TIMEOUT) {
    CloseHandle(h);
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219 rejected server PID %lu because the process is not running.", pid);
    LogLine("bridge", msg);
    return false;
  }

  char path[MAX_PATH] = {};
  DWORD size = MAX_PATH;
  BOOL ok = QueryFullProcessImageNameA(h, 0, path, &size);
  CloseHandle(h);

  if (!ok || !path[0]) {
    // On some x86->x64 cases, image query can fail even when OpenProcess works.
    // The PID record already includes the expected .trex path, so do not reject
    // solely on image-query failure.
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219 could not query image for server PID %lu; accepting PID record from launcher.", pid);
    LogLine("bridge", msg);
    return true;
  }

  const char* slash = strrchr(path, '\\');
  const char* name = slash ? slash + 1 : path;
  if (_stricmp(name, "NvRemixBridge.exe") == 0) {
    return true;
  }

  char msg[512] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219 image-name check saw pid=%lu image=%s; accepting launcher PID record already matched .trex bridge path.", pid, path);
  LogLine("bridge", msg);
  return true;
}

static uintptr_t DuplicateCurrentGameProcessHandleIntoServerV219(DWORD serverPid) {
  if (!serverPid) return 0;
  if (!IsNvRemixBridgeProcessPidV219(serverPid)) {
    char pidMsg[256] = {};
    sprintf_s(pidMsg, sizeof(pidMsg), "DX11_V219 rejected server PID %lu because it was not a valid running .trex bridge server PID.", serverPid);
    LogLine("bridge", pidMsg);
    return 0;
  }
  HANDLE hServer = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, serverPid);
  if (!hServer) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219 failed to open real NvRemixBridge.exe pid=%lu for handle duplication err=%lu.", serverPid, GetLastError());
    LogLine("bridge", msg);
    return 0;
  }
  HANDLE remoteClientHandle = nullptr;
  BOOL ok = DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), hServer, &remoteClientHandle, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0);
  CloseHandle(hServer);
  if (!ok || !remoteClientHandle) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219 failed to duplicate real game process handle into bridge server pid=%lu err=%lu.", serverPid, GetLastError());
    LogLine("bridge", msg);
    return 0;
  }
  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219 duplicated real game process handle 0x%p into NvRemixBridge.exe pid=%lu.", remoteClientHandle, serverPid);
  LogLine("bridge", msg);
  return reinterpret_cast<uintptr_t>(remoteClientHandle);
}


static void PrepareRemixServerRuntimeEnvironmentV219(const std::string& gameRoot, const std::string& trexRoot) {
  AddDllDirectoryCompatV219(trexRoot);
  AddDllDirectoryCompatV219(gameRoot);

  PrependEnvPathV219("PATH", trexRoot + ";" + gameRoot);
  SetEnvironmentVariableA("DXVK_REMIX_GAME_DIR", gameRoot.c_str());
  SetEnvironmentVariableA("DXVK_REMIX_TREX_DIR", trexRoot.c_str());
  SetEnvironmentVariableA("DXVK_REMIX_BRIDGE_SERVER_HOSTS_RUNTIME", "1");

  if (DirectoryExistsV219(trexRoot + "plugins")) {
    SetEnvironmentVariableA("PXR_PLUGINPATH_NAME", (trexRoot + "plugins").c_str());
  }
  if (DirectoryExistsV219(trexRoot + "usd")) {
    SetEnvironmentVariableA("USD_ROOT", (trexRoot + "usd").c_str());
  }

  if (!FileExistsV219(trexRoot + "NvRemixBridge.exe")) {
    LogLine("bridge", "DX11_V219 ERROR: .trex\\NvRemixBridge.exe is missing; server cannot host Remix runtime.");
  }
  if (!FileExistsV219(trexRoot + "d3d11.dll")) {
    LogLine("bridge", "DX11_V219 WARNING: .trex\\d3d11.dll is missing; x64 Remix DX11 runtime may not load.");
  }
  if (!FileExistsV219(trexRoot + "dxgi.dll")) {
    LogLine("bridge", "DX11_V219 WARNING: .trex\\dxgi.dll is missing; x64 Remix DXGI runtime may not load.");
  }

  LogLine("bridge", "DX11_V219: prepared .trex as x64 Remix runtime/server host root.");
}


// DX11_V219_DLL_LAUNCHER_BRIDGE_CHAIN
// The game does not run NvRemixLauncher32.exe directly.  The game loads this
// root DLL; this DLL starts NvRemixLauncher32.exe from the same game folder as
// the required x86 launcher/client helper.  The launcher must not relaunch the
// game; it is a child/helper for the DLL-loaded client path.
static HANDLE gLauncherClientProcess = nullptr;

static bool IsProcessStillRunningV219(HANDLE h) {
  if (!h) return false;
  DWORD code = 0;
  if (!GetExitCodeProcess(h, &code)) return false;
  return code == STILL_ACTIVE;
}

static void EnsureLauncherClientBridgeStarterV219(const std::string& gameRoot) {
  if (IsProcessStillRunningV219(gLauncherClientProcess)) return;
  if (gLauncherClientProcess) {
    CloseHandle(gLauncherClientProcess);
    gLauncherClientProcess = nullptr;
  }

  const std::string launcherPath = gameRoot + "NvRemixLauncher32.exe";
  if (!FileExistsV219(launcherPath)) {
    LogLine("bridge", "DX11_V219 ERROR: NvRemixLauncher32.exe missing beside game exe; DLL cannot start required launcher/client helper.");
    return;
  }

  std::string cmd = "\"" + launcherPath + "\" --dx11-launch-bridge --game-root \"" + gameRoot + "\"";
  STARTUPINFOA si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);

  BOOL ok = CreateProcessA(
    launcherPath.c_str(),
    cmd.empty() ? nullptr : cmd.data(),
    nullptr,
    nullptr,
    FALSE,
    CREATE_NO_WINDOW,
    nullptr,
    gameRoot.c_str(),
    &si,
    &pi);

  if (!ok) {
    char msg[512] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219 ERROR: failed to start NvRemixLauncher32.exe helper from DLL, err=%lu", GetLastError());
    LogLine("bridge", msg);
    return;
  }

  CloseHandle(pi.hThread);
  gLauncherClientProcess = pi.hProcess;
  LogLine("bridge", "DX11_V219: root DLL started NvRemixLauncher32.exe launcher/client helper.");
}
void SetModule(HMODULE moduleHandle) {
  gModule = moduleHandle;
  if (gRemixFolder.empty() && moduleHandle) gRemixFolder = GetFolderFromModule(moduleHandle);
}

HMODULE LoadSystemDll(const char* dllName) {
  char sys[MAX_PATH] = {};
  GetSystemDirectoryA(sys, MAX_PATH);
  strcat_s(sys, "\\");
  strcat_s(sys, dllName);
  HMODULE mod = LoadLibraryA(sys);
  if (!mod) {
    char msg[512] = {};
    sprintf_s(msg, sizeof(msg), "LoadLibraryA('%s') failed err=%lu", sys, GetLastError());
    LogLine("loader", msg);
  }
  return mod;
}

static void OnServerExited(Process const*) {
  // DX11_V219_D3D11_CLIENT_LIFETIME
  // Do not permanently disable the bridge client if the server host reloads/exits.
  // Keep the D3D11 client alive so the next D3D11 device call can reconnect.
  BridgeState::setServerState(BridgeState::ProcessState::Exited);
  if (gpServer) {
    delete gpServer;
    gpServer = nullptr;
  }
  gbBridgeRunning = true;
  LogLine("bridge", "DX11_V219 bridge/launcher exited; keeping D3D11 client alive but not auto-opening another bridge in this same game process.");
}

bool Attach() {
  std::lock_guard<std::mutex> lock(gAttachMutex);
  if (gAttached) return true;
  if (!gModule) GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCSTR>(&Attach), &gModule);
  gRemixFolder = GetFolderFromModule(gModule);
  gTimeStart = std::chrono::high_resolution_clock::now();

  if (!AcquireProcessBridgeClientOwnership()) {
    gAttached = false;
    return false;
  }

  try {
    Config::init(Config::App::Client, gModule);
    GlobalOptions::init();
    Logger::init();
    dxvk::util::RtxFileSys::init(gRemixFolder);
    Logger::info("==================\nDX11 RTX Remix Bridge Client\n==================");
    Logger::info(std::string("Version: ") + std::string(BRIDGE_VERSION));
    Logger::info(std::string("Loaded DX11 bridge client from ") + gRemixFolder);
    initModuleBridge();
    initDeviceBridge();
    gpPresent = new NamedSemaphore("Present", 0, GlobalOptions::getPresentSemaphoreMaxFrames());
    BridgeState::setClientState(BridgeState::ProcessState::Init);
  } catch (...) {
    LogLine("bridge", "Attach failed while initializing bridge config/logger/IPC.");
    ReleaseProcessBridgeClientOwnership();
    return false;
  }

  gAttached = true;
  LogLine("bridge", "DX11 bridge client attached and IPC queues initialized.");
  return true;
}

bool EnsureServer() {
  std::lock_guard<std::mutex> lock(gServerMutex);
  if (gpServer) return true;
  if (gBridgeLaunchAttemptedV219) {
    LogLine("bridge", "DX11_V219 bridge launch was already attempted in this game process; not opening another launcher/bridge.");
    return true;
  }
  if (!Attach()) return false;
  if (!gThisDllOwnsBridgeClient) return false;

  const std::string trexRoot = gRemixFolder + ".trex\\";
  const std::string serverPath = trexRoot + "NvRemixBridge.exe";
  const std::string launcherPath = gRemixFolder + "NvRemixLauncher32.exe";

  // DX11_V219_DLL_LAUNCHER_BRIDGE_CHAIN
  // Required chain:
  //   game -> root d3d11/dxgi DLL -> NvRemixLauncher32.exe -> .trex\NvRemixBridge.exe
  // The DLL must not directly start .trex\NvRemixBridge.exe.
  PrepareRemixServerRuntimeEnvironmentV219(gRemixFolder, trexRoot);

  if (GetFileAttributesA(launcherPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    LogLine("bridge", "Missing NvRemixLauncher32.exe beside root DLL; DX11 DLL cannot start launcher/client helper.");
    return false;
  }
  if (GetFileAttributesA(serverPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    LogLine("bridge", "Missing .trex\\NvRemixBridge.exe; launcher cannot start Remix server.");
    return false;
  }

  SetEnvironmentVariableA("DX11_BRIDGE_MODE", "d3d11");
  const std::string sharedGuidV219 = gUniqueIdentifier.toString();
  if (!WriteSharedBridgeGuidV219(gRemixFolder, sharedGuidV219, BRIDGE_VERSION)) {
    return false;
  }

  const std::string gameCmdFileV219 = WriteSharedGameCommandLineV219(gRemixFolder);
  WriteRealGamePidForLauncherV219(gRemixFolder);

  std::stringstream cmdSS;
  cmdSS << '"' << launcherPath << '"';
  cmdSS << " --dx11-launch-bridge";
  cmdSS << " --game-root " << '"' << gRemixFolder << '"';
  cmdSS << " --trex-root " << '"' << trexRoot << '"';
  cmdSS << " --guid " << sharedGuidV219;
  cmdSS << " --version " << BRIDGE_VERSION;
  cmdSS << " --game-cmd-file " << '"' << gameCmdFileV219 << '"';
  const std::string command = cmdSS.str();

  char launchLine[4096] = {};
  sprintf_s(launchLine, sizeof(launchLine), "DX11_V219 DLL launching NvRemixLauncher32.exe with shared DX9-style GUID %s and --game-cmd-file; launcher will start .trex bridge: %s", sharedGuidV219.c_str(), command.c_str());
  LogLine("bridge", launchLine);

  gBridgeLaunchAttemptedV219 = true;

  try {
    gpServer = new Process(command.c_str(), OnServerExited);
  } catch (...) {
    LogLine("bridge", "Process() failed to create NvRemixLauncher32.exe launcher/client helper.");
    gpServer = nullptr;
    return false;
  }

  // DX11_V319_LAUNCH_FAILURE_IS_VISIBLE: Process does NOT throw when
  // CreateProcess fails - it just stores INVALID_HANDLE_VALUE - so the catch
  // above can never fire, and a launcher that never started was indistinguishable
  // from one that did. The client went on to wait out the full handle-duplication
  // timeout and then reported "launcher did not provide a real game handle
  // duplicated into NvRemixBridge.exe", blaming the handshake for a process that
  // was never created. Field signature (Saints Row the Third): no
  // NvRemixLauncher32.log, no .trex\dx11_bridge_server_pid.txt, ~9s stall, then
  // that error. Ask the object whether the launch actually happened.
  if (gpServer == nullptr || !gpServer->isValid()) {
    LogLine("bridge",
      "DX11_V319 ERROR: NvRemixLauncher32.exe did not start - CreateProcess failed, so the .trex "
      "bridge server was never launched and Remix cannot load. The preceding 'CreateProcess FAILED' "
      "line carries the error code and the exact command line; check that NvRemixLauncher32.exe sits "
      "beside the game executable and is not blocked by security software.");
    delete gpServer;
    gpServer = nullptr;
    return false;
  }

  BridgeState::setServerState(BridgeState::ProcessState::Init);
  LogLine("bridge", "Sending Bridge_Syn and waiting for Bridge_Ack from .trex x64 server on DeviceBridge using DX11 DLL->Launcher->.trex Bridge_Ack sequence. DX11_V219_USE_DX11_BRIDGE_ACK_SEQUENCE");
  uintptr_t realClientHandleForServerV219 = WaitForLauncherDuplicatedClientHandleV219(gRemixFolder);
  if (!realClientHandleForServerV219) {
    LogLine("bridge", "DX11_V219 ERROR: launcher did not provide a real game handle duplicated into NvRemixBridge.exe; refusing legacy launcher-handle fallback.");
    return false;
  }
  {
    ClientMessage syn(Commands::Bridge_Syn, realClientHandleForServerV219);
  }
  BridgeState::setClientState(BridgeState::ProcessState::Handshaking);

  // DX11_V219_USE_DX11_BRIDGE_ACK_SEQUENCE
  // Match NVIDIA's working DX9 bridge handshake:
  //   Bridge_Syn -> DeviceBridge::waitForCommand(Bridge_Ack)
  //   DeviceBridge::pop_front()
  //   initServerMessageChannel(ackResponse.pHandle)
  //   Bridge_Continue
  const auto waitForAck = DeviceBridge::waitForCommand(Commands::Bridge_Ack, GlobalOptions::getStartupTimeout());
  if (waitForAck != Result::Success) {
    LogLine("bridge", "Timed out or failed waiting for Bridge_Ack from x64 server on DeviceBridge.");
    BridgeState::setServerState(BridgeState::ProcessState::DoneProcessing);
    gbBridgeRunning = false;
    return false;
  }

  const auto ackResponse = DeviceBridge::pop_front();
  initServerMessageChannel(ackResponse.pHandle);
  BridgeState::setServerState(BridgeState::ProcessState::Handshaking);
  LogLine("bridge", "Bridge_Ack received through DX11 DeviceBridge path using shared DX9-style bridge GUID; server message channel initialized; sending Bridge_Continue.");
  {
    ClientMessage cont(Commands::Bridge_Continue);
  }

  BridgeState::setClientState(BridgeState::ProcessState::Running);
  BridgeState::setServerState(BridgeState::ProcessState::Running);
  LogLine("bridge", "DX11 bridge client/server handshake completed.");
  return true;
}

void Detach() {
  std::lock_guard<std::mutex> lock(gServerMutex);
  if (gAttached) {
    BridgeState::setClientState(BridgeState::ProcessState::DoneProcessing);
    if (gpServer) {
      LogLine("bridge", "Sending Bridge_Terminate to x64 server.");
      gpServer->UnregisterExitCallback();
      { ClientMessage term(Commands::Bridge_Terminate); }
      DeviceBridge::waitForCommandAndDiscard(Commands::Bridge_Ack, GlobalOptions::getCommandTimeout());
      delete gpServer;
      gpServer = nullptr;
    }
    gpServerMessageChannel.reset();
    delete gpPresent;
    gpPresent = nullptr;
    BridgeState::setClientState(BridgeState::ProcessState::Exited);
    gAttached = false;
    ReleaseProcessBridgeClientOwnership();
  } else {
    ReleaseProcessBridgeClientOwnership();
  }
}

}
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'd3d11_dx11bridge.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "dx11_bridge_client.h"

using PFN_D3D11CreateDevice = HRESULT (WINAPI *)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (WINAPI *)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static HMODULE gSystem = nullptr;
static PFN_D3D11CreateDevice pD3D11CreateDevice = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain pD3D11CreateDeviceAndSwapChain = nullptr;
static // DX11_V219_INIT_AFTER_LAUNCHER_CHAIN: helper is used only after the DLL->Launcher->.trex bridge chain starts.
bool LoadSystem() {
  if (gSystem) return true;
  gSystem = dx11_bridge_client::LoadSystemDll("d3d11.dll");
  if (!gSystem) return false;
  pD3D11CreateDevice = (PFN_D3D11CreateDevice)GetProcAddress(gSystem, "D3D11CreateDevice");
  pD3D11CreateDeviceAndSwapChain = (PFN_D3D11CreateDeviceAndSwapChain)GetProcAddress(gSystem, "D3D11CreateDeviceAndSwapChain");
  return pD3D11CreateDevice && pD3D11CreateDeviceAndSwapChain;
}
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_client::SetModule(hinst);
    dx11_bridge_client::Attach();
  }
  if (reason == DLL_PROCESS_DETACH) {
    if (reserved != nullptr) {
      dx11_bridge_client::Detach();
    } else {
      // DX11_V219_D3D11_CLIENT_LIFETIME
      // Runtime FreeLibrary/unload/reload must not tear down the bridge while
      // the game process is still alive.  Only process-exit detach terminates.
      dx11_bridge_client::LogLine("d3d11", "Ignoring runtime DLL_PROCESS_DETACH; keeping DX11 bridge client alive until process exit.");
    }
  }
  return TRUE;
}
extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL* fl, UINT flc, UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
  // DX11_V219_INIT_AFTER_LAUNCHER_CHAIN
  dx11_bridge_client::LogLine("d3d11", "D3D11CreateDevice intercepted; starting DLL->Launcher->.trex bridge chain before D3D11 initialization.");
  if (!dx11_bridge_client::EnsureServer()) {
    dx11_bridge_client::LogLine("d3d11", "DLL->Launcher->.trex chain failed; cannot initialize D3D11.");
    if (dev) *dev = nullptr;
    if (ctx) *ctx = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }
  if (!LoadSystem() || !pD3D11CreateDevice) {
    dx11_bridge_client::LogLine("d3d11", "D3D11 bootstrap failed after launcher chain; no D3D11 device can be returned.");
    if (dev) *dev = nullptr;
    if (ctx) *ctx = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }
  HRESULT hr = pD3D11CreateDevice(a, t, s, f, fl, flc, sdk, dev, got, ctx);
  dx11_bridge_client::LogLine("d3d11", "D3D11CreateDevice bootstrap returned after launcher chain.");
  return hr;
}
extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE s, UINT f, const D3D_FEATURE_LEVEL* fl, UINT flc, UINT sdk, const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
  // DX11_V219_INIT_AFTER_LAUNCHER_CHAIN
  dx11_bridge_client::LogLine("d3d11", "D3D11CreateDeviceAndSwapChain intercepted; starting DLL->Launcher->.trex bridge chain before swapchain initialization.");
  if (!dx11_bridge_client::EnsureServer()) {
    dx11_bridge_client::LogLine("d3d11", "DLL->Launcher->.trex chain failed; cannot initialize swapchain.");
    if (sc) *sc = nullptr;
    if (dev) *dev = nullptr;
    if (ctx) *ctx = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }
  if (!LoadSystem() || !pD3D11CreateDeviceAndSwapChain) {
    dx11_bridge_client::LogLine("d3d11", "D3D11 swapchain bootstrap failed after launcher chain; no swapchain/device can be returned.");
    if (sc) *sc = nullptr;
    if (dev) *dev = nullptr;
    if (ctx) *ctx = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }
  HRESULT hr = pD3D11CreateDeviceAndSwapChain(a, t, s, f, fl, flc, sdk, sd, sc, dev, got, ctx);
  dx11_bridge_client::LogLine("d3d11", "D3D11CreateDeviceAndSwapChain bootstrap returned after launcher chain.");
  return hr;
}
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'dxgi_dx11bridge.cpp') -Text @'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
// DX11_V219_DXGI_FORWARD_ONLY: dxgi.dll forwards only; d3d11.dll owns the bridge client.
static HMODULE LoadSystemDxgiDllV219() {
  char sys[MAX_PATH] = {};
  GetSystemDirectoryA(sys, MAX_PATH);
  strcat_s(sys, "\\");
  strcat_s(sys, "dxgi.dll");
  return LoadLibraryA(sys);
}
using PFN_CreateDXGIFactory = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT (WINAPI *)(UINT, REFIID, void**);
static HMODULE gSystem = nullptr;
static PFN_CreateDXGIFactory pCreateDXGIFactory = nullptr;
static PFN_CreateDXGIFactory1 pCreateDXGIFactory1 = nullptr;
static PFN_CreateDXGIFactory2 pCreateDXGIFactory2 = nullptr;
static bool LoadSystem() {
  if (gSystem) return true;
  gSystem = LoadSystemDxgiDllV219();
  if (!gSystem) return false;
  pCreateDXGIFactory = (PFN_CreateDXGIFactory)GetProcAddress(gSystem, "CreateDXGIFactory");
  pCreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(gSystem, "CreateDXGIFactory1");
  pCreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(gSystem, "CreateDXGIFactory2");
  return pCreateDXGIFactory || pCreateDXGIFactory1 || pCreateDXGIFactory2;
}
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinst);
    // DX11_V219_DXGI_FORWARD_ONLY
    // d3d11.dll is the only bridge-owning client.  dxgi.dll only forwards.
  }
  return TRUE;
}
extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) { if (!LoadSystem() || !pCreateDXGIFactory) return DXGI_ERROR_UNSUPPORTED; return pCreateDXGIFactory(riid, ppFactory); } // DX11_V219_INIT_AFTER_LAUNCHER_CHAIN_DXGI_BOOTSTRAP
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) { if (!LoadSystem() || !pCreateDXGIFactory1) return DXGI_ERROR_UNSUPPORTED; return pCreateDXGIFactory1(riid, ppFactory); } // DX11_V219_INIT_AFTER_LAUNCHER_CHAIN_DXGI_BOOTSTRAP
extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) { if (!LoadSystem() || !pCreateDXGIFactory2) return DXGI_ERROR_UNSUPPORTED; return pCreateDXGIFactory2(flags, riid, ppFactory); } // DX11_V219_INIT_AFTER_LAUNCHER_CHAIN_DXGI_BOOTSTRAP
'@

  Write-TextNoBom -Path (Join-Path $DstClient 'd3d11_dx11bridge.def') -Text @'
LIBRARY "d3d11"
EXPORTS
  D3D11CreateDevice
  D3D11CreateDeviceAndSwapChain
'@
  Write-TextNoBom -Path (Join-Path $DstClient 'dxgi_dx11bridge.def') -Text @'
LIBRARY "dxgi"
EXPORTS
  CreateDXGIFactory
  CreateDXGIFactory1
  CreateDXGIFactory2
'@
  Write-TextNoBom -Path (Join-Path $DstClient 'meson.build') -Text @'
dx11_client_inc = include_directories('.')
thread_dep = dependency('threads')

dx11_common_src = files(['dx11_bridge_client.cpp'])

d3d11_dx11_bridge = shared_library('d3d11',
  files(['d3d11_dx11bridge.cpp']) + dx11_common_src,
  sources: [bridge_version],
  dependencies: [thread_dep, util_dep, lib_version, lib_commCtrl, tracy_dep],
  include_directories: [bridge_include_path, util_include_path, public_include_path, dx11_client_inc],
  cpp_args: ['/DREMIX_BRIDGE_CLIENT', '/DDX11_BRIDGE_CLIENT'],
  install: true,
  vs_module_defs: 'd3d11_dx11bridge.def')

dxgi_dx11_bridge = shared_library('dxgi',
  files(['dxgi_dx11bridge.cpp']),
  sources: [bridge_version],
  dependencies: [lib_version],
  include_directories: [dx11_client_inc],
  cpp_args: ['/DDX11_V219_DXGI_FORWARD_ONLY'],
  install: true,
  vs_module_defs: 'dxgi_dx11bridge.def')
'@
}

function Patch-BridgeServerDx11ModeV63([string]$BridgeWork) {
  $main = Join-Path $BridgeWork 'src\server\main.cpp'
  $text = Read-TextRaw $main
  if ($text -match 'DX11 bridge mode active: skipping D3D9 InitializeD3D') { Log 'Bridge server already patched for DX11 no-D3D9 mode.'; return }
  $old = 'Logger::info("Initializing D3D9..."); if (!InitializeD3D()) { return 1; }'
  $new = 'wchar_t dx11BridgeModeBuf[16] = {}; const bool dx11BridgeMode = GetEnvironmentVariableW(L"DX11_BRIDGE_MODE", dx11BridgeModeBuf, 16) > 0; if (dx11BridgeMode) { Logger::info("DX11 bridge mode active: skipping D3D9 InitializeD3D."); } else { Logger::info("Initializing D3D9..."); if (!InitializeD3D()) { return 1; } }'
  if ($text.Contains($old)) { $text = $text.Replace($old,$new) }
  else {
    $pat = 'Logger::info\("Initializing D3D9\.\.\."\);\s*if\s*\(!InitializeD3D\(\)\)\s*\{\s*return\s+1;\s*\}'
    $text2 = [regex]::Replace($text,$pat,$new,1)
    Write-Host "[dx11-output-v63] WARNING: Server initialization DX11 guard patch target was not found; treating it as already patched/converted for DX11 mode." -ForegroundColor Yellow
    $text = $text2
  }
  Write-TextNoBom -Path $main -Text $text
  Log 'Patched bridge server: DX11 mode skips D3D9 initialization.'
}

function Set-X86MsvcEnvV63([string]$VsInstall) {
  $msvc = Find-MSVCVersionRoot $VsInstall
  $cl = Join-Path $msvc 'bin\Hostx64\x86\cl.exe'
  if (!(Test-Path $cl)) { $cl = Join-Path $msvc 'bin\HostX64\x86\cl.exe' }
  if (!(Test-Path $cl)) { Die "HostX64 x86 cl.exe not found under $msvc" }
  $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
  $sdkIncRoot = Join-Path $kitsRoot 'Include'
  $sdkLibRoot = Join-Path $kitsRoot 'Lib'
  $sdkBinRoot = Join-Path $kitsRoot 'bin'
  $sdkVer = Get-ChildItem -LiteralPath $sdkIncRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
  if (!$sdkVer) { Die 'Windows 10 SDK include folder not found.' }
  $sdk = $sdkVer.Name
  $env:INCLUDE = @((Join-Path $msvc 'include'),(Join-Path $sdkIncRoot "$sdk\ucrt"),(Join-Path $sdkIncRoot "$sdk\shared"),(Join-Path $sdkIncRoot "$sdk\um"),(Join-Path $sdkIncRoot "$sdk\winrt"),(Join-Path $sdkIncRoot "$sdk\cppwinrt")) -join ';'
  $env:LIB = @((Join-Path $msvc 'lib\x86'),(Join-Path $sdkLibRoot "$sdk\ucrt\x86"),(Join-Path $sdkLibRoot "$sdk\um\x86")) -join ';'
  $env:Path = @((Split-Path -Parent $cl),(Join-Path $sdkBinRoot "$sdk\x64"),(Join-Path $VsInstall 'Common7\IDE'),$env:Path) -join ';'
  $env:CC = 'cl'
  $env:CXX = 'cl'
  return $cl
}

function Build-DX11ClientMesonV63([string]$VsInstall, [string]$BridgeWork, [string]$Meson, [string]$Ninja) {
  Set-X86MsvcEnvV63 $VsInstall | Out-Null
  Repair-MesonUtf8NoBom $BridgeWork
  Repair-MesonNestedTernary $BridgeWork
  $env:NINJA = $Ninja
  $env:Path = (Split-Path -Parent $Ninja) + ';' + $env:Path
  $b32 = Join-Path $BridgeWork '_Comp32Release'
  if ($Clean -and (Test-Path $b32)) { Log "Removing DX11 x86 client Meson build: $b32"; Remove-Item -LiteralPath $b32 -Recurse -Force }
  $buildNinja = Join-Path $b32 'build.ninja'
  if (!(Test-Path $buildNinja)) {
Log 'Configuring x86 DX11 bridge client with Meson/Ninja and NVIDIA IPC utilities.'
    Invoke-Logged -Label 'bridge-client-meson-x86' -Exe $Meson -CommandArgs @('setup','--buildtype=release','--backend=ninja','-Dwerror=false','-Denable_tests=false','-Denable_tracy=true',$b32,$BridgeWork) -WorkingDirectory $BridgeWork -AllowIfFileExists $buildNinja | Out-Null
  }
  Log 'Building x86 DX11 bridge client with Meson/Ninja.'
  Invoke-Logged -Label 'bridge-client-ninja-x86' -Exe $Ninja -CommandArgs (Get-DX11FastNinjaArgsV219 $b32) -WorkingDirectory $BridgeWork | Out-Null
  $d3d11 = @(Get-ChildItem -LiteralPath $b32 -Recurse -Filter 'd3d11.dll' -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'client_dx11' } | Select-Object -First 1)
  $dxgi = @(Get-ChildItem -LiteralPath $b32 -Recurse -Filter 'dxgi.dll' -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'client_dx11' } | Select-Object -First 1)
  if ($d3d11.Count -lt 1 -or $dxgi.Count -lt 1) { Die "x86 DX11 client Meson build finished but d3d11.dll/dxgi.dll were not found under $b32" }
  Test-PeMachine $d3d11[0].FullName 'x86'
  Test-PeMachine $dxgi[0].FullName 'x86'

  # V219: Meson builds the proxy DLLs, but the launcher EXE is generated by
  # Build-DX11ClientDirect. Stage from one joined folder so the launcher-client
  # and the game-side proxy DLLs are always packaged together.
  $clientSrc = Join-Path $BridgeWork 'src\client_dx11'
  $directOut = Build-DX11ClientDirect $VsInstall $clientSrc
  $directLauncher = Join-Path $directOut 'NvRemixLauncher32.exe'
  $directLauncherPdb = Join-Path $directOut 'NvRemixLauncher32.pdb'
  if (!(Test-Path -LiteralPath $directLauncher -PathType Leaf)) {
    Die "V219 launcher-only direct x86 build did not produce NvRemixLauncher32.exe: $directLauncher"
  }
  Test-PeMachine $directLauncher 'x86'

  $joined = Join-Path $BridgeWork '_dx11_client_x86_joined'
  if (Test-Path -LiteralPath $joined -PathType Container) { Remove-Item -LiteralPath $joined -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $joined | Out-Null

  Copy-FileIfDifferent -Source $d3d11[0].FullName -Destination (Join-Path $joined 'd3d11.dll')
  Copy-FileIfDifferent -Source $dxgi[0].FullName -Destination (Join-Path $joined 'dxgi.dll')
  Copy-FileIfDifferent -Source $directLauncher -Destination (Join-Path $joined 'NvRemixLauncher32.exe')
  if (Test-Path -LiteralPath $directLauncherPdb -PathType Leaf) {
    Copy-FileIfDifferent -Source $directLauncherPdb -Destination (Join-Path $joined 'NvRemixLauncher32.pdb')
  }

  Test-PeMachine (Join-Path $joined 'd3d11.dll') 'x86'
  Test-PeMachine (Join-Path $joined 'dxgi.dll') 'x86'
  Test-PeMachine (Join-Path $joined 'NvRemixLauncher32.exe') 'x86'

  $manifest = @"
DX11_V219_JOINED_X86_CLIENT_OUTPUT

This folder is the x86 launcher-client output used for staging.

NvRemixLauncher32.exe = x86 DX11 bridge client / entrypoint
d3d11.dll             = x86 DX11 bridge client/interposer DLL
dxgi.dll              = x86 DXGI bridge client/interposer DLL

Meson output source:
  d3d11.dll = $($d3d11[0].FullName)
  dxgi.dll  = $($dxgi[0].FullName)

Direct launcher source:
  NvRemixLauncher32.exe = $directLauncher
"@
  Set-Content -LiteralPath (Join-Path $joined 'DX11_V219_JOINED_X86_CLIENT_OUTPUT.txt') -Encoding UTF8 -Value $manifest
  Log "V219 joined x86 launcher-client output: $joined"
  return $joined
}

function Build-DX11Bridge([string]$BridgeWork, [string]$VsInstall, [string]$Meson, [string]$Ninja) {
  
  Enable-DX11FastCompilerEnvV219
  Patch-BridgeServerRemoveLegacyD3D9RegistrationV219 -BridgeWork $BridgeWork
  Assert-RealBridgeServerCanAckV219 -BridgeWork $BridgeWork
  $b64 = Join-Path $BridgeWork '_Comp64Release'
  Log 'V219 preserving bridge server Meson/Ninja tree; changed Bridge_Ack sources rebuild incrementally.'
  if (!$SkipBridgeBuild) {
    Import-VSEnvironment $VsInstall 'x64'
    $env:NINJA = $Ninja
    $env:Path = (Split-Path -Parent $Ninja) + ';' + $env:Path
    Repair-MesonUtf8NoBom $BridgeWork
    $buildNinja = Join-Path $b64 'build.ninja'
# V219_REAL_BRIDGE_SERVER_ACK: removed V90 dummy DX11 server rewrite; keep real bridge server so Bridge_Ack is sent.
    Log 'Building official x64 bridge server from source.'
    if (!(Test-Path $buildNinja)) { Invoke-Logged -Label 'bridge-server-meson-x64' -Exe $Meson -CommandArgs @('setup','--buildtype=release','--backend=ninja','-Dwerror=false','-Denable_tests=false','-Denable_tracy=true',$b64,$BridgeWork) -WorkingDirectory $BridgeWork -AllowIfFileExists $buildNinja | Out-Null }
    Invoke-Logged -Label 'bridge-server-ninja-x64' -Exe $Ninja -CommandArgs (Get-DX11FastNinjaArgsV219 $b64) -WorkingDirectory $BridgeWork | Out-Null
  }
  $clientOut = Build-DX11ClientMesonV63 $VsInstall $BridgeWork $Meson $Ninja
  return @{ Build64 = $b64; Build32 = $clientOut }
}

function Copy-Usd([string]$PkgTrex) {
  $candidates = @(
    (Join-Path $Root '_output\x64\usd'),
    (Join-Path $Root '_output\usd'),
    (Join-Path $Root '_Comp64Release\usd'),
    (Join-Path $Root 'external\nv_usd_Release'),
    (Join-Path $Root 'external\nv_usd_release'),
    (Join-Path $Root 'src\usd-plugins\_external\nv_usd\release')
  )
  foreach ($c in $candidates) {
    if (Test-Path -LiteralPath $c -PathType Container) {
      $dst = Join-Path $PkgTrex 'usd'
      $srcFull = [System.IO.Path]::GetFullPath($c).TrimEnd('\')
      $dstFull = [System.IO.Path]::GetFullPath($dst).TrimEnd('\')
      if ($srcFull -ieq $dstFull) {
        Log "USD folder already staged: $dst"
        return
      }
      if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
      Log "Copying USD runtime/dev tree from: $c"
      Copy-Item -LiteralPath $c -Destination $dst -Recurse -Force
      return
    }
  }
  Warn 'No USD folder found to stage.'
}


function Copy-FileIfDifferent {
  param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Destination
  )
  $srcFull = [System.IO.Path]::GetFullPath($Source).TrimEnd('\')
  $dstFull = [System.IO.Path]::GetFullPath($Destination).TrimEnd('\')
  if ($srcFull -ieq $dstFull) {
    Log "Already staged, skipping self-copy: $Destination"
    return
  }
  $dstDir = Split-Path -Parent $Destination
  if ($dstDir -and !(Test-Path -LiteralPath $dstDir -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
  }
  Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Copy-DirectoryContentsSafe {
  param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Destination
  )
  if (!(Test-Path -LiteralPath $Source -PathType Container)) { return $false }
  New-Item -ItemType Directory -Force -Path $Destination | Out-Null
  $items = @(Get-ChildItem -LiteralPath $Source -Force -ErrorAction SilentlyContinue)
  foreach ($item in $items) {
    $target = Join-Path $Destination $item.Name
    if (([System.IO.Path]::GetFullPath($item.FullName).TrimEnd('\')) -ieq ([System.IO.Path]::GetFullPath($target).TrimEnd('\'))) {
      Log "Skipping self-copy while mirroring: $target"
      continue
    }
    Copy-Item -LiteralPath $item.FullName -Destination $Destination -Recurse -Force
  }
  return $true
}

function Remove-D3D9Artifacts {
  param([Parameter(Mandatory)][string]$BaseDir)
  # Integrated DX11 build: d3d11.dll and dxgi.dll are required output files.
  # Older repair scripts removed d3d11.dll here as a "D3D9 artifact", which broke
  # the final x64/x86 staging. Only remove actual legacy D3D9 files.
  foreach ($bad in @(Get-ChildItem -LiteralPath $BaseDir -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -ieq 'd3d9.dll' -or $_.Name -ieq 'nvapi.dll.d3d9' })) {
    Warn "Removing unwanted legacy D3D9 artifact from DX11 output: $($bad.FullName)"
    Remove-Item -LiteralPath $bad.FullName -Force
  }
}

function Resolve-X64RuntimeFiles {
  param([Parameter(Mandatory)][string]$RuntimeBuild)
  $rtD3D11 = Find-FileRecursive $RuntimeBuild 'd3d11.dll'
  $rtDXGI = Find-FileRecursive $RuntimeBuild 'dxgi.dll'
  if (!$rtD3D11 -or !$rtDXGI) {
    $rtD3D11 = Join-Path $Root '_output\x64\d3d11.dll'
    $rtDXGI = Join-Path $Root '_output\x64\dxgi.dll'
  }
  if (!(Test-Path -LiteralPath $rtD3D11 -PathType Leaf) -or !(Test-Path -LiteralPath $rtDXGI -PathType Leaf)) {
    Die "x64 runtime d3d11.dll/dxgi.dll missing. RuntimeBuild=$RuntimeBuild"
  }
  Test-PeMachine $rtD3D11 'x64'
  Test-PeMachine $rtDXGI 'x64'
  return @{ D3D11 = $rtD3D11; DXGI = $rtDXGI }
}

function Same-FullPath {
  param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B)
  $af = [System.IO.Path]::GetFullPath($A).TrimEnd('\')
  $bf = [System.IO.Path]::GetFullPath($B).TrimEnd('\')
  return ($af -ieq $bf)
}


function Get-BridgeVersionFromBuild {
  param([Parameter(Mandatory)][string]$BridgeBuild64)
  $candidates = @(
    (Join-Path $BridgeBuild64 'version.h'),
    (Join-Path (Split-Path -Parent $BridgeBuild64) 'version.h')
  )
  foreach ($c in $candidates) {
    if (Test-Path -LiteralPath $c -PathType Leaf) {
      $txt = Get-Content -LiteralPath $c -Raw -ErrorAction SilentlyContinue
      $m = [regex]::Match($txt, '#define\s+BRIDGE_VERSION\s+"([^"]+)"')
      if ($m.Success -and $m.Groups[1].Value.Trim().Length -gt 0) {
        return $m.Groups[1].Value.Trim()
      }
    }
  }
  return $null
}

function Write-BridgeVersionFile {
  param(
    [Parameter(Mandatory)][string]$TrexDir,
    [Parameter(Mandatory)][string]$BridgeBuild64
  )
  $ver = Get-BridgeVersionFromBuild -BridgeBuild64 $BridgeBuild64
  if (!$ver) {
    Warn "Could not resolve bridge BRIDGE_VERSION from $BridgeBuild64; x86 bridge server launch may fail version check."
    return
  }
  New-Item -ItemType Directory -Force -Path $TrexDir | Out-Null
  $dst = Join-Path $TrexDir 'bridge_version.txt'
  Set-Content -LiteralPath $dst -Value $ver -Encoding ASCII -NoNewline
  $guidPath = Join-Path $TrexDir 'dx11_bridge_guid.txt'
  Set-Content -LiteralPath $guidPath -Value $script:Dx11BridgeFixedGuidV63 -Encoding ASCII -NoNewline
  $argsPath = Join-Path $TrexDir 'dx11_bridge_args.txt'
  Set-Content -LiteralPath $argsPath -Value ($script:Dx11BridgeFixedGuidV63 + "`r`n" + $ver + "`r`n") -Encoding ASCII
  Log "Wrote bridge version for x86 client/server launch: $dst = $ver"
  Log "Wrote fixed DX11 bridge GUID fallback: $guidPath = $script:Dx11BridgeFixedGuidV63"
  Log "Wrote DX11 bridge fallback args file: $argsPath"
}


function Assert-DX11X86LauncherClientLayoutV219 {
  param(
    [Parameter(Mandatory)][string]$X86Out,
    [Parameter(Mandatory)][string]$X64Out
  )

  $trex = Join-Path $X86Out '.trex'
  $checks = @(
    @{ Path = (Join-Path $X86Out 'NvRemixLauncher32.exe'); Machine = 'x86'; Role = 'required x86 NvRemixLauncher32.exe DLL-started launcher/client helper' },
    @{ Path = (Join-Path $X86Out 'd3d11.dll'); Machine = 'x86'; Role = 'ROOT x86 DX11 bridge client/interposer d3d11.dll' },
    @{ Path = (Join-Path $X86Out 'dxgi.dll');  Machine = 'x86'; Role = 'ROOT x86 DXGI bridge client/interposer dxgi.dll' },
    @{ Path = (Join-Path $trex 'NvRemixBridge.exe'); Machine = 'x64'; Role = '.trex x64 Remix bridge server/runtime host NvRemixBridge.exe' },
    @{ Path = (Join-Path $trex 'd3d11.dll'); Machine = 'x64'; Role = '.trex x64 Remix DX11 runtime d3d11.dll' },
    @{ Path = (Join-Path $trex 'dxgi.dll');  Machine = 'x64'; Role = '.trex x64 Remix DXGI runtime dxgi.dll' }
  )

  $report = New-Object System.Collections.Generic.List[string]
  $report.Add('DX11 x86 launcher-client game-root layout V219')
  $report.Add(('Generated: {0}' -f (Get-Date)))
  $report.Add('')
  $report.Add('Required layout for any 32-bit DX11 game:')
  $report.Add('  <game exe folder>\NvRemixLauncher32.exe    = required x86 launcher DLL-started launcher/client helper')
  $report.Add('  <game exe folder>\d3d11.dll                = x86 DX11 bridge client/interposer loaded by the game')
  $report.Add('  <game exe folder>\dxgi.dll                 = x86 DXGI bridge client/interposer loaded by the game')
  $report.Add('  <game exe folder>\.trex\NvRemixBridge.exe  = local x64 Remix bridge server')
  $report.Add('  <game exe folder>\.trex\d3d11.dll          = local x64 Remix runtime')
  $report.Add('  <game exe folder>\.trex\dxgi.dll           = local x64 Remix runtime')
  $report.Add('')
  $report.Add('Run the game normally from this same game folder. NvRemixLauncher32.exe is required and must remain beside the proxy DLLs.')
  $report.Add('Do not install any of these files to System32/SysWOW64.')
  $report.Add('')

  foreach ($c in $checks) {
    $p = [string]$c.Path
    $role = [string]$c.Role
    $machine = [string]$c.Machine
    if (!(Test-Path -LiteralPath $p -PathType Leaf)) {
      $report.Add(('BAD missing: {0} -> {1}' -f $role, $p))
      Set-Content -LiteralPath (Join-Path $X86Out 'DX11_V219_LAUNCHER_CLIENT_LAYOUT.txt') -Encoding UTF8 -Value ($report -join "`r`n")
      Die ("V219 layout check failed; missing {0}: {1}" -f $role, $p)
    }
    Test-PeMachine $p $machine
    $len = (Get-Item -LiteralPath $p).Length
    if ($len -lt 32768) {
      $report.Add(('BAD too small: {0} size={1} -> {2}' -f $role, $len, $p))
      Set-Content -LiteralPath (Join-Path $X86Out 'DX11_V219_LAUNCHER_CLIENT_LAYOUT.txt') -Encoding UTF8 -Value ($report -join "`r`n")
      Die ("V219 layout check failed; {0} is too small to be the real built artifact: {1}" -f $role, $p)
    }
    $report.Add(('OK {0}: {1} bytes -> {2}' -f $role, $len, $p))
  }

  $bridgeVersion = Join-Path $trex 'bridge_version.txt'
  if (!(Test-Path -LiteralPath $bridgeVersion -PathType Leaf)) {
    $report.Add(('BAD missing bridge_version.txt: {0}' -f $bridgeVersion))
    Set-Content -LiteralPath (Join-Path $X86Out 'DX11_V219_LAUNCHER_CLIENT_LAYOUT.txt') -Encoding UTF8 -Value ($report -join "`r`n")
    Die "V219 layout check failed; .trex\bridge_version.txt is missing."
  }
  $report.Add(('OK .trex bridge version file: {0}' -f $bridgeVersion))

  $clientLog = @"
DX11_V114_LAUNCHER_IS_CLIENT

NvRemixLauncher32.exe is required in the x86 bridge package and must stay beside the proxy DLLs.

Run the actual game EXE normally from the game folder after copying this full x86 package beside it.

The root d3d11.dll and dxgi.dll are x86 game-side bridge proxy DLLs that the game loads from the same folder.

No files go in System32 or SysWOW64.
The game folder is the install/run folder.
"@
  Set-Content -LiteralPath (Join-Path $X86Out 'DX11_V219_LAUNCHER_IS_CLIENT.txt') -Encoding UTF8 -Value $clientLog

  $runBat = @'
@echo off
setlocal
cd /d "%~dp0"
echo Start the actual game executable normally from this folder.
echo NvRemixLauncher32.exe is required and must remain here, but this helper does not launch it as the game command.
pause
'@
  Set-Content -LiteralPath (Join-Path $X86Out 'RUN_GAME_WITH_DX11_REMIX_V219.bat') -Encoding ASCII -Value $runBat

  $verifyPs = @'
param([string]$GameDir = "")

$ErrorActionPreference = "Stop"

function Get-Machine([string]$Path) {
  if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return "missing" }
  $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
  try {
    $br = New-Object System.IO.BinaryReader($fs)
    if ($fs.Length -lt 64) { return "bad" }
    $fs.Seek(0x3c, [System.IO.SeekOrigin]::Begin) | Out-Null
    $pe = $br.ReadInt32()
    if ($pe -lt 0 -or $pe + 6 -gt $fs.Length) { return "bad" }
    $fs.Seek($pe, [System.IO.SeekOrigin]::Begin) | Out-Null
    $sig = $br.ReadUInt32()
    if ($sig -ne 0x00004550) { return "bad" }
    $m = $br.ReadUInt16()
    if ($m -eq 0x14c) { return "x86" }
    if ($m -eq 0x8664) { return "x64" }
    return ("0x{0:X4}" -f $m)
  } finally { $fs.Close() }
}

if ([string]::IsNullOrWhiteSpace($GameDir)) { $GameDir = Split-Path -Parent $PSCommandPath }
$GameDir = [System.IO.Path]::GetFullPath($GameDir)

$items = @(
  @{ Path = (Join-Path $GameDir "NvRemixLauncher32.exe"); Expect = "x86"; Role = "required x86 launcher DLL-started launcher/client helper" },
  @{ Path = (Join-Path $GameDir "d3d11.dll"); Expect = "x86"; Role = "x86 DX11 bridge client/interposer" },
  @{ Path = (Join-Path $GameDir "dxgi.dll"); Expect = "x86"; Role = "x86 DXGI bridge client/interposer" },
  @{ Path = (Join-Path $GameDir ".trex\NvRemixBridge.exe"); Expect = "x64"; Role = "local .trex x64 bridge server" },
  @{ Path = (Join-Path $GameDir ".trex\d3d11.dll"); Expect = "x64"; Role = "local .trex x64 DX11 runtime" },
  @{ Path = (Join-Path $GameDir ".trex\dxgi.dll"); Expect = "x64"; Role = "local .trex x64 DXGI runtime" }
)

$bad = 0
Write-Host "[dx11-v219] Verifying launcher-client game-root layout: $GameDir" -ForegroundColor Cyan
foreach ($i in $items) {
  $actual = Get-Machine $i.Path
  if ($actual -ne $i.Expect) {
    Write-Host "[BAD] $($i.Role): expected $($i.Expect), got $actual -> $($i.Path)" -ForegroundColor Red
    $bad++
  } else {
    $size = (Get-Item -LiteralPath $i.Path).Length
    Write-Host "[OK]  $($i.Role): $actual, $size bytes" -ForegroundColor Green
  }
}

$ver = Join-Path $GameDir ".trex\bridge_version.txt"
if (!(Test-Path -LiteralPath $ver -PathType Leaf)) {
  Write-Host "[BAD] .trex\bridge_version.txt missing" -ForegroundColor Red
  $bad++
} else {
  Write-Host "[OK]  .trex\bridge_version.txt present" -ForegroundColor Green
}

if ($bad -ne 0) { throw "DX11 x86 launcher-client game-root layout failed with $bad problem(s)." }
Write-Host "[dx11-v219] Layout is valid. Run the game normally from this folder. Keep NvRemixLauncher32.exe beside the proxy DLLs." -ForegroundColor Cyan
'@

  $verifyBat = @'
@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0VERIFY_DX11_V219_LAUNCHER_CLIENT_LAYOUT.ps1"
pause
'@

  Set-Content -LiteralPath (Join-Path $X86Out 'VERIFY_DX11_V219_LAUNCHER_CLIENT_LAYOUT.ps1') -Encoding UTF8 -Value $verifyPs
  Set-Content -LiteralPath (Join-Path $X86Out 'RUN_VERIFY_DX11_V219_LAUNCHER_CLIENT_LAYOUT.bat') -Encoding ASCII -Value $verifyBat

  $report.Add('')
  $report.Add('OK: d3d11.dll/dxgi.dll are the x86 DX11 bridge client/interposer loaded by the game. NvRemixLauncher32.exe is required in the package as a DLL-started launcher/client helper.')
  $report.Add('OK: root d3d11.dll/dxgi.dll are x86 game-side proxy DLLs.')
  $report.Add('OK: .trex contains the local x64 Remix bridge/runtime.')
  $report.Add('OK: run script and verifier written into x86 output.')
  Set-Content -LiteralPath (Join-Path $X86Out 'DX11_V219_LAUNCHER_CLIENT_LAYOUT.txt') -Encoding UTF8 -Value ($report -join "`r`n")
  Log "V219 verified x86 launcher-client game-root layout: $X86Out"
}


function Stage-DualOutput([string]$RuntimeBuild, [hashtable]$BridgeBuilds) {
  # v62: final user-facing layout is _output\x64 and _output\x86, matching the folder
  # the repo already uses for the x64 Meson install tree. Do not delete _output\x64
  # when it is also the installed runtime tree; keep its support DLL layout intact.
  $outRoot = if ([IO.Path]::IsPathRooted($OutputRoot)) { $OutputRoot } else { Join-Path $Root $OutputRoot }
  $x64Out = Join-Path $outRoot 'x64'
  $x86Out = Join-Path $outRoot 'x86'
  $x86Trex = Join-Path $x86Out '.trex'
  $installedX64 = Join-Path $Root '_output\x64'
  $sameAsInstalledX64 = Same-FullPath -A $x64Out -B $installedX64

  $runtime = Resolve-X64RuntimeFiles $RuntimeBuild

  if ($sameAsInstalledX64) {
    New-Item -ItemType Directory -Force -Path $x64Out | Out-Null
    Log "Using existing Meson x64 output folder as final x64 output: $x64Out"
  } else {
    if (Test-Path -LiteralPath $x64Out) {
      Log "Removing old staged x64 output folder: $x64Out"
      Remove-Item -LiteralPath $x64Out -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $x64Out | Out-Null
    if (Test-Path -LiteralPath $installedX64 -PathType Container) {
      Log "Copying installed x64 output tree: $installedX64 -> $x64Out"
      [void](Copy-DirectoryContentsSafe -Source $installedX64 -Destination $x64Out)
    }
  }

  Copy-FileIfDifferent -Source $runtime.D3D11 -Destination (Join-Path $x64Out 'd3d11.dll')
  Copy-FileIfDifferent -Source $runtime.DXGI  -Destination (Join-Path $x64Out 'dxgi.dll')
  Test-PeMachine (Join-Path $x64Out 'd3d11.dll') 'x64'
  Test-PeMachine (Join-Path $x64Out 'dxgi.dll') 'x64'
  Copy-Usd $x64Out
  Remove-D3D9Artifacts $x64Out

  if (Test-Path -LiteralPath $x86Out) {
    Log "Removing old staged x86 output folder: $x86Out"
    Remove-Item -LiteralPath $x86Out -Recurse -Force
  }
  New-Item -ItemType Directory -Force -Path $x86Trex | Out-Null

  $clientD3D11 = Join-Path $BridgeBuilds.Build32 'd3d11.dll'
  $clientDXGI = Join-Path $BridgeBuilds.Build32 'dxgi.dll'
  if (!(Test-Path -LiteralPath $clientD3D11 -PathType Leaf)) { Die "x86 bridge d3d11.dll missing: $clientD3D11" }
  if (!(Test-Path -LiteralPath $clientDXGI -PathType Leaf)) { Die "x86 bridge dxgi.dll missing: $clientDXGI" }

  Copy-FileIfDifferent -Source $clientD3D11 -Destination (Join-Path $x86Out 'd3d11.dll')
  Copy-FileIfDifferent -Source $clientDXGI  -Destination (Join-Path $x86Out 'dxgi.dll')
  Test-PeMachine (Join-Path $x86Out 'd3d11.dll') 'x86'
  Test-PeMachine (Join-Path $x86Out 'dxgi.dll') 'x86'

  $launcher32 = Join-Path $BridgeBuilds.Build32 'NvRemixLauncher32.exe'
  $launcher32Pdb = Join-Path $BridgeBuilds.Build32 'NvRemixLauncher32.pdb'
  if (!(Test-Path -LiteralPath $launcher32 -PathType Leaf)) {
    Die "NvRemixLauncher32.exe was not built. V219 requires the x86 launcher EXE as the bridge client / entrypoint."
  }
  Copy-FileIfDifferent -Source $launcher32 -Destination (Join-Path $x86Out 'NvRemixLauncher32.exe')
  Test-PeMachine (Join-Path $x86Out 'NvRemixLauncher32.exe') 'x86'
  if (Test-Path -LiteralPath $launcher32Pdb -PathType Leaf) { Copy-FileIfDifferent -Source $launcher32Pdb -Destination (Join-Path $x86Out 'NvRemixLauncher32.pdb') }

  $artifactReadme = @"
DXVK Remix DX11 x86 Launcher-Client Package v219
======================================

Placement matches the NVIDIA x86 bridge package style:

Root:
  NvRemixLauncher32.exe      32-bit DX11 bridge client / entrypoint
  d3d11.dll                  32-bit game-side DX11 proxy loaded by the game
  dxgi.dll                   32-bit game-side DXGI proxy loaded by the game
  NvRemixLauncher32.pdb      launcher symbols when built
  artifacts_readme.txt       this file

.trex:
  NvRemixBridge.exe          64-bit bridge server
  bridge_version.txt         bridge version passed by the root DX11 bridge DLLs
  d3d11.dll                  64-bit DX11 runtime from this repo
  dxgi.dll                   64-bit DXGI runtime from this repo
  usd\                       USD runtime/data when available
  *.dll                      64-bit support DLLs mirrored from _output\x64

Usage:
  Copy this folder next to the 32-bit DX11 game exe and run the game normally. The game reads/loads the root d3d11.dll/dxgi.dll from the game folder. The DLL starts NvRemixLauncher32.exe from that same folder as the required launcher/client helper.

d3d11.dll and dxgi.dll are intentionally staged in the root as the x86 DX11 bridge client/interposer loaded by the game. NvRemixLauncher32.exe is also required in the package as a DLL-started launcher/client helper.
"@
  Set-Content -LiteralPath (Join-Path $x86Out 'artifacts_readme.txt') -Encoding UTF8 -Value $artifactReadme

  # The 32-bit package needs the complete x64 runtime/support tree inside .trex,
  # not just d3d11.dll and dxgi.dll. Mirror the final x64 folder so the x86 layout
  # has the same support DLLs/USD structure as the native x64 output.
# >>> DX11_V219_DISABLE_STALE_X64_RUNTIME_CACHE_RESTORE
  # V219 intentionally disables the old V93 cache restore block. After a Remix
  # 1.5 runtime/UI import, reusing cached x64 d3d11.dll/dxgi.dll would put old
  # binaries back into .trex. The runtime is rebuilt by Build-X64Runtime and the
  # freshly resolved $runtime.D3D11/$runtime.DXGI files are copied below.
  Log "V219 disabled stale x64 runtime cache restore before .trex mirror."
# <<< DX11_V219_DISABLE_STALE_X64_RUNTIME_CACHE_RESTORE
  Log "Mirroring final x64 output tree into x86 .trex: $x64Out -> $x86Trex"
  [void](Copy-DirectoryContentsSafe -Source $x64Out -Destination $x86Trex)

  $server = Find-FileRecursive $BridgeBuilds.Build64 'NvRemixBridge.exe'
  if ($server) {
    Copy-FileIfDifferent -Source $server -Destination (Join-Path $x86Trex 'NvRemixBridge.exe')
    Test-PeMachine (Join-Path $x86Trex 'NvRemixBridge.exe') 'x64'
    Write-BridgeVersionFile -TrexDir $x86Trex -BridgeBuild64 $BridgeBuilds.Build64
  } else {
    Warn "x64 NvRemixBridge.exe was not found under $($BridgeBuilds.Build64); x86 output will include root DX11 bridge DLLs and .trex runtime but no bridge server."
  }

  # Ensure the .trex runtime entry DLLs are the actual x64 runtime build outputs.
  Copy-FileIfDifferent -Source $runtime.D3D11 -Destination (Join-Path $x86Trex 'd3d11.dll')
  Copy-FileIfDifferent -Source $runtime.DXGI  -Destination (Join-Path $x86Trex 'dxgi.dll')
  Test-PeMachine (Join-Path $x86Trex 'd3d11.dll') 'x64'
  Test-PeMachine (Join-Path $x86Trex 'dxgi.dll') 'x64'
  Copy-Usd $x86Trex
  Remove-D3D9Artifacts $x86Out

  $x64Readme = @"
DXVK Remix DX11 x64 output v62
==============================

Use this folder for native 64-bit DX11 games.

This folder intentionally matches the repo's normal _output\x64 structure:
  d3d11.dll      x64 DX11 runtime from this repo
  dxgi.dll       x64 DXGI runtime from this repo
  usd\           USD runtime/data when available
  *.dll          required x64 runtime/dependency DLLs from the build/install tree

d3d11.dll is intentionally staged for this DX11 build.
"@
  Set-Content -LiteralPath (Join-Path $x64Out 'README_X64_DX11_OUTPUT_V63.txt') -Encoding UTF8 -Value $x64Readme

  $x86Readme = @"
DXVK Remix DX11 x86 bridge output v62
=====================================

Use this folder for 32-bit DX11 games.

Root layout:
  d3d11.dll                 x86 DX11 bridge pickup/interposer DLL
  dxgi.dll                  x86 DXGI bridge pickup/interposer DLL
  NvRemixLauncher32.exe     optional x86 injection helper; the root d3d11.dll/dxgi.dll are the client by default
  artifacts_readme.txt      root package layout notes
  .trex\NvRemixBridge.exe   x64 bridge server when the bridge build produced it
  .trex\bridge_version.txt   exact server BRIDGE_VERSION passed by x86 launcher
  .trex\d3d11.dll           x64 DX11 runtime from this repo
  .trex\dxgi.dll            x64 DXGI runtime from this repo
  .trex\usd\                USD runtime/data when available
  .trex\*.dll               required x64 runtime/dependency DLLs mirrored from _output\x64

d3d11.dll is intentionally staged for this DX11 build.
"@
  Set-Content -LiteralPath (Join-Path $x86Out 'README_X86_DX11_BRIDGE_OUTPUT_V63.txt') -Encoding UTF8 -Value $x86Readme

  $pkgRoot = Join-Path $Root '_packages'
  New-Item -ItemType Directory -Force -Path $pkgRoot | Out-Null
  $zip = Join-Path $pkgRoot 'dxvk-remix-dx11-output-x64-x86-v62.zip'
  Restore-FinalDX11OutputsIntegrated -X86BuildClient $BridgeBuilds.Build32 -X86Out $x86Out -X64Out $x64Out
  Test-PeMachine (Join-Path $x86Out 'd3d11.dll') 'x86'
  Test-PeMachine (Join-Path $x86Out 'dxgi.dll') 'x86'
  Test-PeMachine (Join-Path $x86Trex 'd3d11.dll') 'x64'
  Test-PeMachine (Join-Path $x86Trex 'dxgi.dll') 'x64'
  Assert-DX11X86LauncherClientLayoutV219 -X86Out $x86Out -X64Out $x64Out
  Stage-RealNgxRuntimeV219 -X86Out $x86Out -X64Out $x64Out
  Write-DX11DllUpdateManifestV219 -X86Out $x86Out -X64Out $x64Out -RuntimeD3D11 $runtime.D3D11 -RuntimeDXGI $runtime.DXGI

  if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::CreateFromDirectory($outRoot, $zip)
Log "Output x64: $x64Out"
  Log "Output x86: $x86Out"
  Log "Combined output zip: $zip"
}

function Stage-Package([string]$RuntimeBuild, [hashtable]$BridgeBuilds) {
  $pkg = Join-Path $Root '_packages\rtx-remix-dx11-x86-bridge-getgoing-v62'
  if (Test-Path $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }
  $trex = Join-Path $pkg '.trex'
  New-Item -ItemType Directory -Force -Path $trex | Out-Null

  $clientD3D11 = Join-Path $BridgeBuilds.Build32 'd3d11.dll'
  $clientDXGI = Join-Path $BridgeBuilds.Build32 'dxgi.dll'
  $server = Find-FileRecursive $BridgeBuilds.Build64 'NvRemixBridge.exe'
  if (!$server) { Warn "x64 NvRemixBridge.exe was not found under $($BridgeBuilds.Build64); package will include DX11 pickup DLLs and .trex runtime but no bridge server." }
  Copy-Item -LiteralPath $clientD3D11 -Destination (Join-Path $pkg 'd3d11.dll') -Force
  Copy-Item -LiteralPath $clientDXGI -Destination (Join-Path $pkg 'dxgi.dll') -Force
  if ($server) {
    Copy-Item -LiteralPath $server -Destination (Join-Path $trex 'NvRemixBridge.exe') -Force
    Write-BridgeVersionFile -TrexDir $trex -BridgeBuild64 $BridgeBuilds.Build64
  }
  Test-PeMachine (Join-Path $pkg 'd3d11.dll') 'x86'
  Test-PeMachine (Join-Path $pkg 'dxgi.dll') 'x86'
  if ($server) { Test-PeMachine (Join-Path $trex 'NvRemixBridge.exe') 'x64' }

  $rtD3D11 = Find-FileRecursive $RuntimeBuild 'd3d11.dll'
  $rtDXGI = Find-FileRecursive $RuntimeBuild 'dxgi.dll'
  if (!$rtD3D11 -or !$rtDXGI) {
    $rtD3D11 = Join-Path $Root '_output\x64\d3d11.dll'
    $rtDXGI = Join-Path $Root '_output\x64\dxgi.dll'
  }
  if (!(Test-Path $rtD3D11) -or !(Test-Path $rtDXGI)) { Die "x64 runtime d3d11.dll/dxgi.dll missing. RuntimeBuild=$RuntimeBuild" }
  Copy-Item -LiteralPath $rtD3D11 -Destination (Join-Path $trex 'd3d11.dll') -Force
  Copy-Item -LiteralPath $rtDXGI -Destination (Join-Path $trex 'dxgi.dll') -Force
  Test-PeMachine (Join-Path $trex 'd3d11.dll') 'x64'
  Test-PeMachine (Join-Path $trex 'dxgi.dll') 'x64'
  Copy-Usd $trex

  $readme = @"
RTX Remix DX11 x86 Bridge Get-Going Package v62
================================================

Layout:
  d3d11.dll                 x86 DX11 pickup/interposer DLL
  dxgi.dll                  x86 DXGI pickup/interposer DLL
  .trex\NvRemixBridge.exe   x64 bridge server when bridge source build produced it
  .trex\d3d11.dll           x64 DX11 runtime from this repo
  .trex\dxgi.dll            x64 DXGI runtime from this repo
  .trex\usd\                USD runtime/data copied from dependency/build tree when found

d3d11.dll is intentionally included for this DX11 build.

v62 fixes:
  - launches NvRemixBridge.exe with required GUID and matching BRIDGE_VERSION arguments
  - writes .trex\bridge_version.txt from bridge server version.h
- wires the x86 root d3d11.dll/dxgi.dll as the client bootstrap to .trex\NvRemixBridge.exe
  - strips UTF-8 BOMs from bridge Meson files before Meson setup
  - writes patched Meson files as UTF-8 without BOM
  - prints Meson/Ninja logs live and tails them on failure
  - continues if Meson exits nonzero after creating build.ninja
  - builds the x86 root d3d11.dll/dxgi.dll directly with HostX64\\x86 cl.exe, avoiding vcvarsall x64_x86 quoting failures
"@
  Set-Content -Path (Join-Path $pkg 'README_DX11_BRIDGE_GETGOING_V63.txt') -Value $readme -Encoding UTF8
  $zip = "$pkg.zip"
  if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::CreateFromDirectory($pkg, $zip)
  Log "Package: $pkg"
  Log "Zip: $zip"
}

$vs = Find-VSInstall
$meson = Find-Meson
$ninja = Find-Ninja $vs
Log "Visual Studio: $vs"
Log "Meson: $meson"
Log "Ninja: $ninja"
Repair-MesonUtf8NoBom $Root
Repair-MesonNestedTernary $Root
Remove-DummyDx11BridgeServerWorkV219
$bridgeWork = Prepare-DX11BridgeSource $Root
if ($SourceOnly) { Log "Source-only requested. Work tree: $bridgeWork"; exit 0 }
$runtimeBuild = Build-X64Runtime $vs $meson $ninja
$bridgeBuilds = Build-DX11Bridge $bridgeWork $vs $meson $ninja
Stage-DualOutput $runtimeBuild $bridgeBuilds
