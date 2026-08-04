# Pull latest from GitHub, build the Windows client, and install to Program Files.
#
# From the repo root (usually Documents\RetroStreamer), Admin PowerShell recommended:
#   .\deploy\windows\update-and-install.ps1
#   .\deploy\windows\update-and-install.ps1 -ResetHard
#   .\deploy\windows\update-and-install.ps1 -ResetHard -Branch dev
#
# Close ArchStreamer / session_client before install (the script also tries to stop them);
# otherwise cmake --install can fail with permission denied on bin\*.exe.
#
# Options:
#   -Branch        git branch to pull (default: master — deploy / stable)
#   -ResetHard     discard local edits, match origin/<Branch> (typical for a client PC)
#   -SkipPull      build/install only (no git)
#   -SkipInstall   build only
#   -BuildHost     host-capable GUI (needs ViGEm etc.)
#   -Reconfigure   force cmake reconfigure
#   -Clean         wipe build/ first
#   -Launch        start the installed GUI when done
#   -Prefix        install root (default: C:\Program Files\ArchStreamer)
#   -VcpkgRoot     default C:\dev\vcpkg or $env:VCPKG_ROOT
#
# Day-to-day after Linux pushes to GitHub (deploy branch):
#   .\deploy\windows\update-and-install.ps1 -ResetHard
#
# Test a non-deploy branch in isolation:
#   .\deploy\windows\update-and-install.ps1 -ResetHard -Branch dev

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
$BuildPs1 = Join-Path $RepoRoot "deploy\windows\build_windows.ps1"
if (-not (Test-Path $BuildPs1)) {
    $BuildPs1 = Join-Path $RepoRoot "build_windows.ps1"
}
if (-not (Test-Path $BuildPs1)) {
    throw "Could not find build_windows.ps1 under deploy\windows or repo root: $RepoRoot"
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    if ($env:VCPKG_ROOT) {
        $VcpkgRoot = $env:VCPKG_ROOT
    } else {
        $VcpkgRoot = "C:\dev\vcpkg"
    }
}

if ([string]::IsNullOrWhiteSpace($Branch)) {
    throw "-Branch must not be empty (default is master)"
}

Set-Location $RepoRoot
Write-Host "=== ArchStreamer Windows update ==="
Write-Host "Repo: $RepoRoot"
Write-Host "Branch: $Branch"

if (-not $SkipPull) {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw "git not found on PATH"
    }
    Write-Host "Fetching origin..."
    git fetch origin
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed" }

    $remoteRef = "origin/$Branch"
    $remoteExists = @(git rev-parse --verify $remoteRef 2>$null)
    if ($LASTEXITCODE -ne 0 -or $remoteExists.Count -eq 0) {
        throw "Remote branch not found: $remoteRef (push it first, or check -Branch spelling)"
    }

    $status = @(git status --porcelain)
    if ($ResetHard) {
        Write-Host "Resetting to $remoteRef (discarding local changes)..."
        git checkout $Branch
        if ($LASTEXITCODE -ne 0) {
            # First time checking out a remote-only branch on this machine.
            git checkout -B $Branch $remoteRef
        }
        if ($LASTEXITCODE -ne 0) { throw "git checkout $Branch failed" }
        git reset --hard $remoteRef
        if ($LASTEXITCODE -ne 0) { throw "git reset --hard failed" }
        git clean -fd
    } elseif ($status.Count -gt 0) {
        Write-Host "Working tree has local changes:"
        git status -sb
        throw "Refusing to pull over dirty tree. Re-run with -ResetHard, or commit/stash locally, or pass -SkipPull."
    } else {
        Write-Host "Pulling $remoteRef..."
        git checkout $Branch
        if ($LASTEXITCODE -ne 0) {
            git checkout -B $Branch $remoteRef
        }
        if ($LASTEXITCODE -ne 0) { throw "git checkout $Branch failed" }
        git pull --ff-only origin $Branch
        if ($LASTEXITCODE -ne 0) { throw "git pull failed (try -ResetHard if histories diverged)" }
    }
    Write-Host "Git: $(git rev-parse --short HEAD) $(git log -1 --pretty=%s) [$Branch]"
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
& $BuildPs1 @buildArgs

if ($SkipInstall) {
    Write-Host "Skipping install. Binary under build\ or build\Release\"
    exit 0
}

Write-Host "Installing to $Prefix ..."
# cmake --install overwrites bin\*.exe; Windows locks running images (permission denied).
$installBin = Join-Path $Prefix "bin"
$procsToStop = @(
    "archstreamer_gui",
    "session_client",
    "host_runner",
    "client_catalog_probe",
    "game_catalog_probe",
    "asset_probe",
    "steam_art_import",
    "uinput_probe",
    "controller_probe"
)
foreach ($name in $procsToStop) {
    Get-Process -Name $name -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 500

# If something still holds an installed exe (Explorer preview, AV), retry a few times.
$installOk = $false
for ($attempt = 1; $attempt -le 5; $attempt++) {
    cmake --install build --config $Config --prefix $Prefix
    if ($LASTEXITCODE -eq 0) {
        $installOk = $true
        break
    }
    Write-Warning "cmake --install failed (attempt $attempt/5). Retrying after stopping processes again..."
    foreach ($name in $procsToStop) {
        Get-Process -Name $name -ErrorAction SilentlyContinue |
            Stop-Process -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 1
}
if (-not $installOk) {
    throw @"
cmake --install failed (often permission denied on $installBin\*.exe).

Common causes:
  1. ArchStreamer / session_client still running — close them (Task Manager).
  2. Not elevated — Program Files needs Admin PowerShell.
  3. Antivirus briefly locking the new binaries — retry.

Then re-run:
  .\deploy\windows\update-and-install.ps1 -SkipPull
"@
}

$finish = Join-Path $RepoRoot "deploy\windows\finish-install.ps1"
$finishArgs = @{
    Prefix = $Prefix
    VcpkgRoot = $VcpkgRoot
    Shortcuts = $true
}
if ($Launch) { $finishArgs["Launch"] = $true }
& $finish @finishArgs

Write-Host ""
Write-Host "Done. All users can launch ArchStreamer from the Start Menu"
Write-Host "  (Programs → ArchStreamer) or Public Desktop."
Write-Host "Or run:"
Write-Host "  & `"$Prefix\bin\archstreamer_gui.exe`""
