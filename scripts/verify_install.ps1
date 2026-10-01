#requires -Version 5.1
<#
.SYNOPSIS
    Proves that an installed Recovery Coordinator package can be consumed by a
    downstream CMake project.

.DESCRIPTION
    The script performs the whole packaging proof, in order:

      1. installs an already-built tree into a temporary prefix;
      2. checks that the prefix really carries the package configuration, the
         exported target file, the installed public headers and the library;
      3. configures tests/downstream against that prefix with
         find_package(RecoveryCoordinator 1.0 REQUIRED);
      4. proves that the consumer's own compile rules resolve into the install
         prefix, so the build cannot have silently used the source tree;
      5. builds and runs the consumer, and requires its machine readable result
         line;
      6. removes the temporary prefix and the consumer build directory on every
         exit path.

    Every stage prints the exact command it runs. The first failing stage stops
    the script and the script exits non-zero.

.PARAMETER BuildDirectory
    The configured build tree to install from. Defaults to <repo>/build/release.

.PARAMETER Prefix
    The install prefix. Defaults to a fresh directory under the system
    temporary directory, which the script removes afterwards.

.PARAMETER KeepTemporaryPrefix
    Leaves the temporary prefix and the consumer build directory in place, and
    prints their paths instead of deleting them.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/verify_install.ps1
#>
[CmdletBinding()]
param(
    [string] $BuildDirectory = '',
    [string] $Prefix = '',
    [switch] $KeepTemporaryPrefix
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-Stage {
    param([Parameter(Mandatory)][string] $Text)
    Write-Host ''
    Write-Host ('=== ' + $Text)
}

function Invoke-Step {
    param(
        [Parameter(Mandatory)][string] $What,
        [Parameter(Mandatory)][string] $Command,
        [Parameter(Mandatory)][string[]] $Arguments
    )
    Write-Host ('  ' + $Command + ' ' + ($Arguments -join ' '))
    & $Command @Arguments
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw ($What + ' failed with exit code ' + $code)
    }
}

# The repository's build instructions run the Visual Studio environment script
# and then a command through cmd.exe. This script does the same once, for the
# environment dump only, and then imports the result: every later command is a
# plain PowerShell invocation, so no path is ever re-quoted by a second command
# interpreter and a path containing spaces stays intact.
function Import-BuildEnvironment {
    param([Parameter(Mandatory)][string] $VcVarsPath)
    $lines = & cmd.exe /c ('"' + $VcVarsPath + '" >nul 2>&1 && set')
    if ($LASTEXITCODE -ne 0) {
        throw ('the Visual Studio environment script failed: ' + $VcVarsPath)
    }
    $imported = 0
    foreach ($line in $lines) {
        $index = $line.IndexOf('=')
        if ($index -le 0) {
            continue
        }
        $name = $line.Substring(0, $index)
        $value = $line.Substring($index + 1)
        Set-Item -Path ('env:' + $name) -Value $value
        $imported = $imported + 1
    }
    if ($imported -eq 0) {
        throw 'the Visual Studio environment script produced no environment'
    }
    Write-Host ('  imported ' + $imported + ' environment variables from ' + $VcVarsPath)
}

function Find-VcVars {
    $candidates = New-Object System.Collections.Generic.List[string]
    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    if ($programFilesX86) {
        $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $roots = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($LASTEXITCODE -eq 0 -and $roots) {
                foreach ($root in $roots) {
                    $candidates.Add((Join-Path $root 'VC\Auxiliary\Build\vcvars64.bat'))
                }
            }
        }
        $candidates.Add((Join-Path $programFilesX86 'Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'))
        $candidates.Add((Join-Path $programFilesX86 'Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'))
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    throw 'vcvars64.bat was not found; install the MSVC 2022 C++ build tools'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $repoRoot 'build\release'
}
$downstreamSource = Join-Path $repoRoot 'tests\downstream'
if (-not (Test-Path -LiteralPath $downstreamSource)) {
    throw ('the downstream consumer project is missing: ' + $downstreamSource)
}
if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt'))) {
    throw ('the build tree is not configured: ' + $BuildDirectory + ' (build it first, see AGENT-STATUS.md)')
}

$temporary = New-Object System.Collections.Generic.List[string]
if (-not $Prefix) {
    $Prefix = Join-Path ([System.IO.Path]::GetTempPath()) ('rc-install-' + [guid]::NewGuid().ToString('N'))
    $temporary.Add($Prefix)
}
$consumerBuild = Join-Path ([System.IO.Path]::GetTempPath()) ('rc-downstream-' + [guid]::NewGuid().ToString('N'))
$temporary.Add($consumerBuild)

Write-Host 'verify_install: the packaging proof'
Write-Host ('  repository     : ' + $repoRoot)
Write-Host ('  build tree     : ' + $BuildDirectory)
Write-Host ('  install prefix : ' + $Prefix)

