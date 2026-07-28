# Finish a Program Files install after:
#   cmake --install build --prefix "C:\Program Files\ArchStreamer"
#
# Run from the repo root in an Admin PowerShell if writing under Program Files.
#
# Usage:
#   .\deploy\windows\finish-install.ps1
#   .\deploy\windows\finish-install.ps1 -Prefix "C:\Program Files\ArchStreamer" -VcpkgRoot "C:\dev\vcpkg"

param(
    [string]$Prefix = "C:\Program Files\ArchStreamer",
    [string]$VcpkgRoot = "",
    [switch]$Launch
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    if ($env:VCPKG_ROOT) {
        $VcpkgRoot = $env:VCPKG_ROOT
    } else {
        $VcpkgRoot = "C:\dev\vcpkg"
    }
}

$binDir = Join-Path $Prefix "bin"
$exe = Join-Path $binDir "archstreamer_gui.exe"
if (-not (Test-Path $exe)) {
    throw "Missing $exe - run: cmake --install build --prefix `"$Prefix`""
}

# --- SDL2.dll (build tree first, then vcpkg) ---
$sdlCandidates = @(
    (Join-Path (Get-Location) "build\SDL2.dll"),
    (Join-Path (Get-Location) "build\Release\SDL2.dll"),
    (Join-Path $VcpkgRoot "installed\x64-windows\bin\SDL2.dll")
)
$sdlCopied = $false
foreach ($src in $sdlCandidates) {
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $binDir "SDL2.dll") -Force
        Write-Host "Copied SDL2.dll from $src"
        $sdlCopied = $true
        break
    }
}
if (-not $sdlCopied) {
    Write-Warning "SDL2.dll not found. Controllers may fail until you copy it into $binDir"
}

# --- windeployqt (Qt is usually only under vcpkg, not a system install) ---
$deployCandidates = @(
    (Join-Path $VcpkgRoot "installed\x64-windows\tools\Qt6\bin\windeployqt.exe"),
    (Join-Path $VcpkgRoot "installed\x64-windows\tools\Qt6\bin\windeployqt6.exe")
)
# Also search if the default layout differs
$foundDeploy = $null
foreach ($c in $deployCandidates) {
    if (Test-Path $c) {
        $foundDeploy = $c
        break
    }
}
if (-not $foundDeploy -and (Test-Path $VcpkgRoot)) {
    $hit = Get-ChildItem -Path $VcpkgRoot -Filter "windeployqt*.exe" -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($hit) {
        $foundDeploy = $hit.FullName
    }
}

if (-not $foundDeploy) {
    Write-Host ""
    Write-Host "windeployqt not found under: $VcpkgRoot"
    Write-Host "Qt is probably installed via vcpkg, but VCPKG_ROOT was wrong/empty."
    Write-Host "Find it, then re-run with -VcpkgRoot:"
    Write-Host '  Get-ChildItem -Path C:\ -Filter windeployqt.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 5 FullName'
    Write-Host '  .\deploy\windows\finish-install.ps1 -VcpkgRoot "C:\path\to\vcpkg"'
    throw "windeployqt.exe not found"
}

Write-Host "Using windeployqt: $foundDeploy"
& $foundDeploy $exe
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Install ready: $exe"
Write-Host "GStreamer must still be on PATH (gst-launch-1.0)."
if ($Launch) {
    & $exe
}
