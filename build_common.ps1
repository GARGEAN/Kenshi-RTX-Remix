<#
  Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved.
  Permission is hereby granted, free of charge, to any person obtaining a
  copy of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom the
  Software is furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
  DEALINGS IN THE SOFTWARE.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# v9 concrete no-hang + USD plugin Packman + MSVC/Ninja fixes. Expected to live in the repository root next to meson.build.
$DxvkBuildRoot = if ($PSScriptRoot) {
  [IO.Path]::GetFullPath($PSScriptRoot)
} else {
  [IO.Path]::GetFullPath((Get-Location).Path)
}
function Get-NormalizedFullPath {
  param(
    [Parameter(Mandatory)]
    [string]$Path
  )
  return [IO.Path]::GetFullPath($Path).TrimEnd([char[]]@('\', '/')).ToLowerInvariant()
}
function Add-PathEntryForCurrentProcess {
  param(
    [Parameter(Mandatory)]
    [string]$PathToAdd,
    [switch]$Append
  )
  if ([string]::IsNullOrWhiteSpace($PathToAdd) -or -not (Test-Path $PathToAdd)) {
    return
  }
  $normalizedPathToAdd = Get-NormalizedFullPath -Path $PathToAdd
  $currentEntries = @()
  if ($env:Path) {
    $currentEntries = $env:Path -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
  }
  foreach ($entry in $currentEntries) {
    try {
      if ((Get-NormalizedFullPath -Path $entry) -eq $normalizedPathToAdd) {
        return
      }
    } catch {
    }
  }
  if ($Append) {
    $env:Path = "$env:Path;$PathToAdd"
  } else {
    $env:Path = "$PathToAdd;$env:Path"
  }
  Write-Host "[build] Added PATH entry for this build only: $PathToAdd" -ForegroundColor Yellow
}
function Add-PythonScriptDirsToPath {
  $candidateDirs = New-Object System.Collections.Generic.List[string]
  if ($env:APPDATA) {
    $roamingPythonRoot = Join-Path $env:APPDATA 'Python'
    if (Test-Path $roamingPythonRoot) {
      Get-ChildItem -Path $roamingPythonRoot -Directory -Filter 'Python*' -ErrorAction SilentlyContinue | ForEach-Object {
        $candidateDirs.Add((Join-Path $_.FullName 'Scripts'))
      }
    }
  }
  if ($env:LOCALAPPDATA) {
    $localProgramsPythonRoot = Join-Path $env:LOCALAPPDATA 'Programs\Python'
    if (Test-Path $localProgramsPythonRoot) {
      Get-ChildItem -Path $localProgramsPythonRoot -Directory -Filter 'Python*' -ErrorAction SilentlyContinue | ForEach-Object {
        $candidateDirs.Add((Join-Path $_.FullName 'Scripts'))
      }
    }
  }
  foreach ($pyName in @('py', 'python')) {
    $pyCmd = Get-Command $pyName -ErrorAction SilentlyContinue
    if (-not $pyCmd) { continue }
    try {
      $scriptsDir = & $pyCmd.Path -c "import sysconfig; print(sysconfig.get_path('scripts') or '')" 2>$null
      if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($scriptsDir)) {
        $candidateDirs.Add([string]$scriptsDir)
      }
    } catch {
    }
  }
  $seen = @{}
  foreach ($dir in $candidateDirs) {
    if ([string]::IsNullOrWhiteSpace($dir)) { continue }
    try {
      $full = [IO.Path]::GetFullPath($dir)
    } catch {
      continue
    }
    $key = $full.ToLowerInvariant()
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true
    Add-PathEntryForCurrentProcess -PathToAdd $full -Append
  }
}
function Resolve-RequiredCommand {
  param(
    [Parameter(Mandatory)]
    [string]$Name,
    [string]$InstallHint = '',
    # Optional Python module fallback. Example: mesonbuild.mesonmain lets the
    # script run Meson even when meson.exe was not put on PATH by pip.
    [string]$PythonModule = ''
  )
  if ($Name -ieq 'ninja' -and -not [string]::IsNullOrWhiteSpace($env:NINJA) -and (Test-Path -LiteralPath $env:NINJA -PathType Leaf)) {
    return [pscustomobject]@{
      FilePath = $env:NINJA
      ArgumentsPrefix = @()
      DisplayName = $env:NINJA
    }
  }
  Add-PythonScriptDirsToPath
  $cmd = Get-Command $Name -ErrorAction SilentlyContinue
  if ($cmd) {
    return [pscustomobject]@{
      FilePath = $cmd.Path
      ArgumentsPrefix = @()
      DisplayName = $cmd.Path
    }
  }
  if (-not [string]::IsNullOrWhiteSpace($PythonModule)) {
    foreach ($pyName in @('py', 'python')) {
      $pyCmd = Get-Command $pyName -ErrorAction SilentlyContinue
      if (-not $pyCmd) { continue }
      try {
        & $pyCmd.Path -m $PythonModule --version *> $null
        if ($LASTEXITCODE -eq 0) {
          $display = ('{0} -m {1}' -f $pyCmd.Path, $PythonModule)
          Write-Host "[build] Using Python module fallback for $Name`: $display" -ForegroundColor Yellow
          return [pscustomobject]@{
            FilePath = $pyCmd.Path
            ArgumentsPrefix = @('-m', $PythonModule)
            DisplayName = $display
          }
        }
      } catch {
      }
    }
  }
  $msg = "Required command '$Name' was not found in PATH or Python Scripts folders."
  if (-not [string]::IsNullOrWhiteSpace($InstallHint)) {
    $msg += " $InstallHint"
  }
  Write-Error $msg -ErrorAction Stop
}
function Quote-NativeArgument {
  param([AllowNull()][string]$Argument)
  if ($null -eq $Argument) { return '""' }
  if ($Argument.Length -eq 0) { return '""' }
  if ($Argument -notmatch '[\s"]') { return $Argument }
  $escaped = $Argument -replace '([\\]*)"', '$1$1\"'
  $escaped = $escaped -replace '([\\]+)$', '$1$1'
  return '"' + $escaped + '"'
}

function New-NativeCommandLine {
  param(
    [Parameter(Mandatory)][string]$FilePath,
    [string[]]$Arguments = @()
  )
  $parts = New-Object System.Collections.Generic.List[string]
  $parts.Add((Quote-NativeArgument $FilePath))
  foreach ($arg in $Arguments) {
    $parts.Add((Quote-NativeArgument ([string]$arg)))
  }
  return ($parts -join ' ')
}

function Stop-ProcessTreeSafe {
  param([Parameter(Mandatory)][int]$ProcessId)
  try {
    $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$ProcessId" -ErrorAction SilentlyContinue)
    foreach ($child in $children) {
      Stop-ProcessTreeSafe -ProcessId ([int]$child.ProcessId)
    }
  } catch {
  }
  try {
    Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
  } catch {
  }
}

function Get-TextTail {
  param(
    [AllowNull()][string]$Text,
    [int]$LineCount = 120
  )
  if ([string]::IsNullOrWhiteSpace($Text)) { return '' }
  $lines = [string[]]($Text -split "`r?`n")
  if ($lines.Count -le $LineCount) { return ($lines -join "`n") }
  return (($lines | Select-Object -Last $LineCount) -join "`n")
}

function Get-FileTail {
  param(
    [Parameter(Mandatory)][string]$Path,
    [int]$LineCount = 160
  )
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
  try {
    return ((Get-Content -LiteralPath $Path -Tail $LineCount -ErrorAction Stop) -join "`n")
  } catch {
    return "Could not read $Path`: $($_.Exception.Message)"
  }
}

function Invoke-CheckedNative {
  param(
    [Parameter(Mandatory)]
    [string]$FilePath,
    [Parameter(Mandatory)]
    [string[]]$Arguments,
    [string[]]$ArgumentsPrefix = @(),
    [Parameter(Mandatory)]
    [string]$FailureMessage,
    [string]$WorkingDirectory = $DxvkBuildRoot,
    # 0 means no wall-clock timeout. Any positive value kills the process tree
    # and exits with a clear error instead of leaving the batch apparently hung.
    [int]$TimeoutSeconds = 0,
    # Optional file to show when the native tool exits non-zero.
    [string]$DiagnosticLogPath = ''
  )
  $fullArguments = @()
  if ($ArgumentsPrefix) { $fullArguments += @($ArgumentsPrefix) }
  if ($Arguments) { $fullArguments += @($Arguments) }

  if (-not (Test-Path -LiteralPath $WorkingDirectory)) {
    Write-Host "[build] ERROR: Working directory does not exist: $WorkingDirectory" -ForegroundColor Red
    exit 1
  }

  $nativeCommandLine = New-NativeCommandLine -FilePath $FilePath -Arguments $fullArguments
  Write-Host ("[build] cmd> {0}" -f $nativeCommandLine) -ForegroundColor DarkGray

  $logRoot = Join-Path $DxvkBuildRoot '_build_logs'
  try { New-Item -ItemType Directory -Path $logRoot -Force | Out-Null } catch {}
  $safeName = ([regex]::Replace($FailureMessage.ToLowerInvariant(), '[^a-z0-9]+', '-')).Trim('-')
  if ([string]::IsNullOrWhiteSpace($safeName)) { $safeName = 'native-command' }
  $stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
  $stdoutPath = Join-Path $logRoot ("{0}-{1}.stdout.log" -f $stamp, $safeName)
  $stderrPath = Join-Path $logRoot ("{0}-{1}.stderr.log" -f $stamp, $safeName)
  $mergedPath = Join-Path $logRoot ("{0}-{1}.merged.log" -f $stamp, $safeName)
  try {
    Set-Content -LiteralPath $mergedPath -Value ("Command: {0}`r`nWorkingDirectory: {1}`r`n" -f $nativeCommandLine, $WorkingDirectory) -Encoding UTF8
    Set-Content -LiteralPath $stdoutPath -Value '' -Encoding UTF8
    Set-Content -LiteralPath $stderrPath -Value '' -Encoding UTF8
  } catch {}

  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $fileExt = [IO.Path]::GetExtension($FilePath).ToLowerInvariant()
  if ($fileExt -eq '.cmd' -or $fileExt -eq '.bat') {
    # .cmd/.bat files require cmd.exe when UseShellExecute is false.
    $psi.FileName = $env:ComSpec
    $psi.Arguments = '/d /s /c "' + $nativeCommandLine + '"'
  } else {
    # Windows PowerShell 5.1 compatibility: do not use ProcessStartInfo.ArgumentList.
    $psi.FileName = $FilePath
    $psi.Arguments = (@($fullArguments) | ForEach-Object { Quote-NativeArgument ([string]$_) }) -join ' '
  }
  $psi.WorkingDirectory = $WorkingDirectory
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.CreateNoWindow = $true

  $proc = New-Object System.Diagnostics.Process
  $proc.StartInfo = $psi
  $stdout = New-Object System.Text.StringBuilder
  $stderr = New-Object System.Text.StringBuilder

  $outHandler = [System.Diagnostics.DataReceivedEventHandler] {
    param($sender, $eventArgs)
    if ($null -ne $eventArgs.Data) {
      [void]$stdout.AppendLine($eventArgs.Data)
      try { Add-Content -LiteralPath $stdoutPath -Value $eventArgs.Data -ErrorAction SilentlyContinue } catch {}
      try { Add-Content -LiteralPath $mergedPath -Value $eventArgs.Data -ErrorAction SilentlyContinue } catch {}
      Write-Host $eventArgs.Data
    }
  }
  $errHandler = [System.Diagnostics.DataReceivedEventHandler] {
    param($sender, $eventArgs)
    if ($null -ne $eventArgs.Data) {
      [void]$stderr.AppendLine($eventArgs.Data)
      try { Add-Content -LiteralPath $stderrPath -Value $eventArgs.Data -ErrorAction SilentlyContinue } catch {}
      try { Add-Content -LiteralPath $mergedPath -Value $eventArgs.Data -ErrorAction SilentlyContinue } catch {}
      Write-Host $eventArgs.Data -ForegroundColor Red
    }
  }

  [void]$proc.add_OutputDataReceived($outHandler)
  [void]$proc.add_ErrorDataReceived($errHandler)

  try {
    if (-not $proc.Start()) {
      Write-Host "[build] ERROR: Failed to start native command." -ForegroundColor Red
      Write-Host "[build] Command: $nativeCommandLine" -ForegroundColor Yellow
      exit 1
    }
    $proc.BeginOutputReadLine()
    $proc.BeginErrorReadLine()

    if ($TimeoutSeconds -gt 0) {
      $finished = $proc.WaitForExit([int]($TimeoutSeconds * 1000))
      if (-not $finished) {
        Write-Host ("[build] ERROR: command timed out after {0} seconds; killing process tree." -f $TimeoutSeconds) -ForegroundColor Red
        Stop-ProcessTreeSafe -ProcessId $proc.Id
        Start-Sleep -Milliseconds 200
        $stdoutText = [string]$stdout.ToString()
        $stderrText = [string]$stderr.ToString()
        $tail = Get-TextTail -Text ($stdoutText + "`n" + $stderrText) -LineCount 180
        if (-not [string]::IsNullOrWhiteSpace($tail)) {
          Write-Host "[build] Last command output before timeout:" -ForegroundColor Yellow
          Write-Host $tail
        }
        Write-Host "[build] Full command log: $mergedPath" -ForegroundColor Yellow
        exit 1
      }
    } else {
      $proc.WaitForExit()
    }

    # Let async output handlers flush before checking ExitCode.
    $proc.WaitForExit()
    Start-Sleep -Milliseconds 200
    $exitCode = $proc.ExitCode
    if ($exitCode -ne 0) {
      $stdoutText = [string]$stdout.ToString()
      $stderrText = [string]$stderr.ToString()
      if ([string]::IsNullOrWhiteSpace($stdoutText) -and (Test-Path -LiteralPath $stdoutPath)) {
        $stdoutText = Get-Content -LiteralPath $stdoutPath -Raw -ErrorAction SilentlyContinue
      }
      if ([string]::IsNullOrWhiteSpace($stderrText) -and (Test-Path -LiteralPath $stderrPath)) {
        $stderrText = Get-Content -LiteralPath $stderrPath -Raw -ErrorAction SilentlyContinue
      }

      Write-Host ("[build] ERROR: {0} (exit code {1})" -f $FailureMessage, $exitCode) -ForegroundColor Red
      Write-Host "[build] Command: $nativeCommandLine" -ForegroundColor Yellow
      Write-Host "[build] Full command log: $mergedPath" -ForegroundColor Yellow
      Write-Host "[build] stdout log: $stdoutPath" -ForegroundColor DarkYellow
      Write-Host "[build] stderr log: $stderrPath" -ForegroundColor DarkYellow

      $commandTail = Get-TextTail -Text ($stdoutText + "`n" + $stderrText) -LineCount 220
      if (-not [string]::IsNullOrWhiteSpace($commandTail)) {
        Write-Host "[build] ---- command output tail ----" -ForegroundColor Yellow
        Write-Host $commandTail
      }
      if (-not [string]::IsNullOrWhiteSpace($DiagnosticLogPath)) {
        $diagTail = Get-FileTail -Path $DiagnosticLogPath -LineCount 260
        if (-not [string]::IsNullOrWhiteSpace($diagTail)) {
          Write-Host "[build] ---- diagnostic log tail: $DiagnosticLogPath ----" -ForegroundColor Yellow
          Write-Host $diagTail
        } else {
          Write-Host "[build] Diagnostic log missing or empty: $DiagnosticLogPath" -ForegroundColor Yellow
        }
      }
      exit 1
    }
  } finally {
    try { $proc.remove_OutputDataReceived($outHandler) } catch {}
    try { $proc.remove_ErrorDataReceived($errHandler) } catch {}
    try { $proc.Dispose() } catch {}
  }
}

function Get-MesonOptionNames {
  param(
    [Parameter(Mandatory)]
    [string]$SourceDir
  )
  $optionFile = Join-Path $SourceDir 'meson_options.txt'
  if (-not (Test-Path $optionFile)) {
    Write-Host "[build] WARNING: meson_options.txt was not found. User Meson -D options will be skipped." -ForegroundColor Yellow
    return @()
  }
  $text = Get-Content -Path $optionFile -Raw
  $matches = [regex]::Matches($text, 'option\(\s*[''"]([^''"]+)[''"]')
  $names = @()
  foreach ($m in $matches) {
    $names += $m.Groups[1].Value
  }
  return $names
}
function New-MesonOptionArg {
  param(
    [Parameter(Mandatory)]
    [string[]]$AvailableOptions,
    [Parameter(Mandatory)]
    [string]$Name,
    [Parameter(Mandatory)]
    [string]$Value
  )
  if ($AvailableOptions -contains $Name) {
    return "-D$Name=$Value"
  }
  Write-Host "[build] WARNING: Meson option '$Name' is not declared in meson_options.txt; not passing -D$Name=$Value" -ForegroundColor Yellow
  return $null
}
function Get-ConfiguredMesonSourceDir {
  param(
    [Parameter(Mandatory)]
    [string]$BuildDir
  )
  $mesonInfoPath = Join-Path $BuildDir 'meson-info\meson-info.json'
  if (Test-Path $mesonInfoPath) {
    try {
      $mesonInfo = Get-Content $mesonInfoPath -Raw | ConvertFrom-Json
      if ($mesonInfo.directories.source) {
        return $mesonInfo.directories.source
      }
    } catch {
      Write-Host "[build] WARNING: Could not read $mesonInfoPath; build directory will be regenerated." -ForegroundColor Yellow
    }
  }
  $buildNinjaPath = Join-Path $BuildDir 'build.ninja'
  if (Test-Path $buildNinjaPath) {
    $buildNinja = Get-Content $buildNinjaPath -Raw
    $regexes = @(
      [regex]'--internal regenerate\s+"([^"]+)"\s+"\."',
      [regex]'--internal regenerate\s+([^\s]+)\s+\.'
    )
    foreach ($regex in $regexes) {
      $match = $regex.Match($buildNinja)
      if ($match.Success) {
        return $match.Groups[1].Value
      }
    }
  }
  return $null
}
function Repair-FutureFileTimestamps {
  param(
    [Parameter(Mandatory)]
    [string]$SourceDir,
    [string]$BuildDir = ''
  )
  # No-hang timestamp repair: older versions recursively scanned the whole repo,
  # which can look frozen on a large Remix checkout with populated external\ deps.
  # Meson's clock-skew failure is normally caused by root Meson/build metadata, so
  # only touch the small set of files/directories Meson/Ninja actually inspect for
  # regeneration decisions.
  $now = Get-Date
  $futureLimit = $now.AddSeconds(2)
  $safeTime = $now.AddSeconds(-60)
  $fixedCount = 0
  $candidateFiles = New-Object System.Collections.Generic.List[string]

  foreach ($relative in @(
    'meson.build',
    'meson_options.txt',
    'build_common.ps1',
    'build_dxvk_all_ninja.ps1',
    'build.bat',
    'packman-external.xml'
  )) {
    $candidate = Join-Path $SourceDir $relative
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      $candidateFiles.Add([IO.Path]::GetFullPath($candidate))
    }
  }

  foreach ($smallDir in @(
    (Join-Path $SourceDir 'scripts-common'),
    (Join-Path $SourceDir 'scripts')
  )) {
    if (Test-Path -LiteralPath $smallDir -PathType Container) {
      try {
        Get-ChildItem -LiteralPath $smallDir -File -Force -ErrorAction SilentlyContinue | ForEach-Object {
          $candidateFiles.Add($_.FullName)
        }
      } catch {
        Write-Host "[build] WARNING: Could not scan '$smallDir' for timestamp repair: $($_.Exception.Message)" -ForegroundColor Yellow
      }
    }
  }

  if (-not [string]::IsNullOrWhiteSpace($BuildDir) -and (Test-Path -LiteralPath $BuildDir -PathType Container)) {
    foreach ($relative in @('build.ninja', 'meson-info', 'meson-private')) {
      $candidate = Join-Path $BuildDir $relative
      if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $candidateFiles.Add([IO.Path]::GetFullPath($candidate))
      } elseif (Test-Path -LiteralPath $candidate -PathType Container) {
        try {
          Get-ChildItem -LiteralPath $candidate -File -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
            $candidateFiles.Add($_.FullName)
          }
        } catch {
          Write-Host "[build] WARNING: Could not scan '$candidate' for timestamp repair: $($_.Exception.Message)" -ForegroundColor Yellow
        }
      }
    }
  }

  $seenFiles = @{}
  foreach ($path in $candidateFiles) {
    if ([string]::IsNullOrWhiteSpace($path)) { continue }
    $key = $path.ToLowerInvariant()
    if ($seenFiles.ContainsKey($key)) { continue }
    $seenFiles[$key] = $true
    try {
      $item = Get-Item -LiteralPath $path -Force -ErrorAction Stop
      if (($item.LastWriteTime -gt $futureLimit) -or ($item.LastAccessTime -gt $futureLimit) -or ($item.CreationTime -gt $futureLimit)) {
        $deltaSeconds = [math]::Round(($item.LastWriteTime - $now).TotalSeconds, 3)
        Write-Host "[build] Fixing future timestamp (+$deltaSeconds sec): $($item.FullName)" -ForegroundColor Yellow
        $item.CreationTime = $safeTime
        $item.LastWriteTime = $safeTime
        $item.LastAccessTime = $safeTime
        $fixedCount++
      }
    } catch {
      Write-Host "[build] WARNING: Could not repair timestamp for '$path': $($_.Exception.Message)" -ForegroundColor Yellow
    }
  }

  if ($fixedCount -gt 0) {
    Write-Host "[build] Repaired $fixedCount future Meson/Ninja timestamp(s)." -ForegroundColor Yellow
  }
}