try {
    Write-Stage 'Visual Studio build environment'
    Import-BuildEnvironment -VcVarsPath (Find-VcVars)

    Write-Stage '1/5 install'
    Invoke-Step -What 'cmake --install' -Command 'cmake' -Arguments @('--install', $BuildDirectory, '--prefix', $Prefix)

    Write-Stage '2/5 inspect the installed package'
    $configDir = Join-Path $Prefix 'lib\cmake\RecoveryCoordinator'
    $required = @(
        (Join-Path $configDir 'RecoveryCoordinatorConfig.cmake'),
        (Join-Path $configDir 'RecoveryCoordinatorConfigVersion.cmake'),
        (Join-Path $configDir 'RecoveryCoordinatorTargets.cmake'),
        (Join-Path $Prefix 'include\recovery\engine.hpp'),
        (Join-Path $Prefix 'include\recovery\adapters\synthetic.hpp')
    )
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw ('the installed package is incomplete, missing: ' + $path)
        }
        Write-Host ('  present ' + $path)
    }
    $libraries = @(Get-ChildItem -LiteralPath (Join-Path $Prefix 'lib') -File -ErrorAction SilentlyContinue)
    if ($libraries.Count -eq 0) {
        throw ('the installed package carries no library under ' + (Join-Path $Prefix 'lib'))
    }
    foreach ($library in $libraries) {
        Write-Host ('  present ' + $library.FullName + ' (' + $library.Length + ' bytes)')
    }

    Write-Stage '3/5 configure the downstream consumer against the prefix'
    Invoke-Step -What 'cmake configure' -Command 'cmake' -Arguments @(
        '-S', $downstreamSource,
        '-B', $consumerBuild,
        '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        ('-DCMAKE_PREFIX_PATH=' + $Prefix)
    )

    Write-Stage '4/5 prove the consumer resolves into the install prefix'
    $ninjaFile = Join-Path $consumerBuild 'build.ninja'
    if (-not (Test-Path -LiteralPath $ninjaFile)) {
        throw ('the consumer build tree has no Ninja file: ' + $ninjaFile)
    }
    $resolved = @(Select-String -LiteralPath $ninjaFile -SimpleMatch -Pattern $Prefix)
    if ($resolved.Count -eq 0) {
        throw ('the consumer build rules never mention the install prefix ' + $Prefix + ': the package was not found where it was installed')
    }
    Write-Host ('  the consumer compile rules reference the install prefix ' + $resolved.Count + ' times, for example:')
    Write-Host ('    ' + $resolved[0].Line.Trim())
    $importedTarget = @(Select-String -LiteralPath $ninjaFile -SimpleMatch -Pattern 'recovery_coordinator.lib')
    if ($importedTarget.Count -eq 0) {
        throw 'the consumer never links recovery_coordinator.lib through the exported target'
    }
    Write-Host ('  the consumer links the installed library through RecoveryCoordinator::recovery_coordinator')

    Write-Stage '5/5 build and run the downstream consumer'
    Invoke-Step -What 'cmake --build' -Command 'cmake' -Arguments @('--build', $consumerBuild)

    $consumer = Join-Path $consumerBuild 'downstream_consumer.exe'
    if (-not (Test-Path -LiteralPath $consumer)) {
        $consumer = Join-Path $consumerBuild 'Release\downstream_consumer.exe'
    }
    if (-not (Test-Path -LiteralPath $consumer)) {
        throw ('the consumer executable was not produced in ' + $consumerBuild)
    }
    Write-Host ('  ' + $consumer)
    $consumerOutput = & $consumer
    $consumerCode = $LASTEXITCODE
    foreach ($line in $consumerOutput) {
        Write-Host ('  ' + $line)
    }
    if ($consumerCode -ne 0) {
        throw ('the downstream consumer failed with exit code ' + $consumerCode)
    }
    $resultLine = @($consumerOutput | Where-Object { $_ -like 'result *' } | Select-Object -Last 1)
    if ($resultLine.Count -eq 0) {
        throw 'the downstream consumer printed no machine readable result line'
    }
    Write-Host ''
    Write-Host ('verify_install: ok prefix=' + $Prefix)
    Write-Host ('verify_install: ' + $resultLine[0])
}
finally {
    if ($KeepTemporaryPrefix) {
        Write-Host ''
        Write-Host 'verify_install: keeping the temporary directories'
        foreach ($path in $temporary) {
            Write-Host ('  ' + $path)
        }
    }
    else {
        foreach ($path in $temporary) {
            if (Test-Path -LiteralPath $path) {
                Remove-Item -LiteralPath $path -Recurse -Force
                Write-Host ('verify_install: removed ' + $path)
            }
        }
    }
}
