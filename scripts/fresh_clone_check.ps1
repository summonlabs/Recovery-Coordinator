<#
.SYNOPSIS
    Proves that the published repository is self sufficient.

.DESCRIPTION
    Clones the remote repository into a temporary directory outside the working
    tree, configures and builds it from scratch, runs its test suite and its
    benchmark entry point, verifies that its packaging metadata installs, and
    then removes the clone. Nothing from the working tree is reused, so a file
    that was never committed cannot make this pass.

    Every step is fatal: the script stops at the first failure and reports the
    exact command that failed.

.PARAMETER Remote
    The git remote to clone. Defaults to origin of the current repository.

.PARAMETER Ref
    The ref to check out. Defaults to the current branch name.

.PARAMETER Keep
    Leaves the temporary clone in place for inspection instead of removing it.

.EXAMPLE
    pwsh -File scripts/fresh_clone_check.ps1
    pwsh -File scripts/fresh_clone_check.ps1 -Ref v1.0.0
#>
[CmdletBinding()]
param(
    [string] $Remote,
    [string] $Ref,
    [switch] $Keep
)

$ErrorActionPreference = 'Stop'

function Invoke-Step {
    param([string] $What, [scriptblock] $Body)
    Write-Host "== $What"
    & $Body
    if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) {
        throw "$What failed with exit code $LASTEXITCODE"
    }
}

function Invoke-WindowsBuild {
    param([string] $Source, [string] $Build)
    # vcvars64.bat only exists on a Windows host with the MSVC build tools. On
    # any other host the plain configure below is used, so this script is not
    # Windows specific.
    $vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path -LiteralPath $vcvars) {
        $command = '"' + $vcvars + '" >nul 2>&1 && cmake -S "' + $Source + '" -B "' + $Build +
                   '" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "' + $Build + '"'
        & cmd /c $command
    } else {
        & cmake -S $Source -B $Build -DCMAKE_BUILD_TYPE=Release
        if ($LASTEXITCODE -ne 0) { return }
        & cmake --build $Build
    }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Remote) {
    $Remote = (& git -C $repoRoot remote get-url origin).Trim()
}
if (-not $Ref) {
    $Ref = (& git -C $repoRoot rev-parse --abbrev-ref HEAD).Trim()
}

Write-Host "remote : $Remote"
Write-Host "ref    : $Ref"

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("rc-fresh-clone-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$clone = Join-Path $scratch 'clone'
$build = Join-Path $scratch 'build'

try {
    Invoke-Step 'clone' { & git clone --quiet --branch $Ref $Remote $clone }
    Invoke-Step 'clone is clean' {
        $dirty = & git -C $clone status --porcelain
        if ($dirty) { throw "the clone is not clean:\n$dirty" }
    }
    Invoke-Step 'configure and build' { Invoke-WindowsBuild -Source $clone -Build $build }
    Invoke-Step 'test suite' { & (Join-Path $build 'tests/recovery_tests.exe') }
    Invoke-Step 'benchmarks' { & (Join-Path $build 'benchmarks/recovery_benchmarks.exe') --directory (Join-Path $scratch 'bench') }
    Invoke-Step 'install' { & cmake --install $build --prefix (Join-Path $scratch 'install') }
    Write-Host "== fresh clone closure verified"
} finally {
    if ($Keep) {
        Write-Host "kept: $scratch"
    } else {
        Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
    }
}