function Repair-StaleBuildDirectory {
  param(
    [Parameter(Mandatory)]
    [string]$SourceDir,
    [Parameter(Mandatory)]
    [string]$BuildDir
  )
  if (-not (Test-Path $BuildDir)) {
    return $false
  }
  $configuredSourceDir = Get-ConfiguredMesonSourceDir -BuildDir $BuildDir
  if ([string]::IsNullOrWhiteSpace($configuredSourceDir)) {
    Write-Host "[build] Removing partial or unreadable Meson build directory: $BuildDir" -ForegroundColor Yellow
    Remove-Item $BuildDir -Recurse -Force
    return $false
  }
  $normalizedSourceDir = Get-NormalizedFullPath -Path $SourceDir
  $normalizedConfiguredSourceDir = Get-NormalizedFullPath -Path $configuredSourceDir
  if ($normalizedSourceDir -ne $normalizedConfiguredSourceDir) {
    Write-Host "[build] Removing stale Meson build directory '$BuildDir' configured for '$configuredSourceDir'" -ForegroundColor Yellow
    Remove-Item $BuildDir -Recurse -Force
    return $false
  }
  return $true
}

$script:DxvkDependenciesChecked = $false

function Get-DxvkMissingUsdPluginDependencies {
  param([Parameter(Mandatory)][string]$SourceDir)
  $expected = @(
    @{ Relative = 'src\usd-plugins\_external\nv_usd\release\lib'; Description = 'USD plugin release libs' },
    @{ Relative = 'src\usd-plugins\_external\nv_usd\debug\lib';   Description = 'USD plugin debug libs' },
    @{ Relative = 'src\usd-plugins\_external\python\python.exe';   Description = 'USD plugin Python 3.10 runtime' }
  )
  $missing = @()
  foreach ($entry in $expected) {
    $full = Join-Path $SourceDir $entry.Relative
    if (-not (Test-Path -LiteralPath $full)) {
      $missing += $entry.Relative
    }
  }
  return $missing
}

