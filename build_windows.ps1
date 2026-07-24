# Usage:
#   .\build_windows.ps1              # incremental build (reconfigure only if needed)
#   .\build_windows.ps1 -Reconfigure # force cmake reconfigure, then build
#   .\build_windows.ps1 -Clean       # delete build\ and reconfigure from scratch

param(
    [switch]$Reconfigure,
    [switch]$Clean,
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\dev\vcpkg" }),
    [string]$Config = "Release",
    [int]$Jobs = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Set-Location $PSScriptRoot

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

$needsConfigure = $Reconfigure -or $Clean -or -not (Test-Path $cacheFile)
if ($needsConfigure) {
    Write-Host "Configuring CMake (this is the slow step; skipped on later incremental builds)..."

    # Prefer Ninja for fast incremental rebuilds when available.
    $generatorArgs = @()
    if (Get-Command ninja -ErrorAction SilentlyContinue) {
        $generatorArgs = @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Config")
        Write-Host "Using Ninja generator."
    } else {
        Write-Host "Ninja not found; using CMake's default generator (often Visual Studio)."
        Write-Host "Tip: install Ninja and re-run with -Clean for faster incremental builds."
    }

    cmake -S . -B $buildDir `
        @generatorArgs `
        -DARCHSTREAMER_BUILD_HOST=OFF `
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
# Multi-config generators (Visual Studio) need --config; Ninja uses CMAKE_BUILD_TYPE.
cmake --build $buildDir --config $Config --parallel $Jobs
exit $LASTEXITCODE
