[CmdletBinding()]
param(
    [ValidateSet('debug', 'debugoptimized', 'release')]
    [string]$BuildFlavour = 'debugoptimized',

    [string]$BuildSubDir = 'build',

    [ValidateSet('ninja')]
    [string]$Backend = 'ninja',

    [ValidateSet('true', 'false')]
    [string]$EnableTracy = 'false',

    [string[]]$InstallTags = @('output'),

    [switch]$ConfigureOnly,

    [switch]$ShadersOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Path $PSScriptRoot -Parent

function Write-Step {
    param([string]$Message)
    Write-Host "[build] $Message" -ForegroundColor Cyan
}

function Add-PathEntry {
    param([string]$PathEntry)

    if ([string]::IsNullOrWhiteSpace($PathEntry) -or -not (Test-Path $PathEntry)) {
        return
    }

    $currentPath = @($env:PATH -split ';' | Where-Object { $_ })
    if ($currentPath -notcontains $PathEntry) {
        $env:PATH = "$PathEntry;$env:PATH"
    }
}

function Test-CommandExists {
    param([string]$Name)
    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Add-LocalPythonPackages {
    $localPythonPackages = Join-Path $repoRoot 'build_deps\python'
    if (-not (Test-Path $localPythonPackages)) {
        return
    }

    if ($env:PYTHONPATH) {
        $pythonPathEntries = @($env:PYTHONPATH -split ';' | Where-Object { $_ })
        if ($pythonPathEntries -notcontains $localPythonPackages) {
            $env:PYTHONPATH = "$localPythonPackages;$env:PYTHONPATH"
        }
    } else {
        $env:PYTHONPATH = $localPythonPackages
    }

    Add-PathEntry (Join-Path $localPythonPackages 'Scripts')
    Add-PathEntry (Join-Path $localPythonPackages 'bin')
}

function Get-CommandPath {
    param([string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Path
    }

    return $null
}

function Resolve-Python {
    $candidates = @(
        @{ Name = 'py'; Args = @('-3') },
        @{ Name = 'python'; Args = @() }
    )

    foreach ($candidate in $candidates) {
        $command = Get-Command $candidate.Name -ErrorAction SilentlyContinue
        if (-not $command) {
            continue
        }

        # Use a properly quoted -c argument so PowerShell does not break the string
        $pyCmd = "import struct,sys; print(str(sys.version_info.major) + '.' + str(sys.version_info.minor)); print(str(struct.calcsize('P') * 8)); print(sys.executable)"
        $pythonInfo = & $command.Path @($candidate.Args + @('-c', $pyCmd)) 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $pythonInfo -or $pythonInfo.Count -lt 3) {
            continue
        }

        try {
            $version = [Version](($pythonInfo | Select-Object -First 1).Trim())
        } catch {
            continue
        }

        if ($version -lt [Version]'3.9') {
            continue
        }

        $bitness = 0
        if (-not [int]::TryParse((($pythonInfo | Select-Object -Skip 1 -First 1).Trim()), [ref]$bitness)) {
            continue
        }

        if ($bitness -ne 64) {
            continue
        }

        $exePath = ($pythonInfo | Select-Object -Skip 2 -First 1).Trim()
        if ([string]::IsNullOrWhiteSpace($exePath)) {
            continue
        }

        $scriptsDir = Join-Path (Split-Path $exePath -Parent) 'Scripts'

        return [PSCustomObject]@{
            Launcher = $command.Path
            Args = $candidate.Args
            Executable = $exePath
            ScriptsDir = $scriptsDir
            Version = $version.ToString(2)
            Bitness = $bitness
        }
    }

    throw 'A 64-bit Python 3.9 or newer installation was not found. Install a standard x64 python.org build and make sure it is available on PATH.'
}

function Ensure-Meson {
    param([pscustomobject]$Python)

    Add-PathEntry $Python.ScriptsDir

    if (Test-CommandExists 'meson') {
        $mesonCommand = Get-CommandPath 'meson'
        try {
            & $mesonCommand --version *> $null
            if ($LASTEXITCODE -eq 0) {
                return $mesonCommand
            }
        } catch {
        }
    }

    $mesonExe = Join-Path $Python.ScriptsDir 'meson.exe'
    if (Test-Path $mesonExe) {
        try {
            & $mesonExe --version *> $null
            if ($LASTEXITCODE -eq 0) {
                Add-PathEntry $Python.ScriptsDir
                return $mesonExe
            }
        } catch {
        }
    }

    $moduleMesonWorks = $false
    try {
        & $Python.Executable -m mesonbuild.mesonmain --help *> $null
        $moduleMesonWorks = $LASTEXITCODE -eq 0
    } catch {
        $moduleMesonWorks = $false
    }

    if ($moduleMesonWorks) {
        $shimDir = Join-Path $env:TEMP 'dxvk-remix-build-shims'
        New-Item -ItemType Directory -Path $shimDir -Force | Out-Null

        $shimPath = Join-Path $shimDir 'meson.cmd'
        @(
            '@echo off',
            ('"{0}" -m mesonbuild.mesonmain %*' -f $Python.Executable)
        ) | Set-Content -Path $shimPath -Encoding Ascii

        Add-PathEntry $shimDir
        return $shimPath
    }

    throw 'Meson was not found. Install Meson or `pip install meson` for the detected Python installation.'
}

function Ensure-Ninja {
    param([string]$VisualStudioPath, [pscustomobject]$Python)

    if (Test-CommandExists 'ninja') {
        return Get-CommandPath 'ninja'
    }

    $pythonNinja = Join-Path $Python.ScriptsDir 'ninja.exe'
    if (Test-Path $pythonNinja) {
        Add-PathEntry $Python.ScriptsDir
        return $pythonNinja
    }

    $vsNinjaDir = Join-Path $VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
    $vsNinjaExe = Join-Path $vsNinjaDir 'ninja.exe'
    if (Test-Path $vsNinjaExe) {
        Add-PathEntry $vsNinjaDir
        return $vsNinjaExe
    }

    throw 'Ninja was not found. Install Ninja or add the Visual Studio CMake/Ninja tools to your installation.'
}

function Resolve-VsWhere {
    $vsWhere = Get-Command 'vswhere.exe' -ErrorAction SilentlyContinue
    if ($vsWhere) {
        return $vsWhere.Path
    }

    $fallback = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $fallback) {
        return $fallback
    }

    throw 'vswhere.exe was not found. Install Visual Studio 2019 or 2022 with the C++ build tools.'
}

function Resolve-VisualStudio {
    param([string]$VsWherePath)

    $vsPath = & $VsWherePath -latest -version '[16.0,18.0)' -products * -requires Microsoft.Component.MSBuild -property installationPath
    if ([string]::IsNullOrWhiteSpace($vsPath)) {
        throw 'A supported Visual Studio installation was not found. Install Visual Studio 2019 or 2022 with MSBuild and the desktop C++ workload.'
    }

    return ($vsPath | Select-Object -First 1).Trim()
}

function Import-VcVars {
    param([string]$VisualStudioPath)

    if ((Test-Path env:LIBPATH) -and (Test-CommandExists 'cl')) {
        return
    }

    $vcVarsPath = Join-Path $VisualStudioPath 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path $vcVarsPath)) {
        throw "vcvarsall.bat was not found under $VisualStudioPath"
    }

    $envBlock = cmd /c "`"$vcVarsPath`" x64 >nul && set"
    $seenEnvNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($line in $envBlock) {
        if ($line -match '=' -and -not $line.Contains('===')) {
            $parts = $line -split '=', 2
            if (-not $seenEnvNames.Add($parts[0])) {
                continue
            }

            $envName = if ($parts[0] -ieq 'PATH') { 'Path' } else { $parts[0] }
            Set-Item -Path "ENV:$envName" -Value $parts[1]
        }
    }

    if (-not (Test-CommandExists 'cl')) {
        throw 'Failed to load the Visual Studio compiler environment.'
    }

    if (-not $env:WindowsSdkDir) {
        throw 'The Windows SDK was not detected in the Visual Studio toolchain environment.'
    }
}

function Resolve-VulkanSdk {
    $candidateRoots = @()

    if ($env:VULKAN_SDK) {
        $candidateRoots += $env:VULKAN_SDK
    }

    $defaultRoot = 'C:\VulkanSDK'
    if (Test-Path $defaultRoot) {
        $candidateRoots += Get-ChildItem $defaultRoot -Directory | Sort-Object Name -Descending | Select-Object -ExpandProperty FullName
    }

    foreach ($candidate in $candidateRoots | Select-Object -Unique) {
        $includePath = Join-Path $candidate 'Include\vulkan\vulkan.h'
        $libPath = Join-Path $candidate 'Lib\vulkan-1.lib'
        $binPath = Join-Path $candidate 'Bin'

        if ((Test-Path $includePath) -and (Test-Path $libPath) -and (Test-Path $binPath)) {
            $env:VULKAN_SDK = $candidate
            Add-PathEntry $binPath
            return $candidate
        }
    }

    throw 'A Vulkan SDK installation was not found. Install LunarG Vulkan SDK 1.4.313.2 or newer, or set VULKAN_SDK to a valid install.'
}

function Ensure-Submodules {
    Set-Location $repoRoot

    if (-not (Test-Path '.gitmodules')) {
        return
    }

    $requiredMarkers = @(
        @{ Name = 'Vulkan-Headers'; Path = 'include\vulkan\include\vulkan\vulkan.h' },
        @{ Name = 'RTXDI'; Path = 'submodules\rtxdi\rtxdi-sdk\include' },
        @{ Name = 'RTXCR'; Path = 'submodules\rtxcr\shaders\include' },
        @{ Name = 'NRC'; Path = 'submodules\nrc\Include\NrcVk.h' },
        @{ Name = 'NVAPI'; Path = 'submodules\nvapi\amd64\nvapi64.lib' },
        @{ Name = 'Detours'; Path = 'submodules\Detours\src\detours.h' },
        @{ Name = 'XeSS headers'; Path = 'submodules\xess\inc\xess\xess_vk.h' },
        @{ Name = 'XeSS runtime'; Path = 'submodules\xess\bin\libxess.dll' },
        @{ Name = 'DLSS headers'; Path = 'external\ngx_sdk_310_6_0\include\nvsdk_ngx.h' },
        @{ Name = 'DLSS import libraries'; Path = 'external\ngx_sdk_310_6_0\lib\Windows_x86_64\x64\nvsdk_ngx_d.lib' },
        @{ Name = 'FidelityFX SDK'; Path = 'submodules\fidelityfx-sdk\FidelityFX-SDK-2.2.0\Kits\FidelityFX\api\include\ffx_api_loader.h' }
    )

    $missingMarkers = @()
    foreach ($marker in $requiredMarkers) {
        $markerPath = Join-Path $repoRoot $marker.Path
        if (-not (Test-Path $markerPath)) {
            $missingMarkers += ('{0} ({1})' -f $marker.Name, $marker.Path)
        }
    }
    $hasRequiredSubmoduleContent = $missingMarkers.Count -eq 0

    if (-not (Test-Path '.git')) {
        if ($hasRequiredSubmoduleContent) {
            Write-Step 'No .git directory found; using bundled submodule contents.'
            return
        }

        throw ('This source folder is not a git checkout and required submodule content is missing: {0}' -f ($missingMarkers -join ', '))
    }

    if (-not (Test-CommandExists 'git')) {
        if ($hasRequiredSubmoduleContent) {
            Write-Step 'Git was not found; using bundled submodule contents.'
            return
        }

        throw ('Git is required to initialize missing submodules: {0}' -f ($missingMarkers -join ', '))
    }

    $statusLines = & git submodule status --recursive 2>$null
    if ($LASTEXITCODE -ne 0) {
        if ($hasRequiredSubmoduleContent) {
            Write-Step 'Git submodule status failed; using bundled submodule contents.'
            return
        }

        throw 'Failed to query git submodule status. Make sure this repository is a valid git checkout.'
    }

    $needsUpdate = @($statusLines | Where-Object { $_ -match '^[\-+]' }).Count -gt 0
    if ($needsUpdate) {
        Write-Step 'Missing submodules detected. Running git submodule update --init --recursive.'
        & git submodule update --init --recursive
        if ($LASTEXITCODE -ne 0) {
            if ($hasRequiredSubmoduleContent) {
                Write-Step 'Git submodule update failed, but bundled submodule contents are usable.'
                return
            }

            throw 'Failed to initialize required submodules.'
        }
    }
}

Push-Location $repoRoot
try {
    Write-Step 'Resolving build requirements'

    Ensure-Submodules
    Add-LocalPythonPackages
    $python = Resolve-Python
    $vsWherePath = Resolve-VsWhere
    $visualStudioPath = Resolve-VisualStudio -VsWherePath $vsWherePath
    Import-VcVars -VisualStudioPath $visualStudioPath
    $vulkanSdk = Resolve-VulkanSdk
    $mesonPath = Ensure-Meson -Python $python
    $ninjaPath = Ensure-Ninja -VisualStudioPath $visualStudioPath -Python $python

    Write-Host ('Python:        {0} ({1}, {2}-bit)' -f $python.Executable, $python.Version, $python.Bitness) -ForegroundColor Yellow
    Write-Host ('Visual Studio: {0}' -f $visualStudioPath) -ForegroundColor Yellow
    Write-Host ('Windows SDK:   {0}' -f $env:WindowsSdkDir) -ForegroundColor Yellow
    Write-Host ('Vulkan SDK:    {0}' -f $vulkanSdk) -ForegroundColor Yellow
    Write-Host ('Meson:         {0}' -f $mesonPath) -ForegroundColor Yellow
    Write-Host ('Ninja:         {0}' -f $ninjaPath) -ForegroundColor Yellow

    . (Join-Path $repoRoot 'build_common.ps1')

    Write-Step ('Starting build: type={0}, dir={1}, backend={2}, tracy={3}' -f $BuildFlavour, $BuildSubDir, $Backend, $EnableTracy)
    PerformBuild -BuildFlavour $BuildFlavour -BuildSubDir $BuildSubDir -Backend $Backend -EnableTracy $EnableTracy -InstallTags $InstallTags -ConfigureOnly:$ConfigureOnly.IsPresent -ShadersOnly:$ShadersOnly.IsPresent
}
finally {
    Pop-Location
}
