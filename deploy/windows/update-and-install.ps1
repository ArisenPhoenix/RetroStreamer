# Pull latest from GitHub, build the Windows client, and install to Program Files.
#
# From the repo root (usually Documents\RetroStreamer), Admin PowerShell recommended:
#   .\deploy\windows\update-and-install.ps1
#   .\deploy\windows\update-and-install.ps1 -ResetHard
#
# Options:
#   -ResetHard     discard local edits, match origin/master (typical for a client PC)
#   -SkipPull      build/install only (no git)
#   -SkipInstall   build only
#   -BuildHost     host-capable GUI (needs ViGEm etc.)
#   -Reconfigure   force cmake reconfigure
#   -Clean         wipe build/ first
#   -Launch        start the installed GUI when done
#   -Prefix        install root (default: C:\Program Files\ArchStreamer)
#   -VcpkgRoot     default C:\dev\vcpkg or $env:VCPKG_ROOT
#   -Branch        default master
#
# Day-to-day after Linux pushes to GitHub:
#   .\deploy\windows\update-and-install.ps1 -ResetHard

param(
    [switch]$ResetHard,
    [switch]$SkipPull,
    [switch]$SkipInstall,
    [switch]$BuildHost,
    [switch]$Reconfigure,
    [switch]$Clean,
    [switch]$Launch,
    [string]$Prefix = "C:\Program Files\ArchStreamer",
    [string]$VcpkgRoot = "",
    [string]$Config = "Release",
    [string]$Branch = "master"
)

$ErrorActionPreference = "Stop"

# This file lives at <repo>\deploy\windows\update-and-install.ps1
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not (Test-Path (Join-Path $RepoRoot "build_windows.ps1"))) {
    throw "Could not find build_windows.ps1 at repo root: $RepoRoot"
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    if ($env:VCPKG_ROOT) {
        $VcpkgRoot = $env:VCPKG_ROOT
    } else {
        $VcpkgRoot = "C:\dev\vcpkg"
    }
}

Set-Location $RepoRoot
Write-Host "=== ArchStreamer Windows update ==="
Write-Host "Repo: $RepoRoot"

if (-not $SkipPull) {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw "git not found on PATH"
    }
    Write-Host "Fetching origin..."
    git fetch origin
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed" }

    $status = @(git status --porcelain)
    if ($ResetHard) {
        Write-Host "Resetting to origin/$Branch (discarding local changes)..."
        git checkout $Branch
        if ($LASTEXITCODE -ne 0) { throw "git checkout $Branch failed" }
        git reset --hard "origin/$Branch"
        if ($LASTEXITCODE -ne 0) { throw "git reset --hard failed" }
        git clean -fd
    } elseif ($status.Count -gt 0) {
        Write-Host "Working tree has local changes:"
        git status -sb
        throw "Refusing to pull over dirty tree. Re-run with -ResetHard, or commit/stash locally, or pass -SkipPull."
    } else {
        Write-Host "Pulling origin/$Branch..."
        git checkout $Branch
        if ($LASTEXITCODE -ne 0) { throw "git checkout $Branch failed" }
        git pull --ff-only origin $Branch
        if ($LASTEXITCODE -ne 0) { throw "git pull failed (try -ResetHard if histories diverged)" }
    }
    Write-Host "Git: $(git rev-parse --short HEAD) $(git log -1 --pretty=%s)"
} else {
    Write-Host "Skipping git pull."
}

Write-Host "Building..."
$buildArgs = @{
    Config = $Config
    VcpkgRoot = $VcpkgRoot
}
if ($BuildHost) { $buildArgs["BuildHost"] = $true }
if ($Reconfigure) { $buildArgs["Reconfigure"] = $true }
if ($Clean) { $buildArgs["Clean"] = $true }
& (Join-Path $RepoRoot "build_windows.ps1") @buildArgs
if ($LASTEXITCODE -ne 0) { throw "build_windows.ps1 failed with exit $LASTEXITCODE" }

if ($SkipInstall) {
    Write-Host "Skipping install. Binary under build\ or build\Release\"
    exit 0
}

Write-Host "Installing to $Prefix ..."
Get-Process archstreamer_gui -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
cmake --install build --config $Config --prefix $Prefix
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed" }

$finish = Join-Path $RepoRoot "deploy\windows\finish-install.ps1"
$finishArgs = @{
    Prefix = $Prefix
    VcpkgRoot = $VcpkgRoot
    Shortcuts = $true
}
if ($Launch) { $finishArgs["Launch"] = $true }
& $finish @finishArgs

Write-Host ""
Write-Host "Done. Launch from Start Menu / Desktop (ArchStreamer), or:"
Write-Host "  & `"$Prefix\bin\archstreamer_gui.exe`""
