<#
.SYNOPSIS
Install the WSL build and RISC-V guest toolchain used by DoomV.

WSL itself is intentionally separate: install Ubuntu with `wsl --install -d
Ubuntu`, reboot if Windows asks, then run this script. The native host tools
are installed by install_dependencies.ps1.
#>
param([string]$Distro = 'Ubuntu', [switch]$SkipVerification)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
& wsl.exe -d $Distro -u root -- true
if ($LASTEXITCODE -ne 0) {
    throw "WSL distribution '$Distro' is unavailable. Install it first with: wsl --install -d $Distro"
}
$WslRepo = '/mnt/' + $Repo.Substring(0, 1).ToLowerInvariant() + $Repo.Substring(2).Replace('\', '/')
$wslArgs = @('-d', $Distro, '-u', 'root', '--', 'bash', "$WslRepo/scripts/install_wsl.sh", $WslRepo)
if ($SkipVerification) { $wslArgs += '--skip-verification' }
& wsl.exe @wslArgs
if ($LASTEXITCODE -ne 0) { throw "WSL toolchain installation failed ($LASTEXITCODE)." }
Write-Host 'WSL and RISC-V toolchain are ready.'
