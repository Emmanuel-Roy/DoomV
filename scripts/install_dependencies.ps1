<#
.SYNOPSIS
Install the Windows host tools and WSL guest/verification dependencies.
.EXAMPLE
powershell -ExecutionPolicy Bypass -File scripts/install_dependencies.ps1
.NOTES
This installs only native Windows host dependencies. Run scripts/toolchain.ps1
separately for WSL and the RISC-V guest toolchain.
#>
param(
    [string]$Distro = 'Ubuntu',
    [string]$MsysRoot = "$env:LOCALAPPDATA\DoomV\msys64",
[switch]$SkipVerification
)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
function Install-Package([string]$Id) {
    Invoke-Checked winget @('install', '--exact', '--id', $Id, '--source', 'winget',
        '--accept-package-agreements', '--accept-source-agreements', '--disable-interactivity')
}
if (Test-Path -LiteralPath (Join-Path $Repo '.scripts.lock')) { throw 'Another script is running in this checkout.' }
if (Test-Path -LiteralPath (Join-Path $Repo '.signature.lock')) { throw 'Verification is running in this checkout.' }

Write-Host '=== Native Windows dependencies ==='
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Install-Package 'Git.Git' }
if (-not (Get-Command python -ErrorAction SilentlyContinue)) { Install-Package 'Python.Python.3.12' }
if (-not (Test-Path -LiteralPath "$MsysRoot\usr\bin\bash.exe")) {
    Invoke-Checked winget @('install', '--exact', '--id', 'MSYS2.MSYS2', '--source', 'winget',
        '--location', $MsysRoot, '--accept-package-agreements', '--accept-source-agreements', '--disable-interactivity')
}
if (-not (Test-Path -LiteralPath "$MsysRoot\usr\bin\bash.exe")) {
    throw 'MSYS2 was installed elsewhere. Rerun with -MsysRoot <its install directory>.'
}
$MsysBash = "$MsysRoot\usr\bin\bash.exe"
$env:MSYSTEM = 'UCRT64'
# Updating MSYS2's core can require a second invocation in a fresh process.
Invoke-Checked $MsysBash @('-lc', 'pacman --noconfirm -Syu')
Invoke-Checked $MsysBash @('-lc', 'pacman --noconfirm -Syu --needed make git diffutils curl tar unzip mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-python')
$env:PATH = "$MsysRoot\ucrt64\bin;$MsysRoot\usr\bin;$env:PATH"
New-Item -ItemType Directory -Path (Join-Path $Repo 'build') -Force | Out-Null
@{ bash = $MsysBash; path = @("$MsysRoot\ucrt64\bin", "$MsysRoot\usr\bin") } |
    ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $Repo 'build/tools.json')

Write-Host '=== Pinned Windows-compatible submodules ==='
# Linux/BusyBox and the reference sources are checked out on WSL ext4 instead.
Invoke-Checked git @('-C', $Repo, 'submodule', 'update', '--init', '--',
    'tools/verification/simulators/spike/src', 'tools/doom/doombuild/doomgeneric')

Write-Host 'Native dependencies installed. Run scripts/toolchain.ps1 for WSL and RISC-V.'
