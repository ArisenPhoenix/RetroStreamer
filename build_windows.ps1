# Windows build (client by default; pass -BuildHost for Yuzu host + ViGEm/DXGI).
# Runtime: SDL2.dll (copied on build) + GStreamer MSVC 64-bit on PATH.
# Host extras: ViGEmBus + ViGEmClient.dll  -  see deploy/windows/install-deps.ps1
# See deploy/windows/README.md.
#
# Usage:
#   .\build_windows.ps1              # incremental client build
#   .\build_windows.ps1 -InstallDeps # run deploy/windows/install-deps.ps1 then build
#   .\build_windows.ps1 -BuildHost   # ARCHSTREAMER_BUILD_HOST=ON
#   .\build_windows.ps1 -Reconfigure
#   .\build_windows.ps1 -Clean

param(
    [switch]$Reconfigure,
    [switch]$Clean,
    [switch]$InstallDeps,
    [switch]$BuildHost,
    [string]$VcpkgRoot = "",
    [string]$Config = "Release",
    [int]$Jobs = 0
)

# Avoid $(if { } else { }) inside param()  -  Windows PowerShell can report
# "Unexpected token '}'" on the param block.
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    if ($env:VCPKG_ROOT) {
        $VcpkgRoot = $env:VCPKG_ROOT
    } else {
        $VcpkgRoot = "C:\dev\vcpkg"
    }
}

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Set-Location $PSScriptRoot

if ($InstallDeps) {
    $deps = Join-Path $PSScriptRoot "deploy\windows\install-deps.ps1"
    Write-Host "Running $deps ..."
    & $deps -VcpkgRoot $VcpkgRoot
}

$buildDir = Join-Path $PSScriptRoot "build"
$cacheFile = Join-Path $buildDir "CMakeCache.txt"
$toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"

if (-not (Test-Path $toolchain)) {
    Write-Error "vcpkg toolchain not found: $toolchain`nSet VCPKG_ROOT or pass -VcpkgRoot."
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir ..."
    Remove-Item -Recurse -Force $buildDir
}

$hostFlag = if ($BuildHost) { "ON" } else { "OFF" }
$needsConfigure = $Reconfigure -or $Clean -or -not (Test-Path $cacheFile)
if ($needsConfigure) {
    Write-Host "Configuring CMake (ARCHSTREAMER_BUILD_HOST=$hostFlag)..."

    # Only pick a generator for a new build tree. Reconfigure must keep the
    # existing generator (VS vs Ninja) or cmake errors out.
    $generatorArgs = @()
    $freshTree = $Clean -or -not (Test-Path $cacheFile)
    if ($freshTree -and (Get-Command ninja -ErrorAction SilentlyContinue)) {
        $generatorArgs = @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Config")
        Write-Host "Using Ninja generator."
    } elseif ($freshTree) {
        Write-Host "Ninja not found; using CMake's default generator (often Visual Studio)."
        Write-Host "Tip: install Ninja and re-run with -Clean for faster incremental builds."
    } else {
        Write-Host "Reconfigure: keeping existing generator from build cache."
    }

    cmake -S . -B $buildDir `
        @generatorArgs `
        -DARCHSTREAMER_BUILD_HOST=$hostFlag `
        -DCMAKE_TOOLCHAIN_FILE="$toolchain"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
    Write-Host "Reusing existing build cache ($cacheFile)."
    Write-Host "Pass -Reconfigure to refresh cmake options, or -Clean for a full rebuild."
}

if ($Jobs -le 0) {
    $Jobs = [Math]::Max(2, [Environment]::ProcessorCount)
}

Write-Host "Building ($Config, -j$Jobs)..."
cmake --build $buildDir --config $Config --parallel $Jobs
exit $LASTEXITCODE