function Repair-StaleUsdPluginExternalPath {
  param(
    [Parameter(Mandatory)][string]$Path,
    [Parameter(Mandatory)][string]$RequiredChild,
    [bool]$PreparingForPackman = $false
  )
  if (-not (Test-Path -LiteralPath $Path)) {
    return
  }
  $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
  $isReparsePoint = (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
  $required = Join-Path $Path $RequiredChild
  $requiredExists = Test-Path -LiteralPath $required

  # Packman wants to create links at these paths. If a normal directory/file is
  # already there, Packman throws E001: "exists as a different type of file".
  # When preparing for a Packman pull, move any non-link object out of the way.
  if ((-not $PreparingForPackman) -and $requiredExists) {
    return
  }
  if ($PreparingForPackman -and $isReparsePoint -and $requiredExists) {
    return
  }

  $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
  $backup = "$Path.stale_$stamp"
  Write-Host "[build] Repairing USD plugin Packman path: $Path" -ForegroundColor Yellow
  try {
    Move-Item -LiteralPath $Path -Destination $backup -Force -ErrorAction Stop
    Write-Host "[build] Moved stale/conflicting path to: $backup" -ForegroundColor Yellow
  } catch {
    Write-Host "[build] Could not move stale/conflicting path, removing it instead: $($_.Exception.Message)" -ForegroundColor Yellow
    Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
  }
}

function Repair-StaleUsdPluginPackmanLinks {
  param(
    [Parameter(Mandatory)][string]$SourceDir,
    [bool]$PreparingForPackman = $false
  )
  $usdExternalRoot = Join-Path $SourceDir 'src\usd-plugins\_external'
  $nvUsdRoot = Join-Path $usdExternalRoot 'nv_usd'
  Repair-StaleUsdPluginExternalPath -Path (Join-Path $nvUsdRoot 'release') -RequiredChild 'lib' -PreparingForPackman $PreparingForPackman
  Repair-StaleUsdPluginExternalPath -Path (Join-Path $nvUsdRoot 'debug') -RequiredChild 'lib' -PreparingForPackman $PreparingForPackman
  Repair-StaleUsdPluginExternalPath -Path (Join-Path $usdExternalRoot 'python') -RequiredChild 'python.exe' -PreparingForPackman $PreparingForPackman
}

function Patch-UsdPluginsMesonSkipPackman {
  param([Parameter(Mandatory)][string]$SourceDir)
  $usdMeson = Join-Path $SourceDir 'src\usd-plugins\meson.build'
  if (-not (Test-Path -LiteralPath $usdMeson -PathType Leaf)) {
    return
  }
  $text = Get-Content -LiteralPath $usdMeson -Raw
  if ($text -match 'USD-SCHEMA-PLUGINS: Skipping nested Packman fetch') {
    return
  }
  if ($text -notmatch 'USD-SCHEMA-PLUGINS: Downloading USD and other dependencies') {
    Write-Host "[build] WARNING: Could not find USD plugin Packman block in $usdMeson; leaving it unchanged." -ForegroundColor Yellow
    return
  }

  $pattern = "(?s)message\('USD-SCHEMA-PLUGINS: Downloading USD and other dependencies\.\.\.'\)\s+packman_result\s*=\s*run_command\(.*?if\s+packman_result\.returncode\(\)\s*!=\s*0\s+error\('USD-SCHEMA-PLUGINS: packman failed: '\s*\+\s*packman_result\.stderr\(\)\.strip\(\)\)\s+endif"
  $replacement = @'
if get_option('skip_packman_fetch')
  message('USD-SCHEMA-PLUGINS: Skipping nested Packman fetch; build wrapper already checked/fetched plugin dependencies.')
else
  message('USD-SCHEMA-PLUGINS: Downloading USD and other dependencies...')
  packman_result = run_command(
    join_paths(meson.current_source_dir(), '../../scripts-common/packman/packman.cmd'),
    'pull',
    join_paths(meson.current_source_dir(), 'packman-external.xml'),
    '-p',
    'windows-x86_64',
    check: true,
  )
  if packman_result.returncode() != 0
    error('USD-SCHEMA-PLUGINS: packman failed: ' + packman_result.stderr().strip())
  endif
endif
'@
  $patched = [regex]::Replace($text, $pattern, $replacement, 1)
  if ($patched -eq $text) {
    Write-Host "[build] WARNING: Regex did not patch USD plugin Meson Packman block in $usdMeson." -ForegroundColor Yellow
    return
  }
  $backup = "$usdMeson.bak_v5_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
  Copy-Item -LiteralPath $usdMeson -Destination $backup -Force
  Set-Content -LiteralPath $usdMeson -Value $patched -Encoding UTF8
  Write-Host "[build] Patched USD plugin Meson to respect -Dskip_packman_fetch=true." -ForegroundColor Green
}

function Ensure-UsdPluginDependencies {
  param(
    [Parameter(Mandatory)][string]$SourceDir,
    [bool]$FetchDependencies = $true,
    [int]$TimeoutSeconds = 900
  )
  Patch-UsdPluginsMesonSkipPackman -SourceDir $SourceDir
  Repair-StaleUsdPluginPackmanLinks -SourceDir $SourceDir

  $missing = @(Get-DxvkMissingUsdPluginDependencies -SourceDir $SourceDir)
  if ($missing.Count -eq 0) {
    Write-Host "[build] USD plugin dependencies are already present; skipping nested packman fetch." -ForegroundColor Cyan
    return
  }

  Write-Host ("[build] Missing USD plugin dependencies: {0}" -f ($missing -join ', ')) -ForegroundColor Yellow
  if (-not $FetchDependencies) {
    Write-Host "[build] Dependency fetch disabled by -NoDepsFetch; USD plugin configuration may fail." -ForegroundColor Yellow
    return
  }

  $packmanCmd = Join-Path $SourceDir 'scripts-common\packman\packman.cmd'
  $usdPluginXml = Join-Path $SourceDir 'src\usd-plugins\packman-external.xml'
  $usdPluginDir = Join-Path $SourceDir 'src\usd-plugins'
  if (-not (Test-Path -LiteralPath $packmanCmd -PathType Leaf)) {
    throw "USD plugin Packman script missing: $packmanCmd"
  }
  if (-not (Test-Path -LiteralPath $usdPluginXml -PathType Leaf)) {
    throw "USD plugin dependency manifest missing: $usdPluginXml"
  }

  Repair-StaleUsdPluginPackmanLinks -SourceDir $SourceDir -PreparingForPackman $true
  Write-Host ("[build] Fetching USD plugin dependencies with timeout: {0} seconds" -f $TimeoutSeconds) -ForegroundColor Cyan
  Invoke-CheckedNative -FilePath $packmanCmd -Arguments @('pull', $usdPluginXml, '-p', 'windows-x86_64') -FailureMessage 'Failed to fetch USD plugin dependencies with packman' -WorkingDirectory $usdPluginDir -TimeoutSeconds $TimeoutSeconds

  $stillMissing = @(Get-DxvkMissingUsdPluginDependencies -SourceDir $SourceDir)
  if ($stillMissing.Count -gt 0) {
    throw ("USD plugin Packman finished, but these dependency paths are still missing: {0}" -f ($stillMissing -join ', '))
  }
  Write-Host "[build] USD plugin dependency fetch completed." -ForegroundColor Green
}

function Get-DxvkMissingExternalDependencies {
  param([Parameter(Mandatory)][string]$SourceDir)
  $expected = @(
    'external\aftermath',
    'external\reflex',
    'external\rtxio',
    'external\nv_usd_release',
    'external\nv_usd_debug',
    'external\glslangvalidator',
    'external\slang',
    'external\spirv_tools',
    'external\fonts',
    'external\omni_core_materials',
    'external\nv_xxd'
  )
  $missing = @()
  foreach ($relative in $expected) {
    $full = Join-Path $SourceDir $relative
    $hasContent = $false
    if (Test-Path -LiteralPath $full) {
      try {
        $hasContent = $null -ne (Get-ChildItem -LiteralPath $full -Force -ErrorAction SilentlyContinue | Select-Object -First 1)
      } catch {
        $hasContent = $false
      }
    }
    if (-not $hasContent) { $missing += $relative }
  }
  return $missing
}

function Ensure-DxvkDependencies {
  param(
    [Parameter(Mandatory)][string]$SourceDir,
    [bool]$FetchDependencies = $true,
    [int]$TimeoutSeconds = 900
  )
  if ($script:DxvkDependenciesChecked) { return }
  $script:DxvkDependenciesChecked = $true

  $missing = @(Get-DxvkMissingExternalDependencies -SourceDir $SourceDir)
  if ($missing.Count -eq 0) {
    Write-Host "[build] External dependencies are already present; skipping packman fetch." -ForegroundColor Cyan
    Ensure-UsdPluginDependencies -SourceDir $SourceDir -FetchDependencies $FetchDependencies -TimeoutSeconds $TimeoutSeconds
    return
  }

  Write-Host ("[build] Missing external dependencies: {0}" -f ($missing -join ', ')) -ForegroundColor Yellow
  if (-not $FetchDependencies) {
    Write-Host "[build] Dependency fetch disabled by -NoDepsFetch; Meson will continue but compile may fail if these are required." -ForegroundColor Yellow
    return
  }

  $updateDeps = Join-Path $SourceDir 'scripts-common\update-deps.cmd'
  $packmanXml = Join-Path $SourceDir 'packman-external.xml'
  if (-not (Test-Path -LiteralPath $updateDeps)) {
    throw "Dependency fetch script missing: $updateDeps"
  }
  if (-not (Test-Path -LiteralPath $packmanXml)) {
    throw "Dependency manifest missing: $packmanXml"
  }

  Write-Host ("[build] Fetching dependencies with timeout: {0} seconds" -f $TimeoutSeconds) -ForegroundColor Cyan
  Invoke-CheckedNative -FilePath $updateDeps -Arguments @($packmanXml, 'update-deps.log') -FailureMessage 'Failed to fetch external dependencies with packman' -WorkingDirectory $SourceDir -TimeoutSeconds $TimeoutSeconds

  $stillMissing = @(Get-DxvkMissingExternalDependencies -SourceDir $SourceDir)
  if ($stillMissing.Count -gt 0) {
    throw ("packman finished, but these dependency folders are still missing/empty: {0}`nCheck update-deps.log in the repo root." -f ($stillMissing -join ', '))
  }
  Write-Host "[build] Dependency fetch completed." -ForegroundColor Green
  Ensure-UsdPluginDependencies -SourceDir $SourceDir -FetchDependencies $FetchDependencies -TimeoutSeconds $TimeoutSeconds
}

# Find vswhere (installed with recent Visual Studio versions).
if ($vsWhereCommand = Get-Command 'vswhere.exe' -ErrorAction SilentlyContinue) {
  $vsWhere = $vsWhereCommand.Path
} elseif (Test-Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe") {
  $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
} else {
  Write-Error "vswhere not found. Install Visual Studio 2019/2022 with C++ build tools, or add vswhere.exe to PATH." -ErrorAction Stop
}
Write-Host "[build] vswhere found at: $vsWhere" -ForegroundColor Yellow
# Get path to Visual Studio installation using vswhere.
$vsWherePrimaryArgs = @(
  '-latest',
  '-version', '[16.0,18.0)',
  '-products', '*',
  '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
  '-property', 'installationPath'
)
$vsPath = (& $vsWhere @vsWherePrimaryArgs 2>$null | Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace([string]$vsPath)) {
  $vsWhereFallbackArgs = @(
    '-latest',
    '-version', '[16.0,18.0)',
    '-products', '*',
    '-requires', 'Microsoft.Component.MSBuild',
    '-property', 'installationPath'
  )
  $vsPath = (& $vsWhere @vsWhereFallbackArgs 2>$null | Select-Object -First 1)
}
if (-not [string]::IsNullOrWhiteSpace([string]$vsPath)) {
  $vsPath = ([string]$vsPath).Trim()
}
if ([string]::IsNullOrWhiteSpace([string]$vsPath)) {
  Write-Error "Failed to find Visual Studio 2019/2022 with MSVC C++ build tools. Aborting." -ErrorAction Stop
}
Write-Host "[build] Using Visual Studio installation at: ${vsPath}" -ForegroundColor Yellow
# Capture the environment before selecting a compiler architecture. Each build can
# switch between x64 and x86 without reusing the previous architecture's CL/LIB/INCLUDE.
$script:DxvkInitialEnvironment = @{}
Get-ChildItem Env: | ForEach-Object { $script:DxvkInitialEnvironment[$_.Name] = $_.Value }

function Restore-DxvkInitialEnvironment {
  $currentNames = @(Get-ChildItem Env: | ForEach-Object { $_.Name })
  foreach ($name in $currentNames) {
    if (-not $script:DxvkInitialEnvironment.ContainsKey($name)) {
      Remove-Item -Path "ENV:\$name" -ErrorAction SilentlyContinue
    }
  }
  foreach ($entry in $script:DxvkInitialEnvironment.GetEnumerator()) {
    Set-Item -Force -Path "ENV:\$($entry.Key)" -Value $entry.Value
  }
}


function Remove-DxvkDirtyBuildFlagEnvironment {
  # User/global CL/LINK variables can silently inject bad flags into every Meson
  # compiler probe. In the reported log they injected /release into both CL and
  # LINK, producing repeated MSVC D9002 warnings. Clear them for this build
  # process only; vcvarsall-provided INCLUDE/LIB/LIBPATH remain intact.
  foreach ($name in @('CL', '_CL_', 'LINK', '_LINK_', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS')) {
    try {
      if (Test-Path "ENV:\$name") {
        Remove-Item -Path "ENV:\$name" -Force -ErrorAction SilentlyContinue
        Write-Host "[build] Cleared dirty environment variable for this build only: $name" -ForegroundColor DarkYellow
      }
    } catch {
    }
  }
}

function Repair-DxvkPathForNativeTools {
  # Remove broken PATH entries such as:
  #   "C:\...\vcvarsall.bat" x64
  # That is a command, not a directory. It can make Python/Meson CreateProcess
  # failures show up only as "Unhandled python OSError".
  if ([string]::IsNullOrWhiteSpace($env:Path)) { return }
  $kept = New-Object System.Collections.Generic.List[string]
  $seen = @{}
  foreach ($rawEntry in ($env:Path -split ';')) {
    $entry = ([string]$rawEntry).Trim()
    if ([string]::IsNullOrWhiteSpace($entry)) { continue }
    if ($entry -match '"') {
      Write-Host "[build] Removing quoted/non-directory PATH entry for this build only: $entry" -ForegroundColor DarkYellow
      continue
    }
    if ($entry -match '(?i)vcvars(all)?\.bat') {
      Write-Host "[build] Removing vcvars batch command from PATH for this build only: $entry" -ForegroundColor DarkYellow
      continue
    }
    try {
      $full = [IO.Path]::GetFullPath($entry).TrimEnd([char[]]@('\','/'))
    } catch {
      Write-Host "[build] Removing invalid PATH entry for this build only: $entry" -ForegroundColor DarkYellow
      continue
    }
    if (-not (Test-Path -LiteralPath $full -PathType Container)) {
      # Keep normal Windows/VS/Python paths only if they exist. Non-existing PATH
      # entries are not useful for this build and make tool lookup noisy.
      continue
    }
    $key = $full.ToLowerInvariant()
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true
    $kept.Add($full)
  }
  $env:Path = ($kept -join ';')
}

function Set-DxvkPreferredNinjaEnvironment {
  param([Parameter(Mandatory)][string]$VisualStudioPath)

  # Do not let the Python-wheel ninja jobserver build be selected first.  The
  # user log shows Meson picking:
  #   Python314\Scripts\ninja.EXE-1.13.0.git.kitware.jobserver-pipe-1
  # and then stopping immediately after Meson's VS/Ninja wrapper warning.  The
  # Visual Studio bundled Ninja is the safer native tool for MSVC projects.
  $candidateNinjas = New-Object System.Collections.Generic.List[string]
  $candidateNinjas.Add((Join-Path $VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'))
  $candidateNinjas.Add((Join-Path $VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.EXE'))
  $pathNinja = Get-Command 'ninja.exe' -ErrorAction SilentlyContinue
  if ($pathNinja) { $candidateNinjas.Add($pathNinja.Path) }

  foreach ($candidate in $candidateNinjas) {
    if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      $env:NINJA = [IO.Path]::GetFullPath($candidate)
      $ninjaDir = Split-Path -Parent $env:NINJA
      Add-PathEntryForCurrentProcess -PathToAdd $ninjaDir
      Write-Host "[build] Ninja resolved to: $env:NINJA" -ForegroundColor Yellow
      return
    }
  }
  Write-Host '[build] WARNING: Visual Studio bundled ninja.exe was not found; Meson will use PATH ninja.' -ForegroundColor Yellow
}

function Set-DxvkExplicitMsvcToolEnvironment {
  # v8: use vcvarsall's real Native Tools environment and let Meson discover
  # cl/link/lib/rc from PATH.  Do NOT force CC/CXX/LD/AR/RC to full paths here:
  # on this checkout that made Meson keep injecting bogus /release /nologo probe
  # args and it tripped the Ninja backend after the VS wrapper warning.
  Repair-DxvkPathForNativeTools

  foreach ($name in @('CC', 'CXX', 'LD', 'AR', 'RC')) {
    try {
      if (Test-Path "ENV:\$name") {
        Remove-Item -Path "ENV:\$name" -Force -ErrorAction SilentlyContinue
        Write-Host "[build] Cleared forced tool override for this build only: $name" -ForegroundColor DarkYellow
      }
    } catch {
    }
  }

  $cl = Get-Command 'cl.exe' -ErrorAction SilentlyContinue
  $link = Get-Command 'link.exe' -ErrorAction SilentlyContinue
  $lib = Get-Command 'lib.exe' -ErrorAction SilentlyContinue
  $rc = Get-Command 'rc.exe' -ErrorAction SilentlyContinue
  if (-not $cl) {
    throw 'cl.exe was not found after vcvarsall.bat ran. Visual Studio MSVC environment is not usable.'
  }
  if (-not $link) {
    throw 'link.exe was not found after vcvarsall.bat ran. Visual Studio MSVC environment is not usable.'
  }
  Set-DxvkPreferredNinjaEnvironment -VisualStudioPath $vsPath
  Write-Host "[build] MSVC compiler resolved to: $($cl.Path)" -ForegroundColor Yellow
  Write-Host "[build] MSVC linker resolved to:   $($link.Path)" -ForegroundColor Yellow
  if ($lib) { Write-Host "[build] MSVC librarian resolved to: $($lib.Path)" -ForegroundColor DarkYellow }
  if ($rc) { Write-Host "[build] Windows resource compiler resolved to: $($rc.Path)" -ForegroundColor DarkYellow }
  # Force Meson to print the real traceback instead of only
  # "Unhandled python OSError".
  $env:MESON_FORCE_BACKTRACE = '1'
  $env:PYTHONUTF8 = '0'
}

function Set-VisualStudioBuildEnvironment {
  param(
    [Parameter(Mandatory)]
    [ValidateSet('x64', 'x86')]
    [string]$Architecture
  )

  $vcVarsAll = Join-Path $vsPath 'VC\Auxiliary\Build\vcvarsall.bat'
  if (-not (Test-Path $vcVarsAll)) {
    Write-Error "vcvarsall.bat not found under '$(Split-Path $vcVarsAll -Parent)'. Install the MSVC C++ workload in Visual Studio Installer." -ErrorAction Stop
  }

  Restore-DxvkInitialEnvironment
  Remove-DxvkDirtyBuildFlagEnvironment
  Repair-DxvkPathForNativeTools

  $vcVarCandidates = if ($Architecture -eq 'x86') {
    @('x64_x86', 'x86')
  } else {
    @('x64', 'amd64')
  }

  $lastError = $null
  foreach ($vcArg in $vcVarCandidates) {
    Write-Host "[build] Setting Visual Studio compiler environment for $Architecture using vcvarsall.bat $vcArg" -ForegroundColor Yellow
    $cmdLine = 'call "' + $vcVarsAll + '" ' + $vcArg + ' >nul && set'
    $envLines = & $env:ComSpec /d /s /c $cmdLine 2>&1
    $exitCode = if ($null -eq $LASTEXITCODE) { 0 } else { $LASTEXITCODE }
    if ($exitCode -ne 0) {
      $lastError = ($envLines | Out-String).Trim()
      continue
    }

    foreach ($line in $envLines) {
      if ($line -match '=') {
        $v = [string]$line -split '=', 2
        if ($v.Count -eq 2 -and -not [string]::IsNullOrWhiteSpace($v[0])) {
          Set-Item -Force -Path "ENV:\$($v[0])" -Value $v[1]
        }
      }
    }

    Remove-DxvkDirtyBuildFlagEnvironment
    Set-DxvkExplicitMsvcToolEnvironment

    $env:DXVK_BUILD_ARCH = $Architecture
    if ($env:VSCMD_ARG_TGT_ARCH) {
      Write-Host "[build] Visual Studio target architecture: $env:VSCMD_ARG_TGT_ARCH" -ForegroundColor Yellow
    }
    return
  }

  if ([string]::IsNullOrWhiteSpace($lastError)) {
    $lastError = 'vcvarsall.bat did not return a usable environment.'
  }
  Write-Error "Failed to initialize Visual Studio compiler environment for $Architecture. $lastError" -ErrorAction Stop
}

function PerformBuild {
  param(
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64',
    [Parameter(Mandatory)]
    [string]$Backend,
    [Parameter(Mandatory)]
    [ValidateSet('debug', 'debugoptimized', 'release')]
    [string]$BuildFlavour,
    [Parameter(Mandatory)]
    [string]$BuildSubDir,
    [Parameter(Mandatory)]
    [ValidateSet('true', 'false')]
    [string]$EnableTracy,
    [string]$BuildTarget,
    [string[]]$InstallTags,
    [bool]$ConfigureOnly = $false,
    [bool]$ShadersOnly = $false,
    [bool]$FetchDependencies = $true,
    [int]$DepsTimeoutSeconds = 900,
    [int]$MesonSetupTimeoutSeconds = 900,
    [int]$BuildTimeoutSeconds = 0,
    [int]$InstallTimeoutSeconds = 600
  )
  if (-not $InstallTags -or $InstallTags.Count -eq 0) {
    $InstallTags = @('output')
  }
  $SourceDir = [IO.Path]::GetFullPath($DxvkBuildRoot)
  $OutputDir = [IO.Path]::Combine($SourceDir, '_output', $Architecture)
  $BuildDir = [IO.Path]::Combine($SourceDir, $BuildSubDir)
  Set-VisualStudioBuildEnvironment -Architecture $Architecture
  if (-not (Test-Path (Join-Path $SourceDir 'meson.build'))) {
    Write-Error "meson.build was not found at '$SourceDir'. Put build_common.ps1 and build_dxvk_all_ninja.ps1 in the repository root next to meson.build, then run build_dxvk_all_ninja.ps1 from there." -ErrorAction Stop
  }
  Repair-FutureFileTimestamps -SourceDir $SourceDir -BuildDir $BuildDir
  $env:MESON_FORCE_BACKTRACE = '1'
  $env:PYTHONUTF8 = '0'
  $mesonCommand = Resolve-RequiredCommand -Name 'meson' -PythonModule 'mesonbuild.mesonmain' -InstallHint 'Install Meson with: py -m pip install --user meson'
  if ($Backend -eq 'ninja') {
    # Resolve-RequiredCommand may append Python Scripts for meson.exe. Re-force
    # the Visual Studio-bundled Ninja after that so Meson cannot fall back to
    # Python314\Scripts\ninja.EXE again.
    Set-DxvkPreferredNinjaEnvironment -VisualStudioPath $vsPath
    [void](Resolve-RequiredCommand -Name 'ninja' -InstallHint 'Install Ninja with: py -m pip install --user ninja')
  }
  Ensure-DxvkDependencies -SourceDir $SourceDir -FetchDependencies $FetchDependencies -TimeoutSeconds $DepsTimeoutSeconds
  $availableOptions = Get-MesonOptionNames -SourceDir $SourceDir
  Write-Host "[build] Starting $Architecture build for $BuildFlavour..." -ForegroundColor Cyan
  Write-Host "[build] Source directory: $SourceDir" -ForegroundColor DarkGray
  Write-Host "[build] Build directory:  $BuildDir" -ForegroundColor DarkGray
  $buildDirMatchesCurrentSource = Repair-StaleBuildDirectory -SourceDir $SourceDir -BuildDir $BuildDir
  $mesonArgs = @('setup')
  # v8: do not pass --vsenv.  The script already runs vcvarsall.bat and sets a
  # real MSVC PATH/INCLUDE/LIB environment.  Passing --vsenv forces Meson's Ninja
  # backend down the wrapper-warning path shown in the latest log:
  #   "Visual Studio environment is needed to run Ninja..."
  # which is where setup currently exits before build.ninja is written.
  if ($buildDirMatchesCurrentSource) {
    $mesonArgs += '--reconfigure'
  }
  $mesonArgs += @(
    '--buildtype', $BuildFlavour,
    '--backend', $Backend
  )
  foreach ($opt in @(
    @{ Name = 'enable_dxgi';          Value = 'true' },
    @{ Name = 'enable_d3d11';         Value = 'true' },
    @{ Name = 'enable_tests';         Value = 'false' },
    @{ Name = 'enable_tracy';         Value = $EnableTracy },
    @{ Name = 'skip_packman_fetch';   Value = 'true' },
    @{ Name = 'b_vscrt';              Value = 'md' }
  )) {
    $arg = New-MesonOptionArg -AvailableOptions $availableOptions -Name $opt.Name -Value $opt.Value
    if ($arg) { $mesonArgs += $arg }
  }
  if ($ShadersOnly) {
    $arg = New-MesonOptionArg -AvailableOptions $availableOptions -Name 'download_apics' -Value 'false'
    if ($arg) { $mesonArgs += $arg }
  }
  $mesonArgs += $BuildDir
  if (-not $buildDirMatchesCurrentSource) {
    $mesonArgs += $SourceDir
  }
  Write-Host "[build] Running Meson setup..." -ForegroundColor Cyan
  Invoke-CheckedNative -FilePath $mesonCommand.FilePath -ArgumentsPrefix $mesonCommand.ArgumentsPrefix -Arguments $mesonArgs -FailureMessage 'Failed to run Meson setup' -WorkingDirectory $SourceDir -TimeoutSeconds $MesonSetupTimeoutSeconds -DiagnosticLogPath (Join-Path $BuildDir 'meson-logs\meson-log.txt')
  if ($ShadersOnly) {
    Write-Host "[build] Building shaders only..." -ForegroundColor Cyan
    Invoke-CheckedNative -FilePath $mesonCommand.FilePath -ArgumentsPrefix $mesonCommand.ArgumentsPrefix -Arguments @('compile', '-C', $BuildDir, 'rtx_shaders') -FailureMessage 'Failed to build shaders' -WorkingDirectory $SourceDir -TimeoutSeconds $BuildTimeoutSeconds -DiagnosticLogPath (Join-Path $BuildDir 'meson-logs\meson-log.txt')
    return
  }
  if (-not $ConfigureOnly) {
    Write-Host "[build] Compiling..." -ForegroundColor Cyan
    $compileArgs = @('compile', '-C', $BuildDir)
    if (-not [string]::IsNullOrWhiteSpace($BuildTarget)) {
      $compileArgs += $BuildTarget
    }
    $compileArgs += '-v'
    Invoke-CheckedNative -FilePath $mesonCommand.FilePath -ArgumentsPrefix $mesonCommand.ArgumentsPrefix -Arguments $compileArgs -FailureMessage 'Failed to run build step' -WorkingDirectory $SourceDir -TimeoutSeconds $BuildTimeoutSeconds -DiagnosticLogPath (Join-Path $BuildDir 'meson-logs\meson-log.txt')
    $tagList = $InstallTags -join ','
    Write-Host "[build] Installing tag(s) '$tagList' to $OutputDir..." -ForegroundColor Cyan
    Invoke-CheckedNative -FilePath $mesonCommand.FilePath -ArgumentsPrefix $mesonCommand.ArgumentsPrefix -Arguments @('install', '-C', $BuildDir, '--tags', $tagList) -FailureMessage 'Failed to run install step' -WorkingDirectory $SourceDir -TimeoutSeconds $InstallTimeoutSeconds -DiagnosticLogPath (Join-Path $BuildDir 'meson-logs\meson-log.txt')
    # Deploy the NGX runtime DLLs next to every installed/built d3d11.dll.
    $ngxSource = Join-Path $SourceDir 'nv-private\hdremix\bin\release'
    if (Test-Path $ngxSource) {
      $ngxDlls = Get-ChildItem -Path $ngxSource -Filter 'nvngx_*.dll' -File
      $ngxTargets = @()
      foreach ($searchRoot in @($OutputDir, $BuildDir)) {
        if ($searchRoot -and (Test-Path $searchRoot)) {
          $ngxTargets += Get-ChildItem -Path $searchRoot -Recurse -Filter 'd3d11.dll' -File -ErrorAction SilentlyContinue
        }
      }
      foreach ($t in $ngxTargets) {
        foreach ($dll in $ngxDlls) {
          Copy-Item -Path $dll.FullName -Destination $t.DirectoryName -Force
        }
      }
      if ($ngxTargets.Count -gt 0) {
        Write-Host ("[build] Deployed {0} NGX DLLs next to {1} d3d11.dll location(s)" -f $ngxDlls.Count, $ngxTargets.Count) -ForegroundColor Cyan
      }
    } else {
      Write-Host "[build] WARNING: NGX runtime DLLs not found at $ngxSource - DLSS will be unavailable in deployed builds." -ForegroundColor Yellow
    }
    Write-Host "[build] Build completed successfully for $Architecture $BuildFlavour" -ForegroundColor Green
  } else {
    Write-Host "[build] Configuration completed for $Architecture $BuildFlavour (no build performed)" -ForegroundColor Green
  }
}
