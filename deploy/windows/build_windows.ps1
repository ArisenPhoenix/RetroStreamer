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
    [switch]$Ninja,
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

function Get-CMakeCacheGenerator([string]$CachePath) {
    if (-not (Test-Path $CachePath)) { return "" }
    $line = Select-String -Path $CachePath -Pattern "^CMAKE_GENERATOR:" | Select-Object -First 1
    if (-not $line) { return "" }
    return ($line.Line -split "=", 2)[1].Trim()
}

function Ensure-MsvcOnPath {
    if (Get-Command cl -ErrorAction SilentlyContinue) { return $true }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $false }
    $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ([string]::IsNullOrWhiteSpace($install)) { return $false }
    $vcvars = Join-Path $install "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) { return $false }
    $envDump = cmd.exe /c "`"$vcvars`" && set"
    if ($LASTEXITCODE -ne 0) { return $false }
    foreach ($line in ($envDump -split "`r?`n")) {
        if ($line -match "^(.*?)=(.*)$") {
            Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
        }
    }
    return [bool](Get-Command cl -ErrorAction SilentlyContinue)
}

function Get-VsCMakeGeneratorArgs {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $year = "2022"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property catalog_productLineVersion
        if (-not [string]::IsNullOrWhiteSpace($found)) { $year = $found.Trim() }
    }
    $name = switch ($year) {
        "2019" { "Visual Studio 16 2019" }
        "2017" { "Visual Studio 15 2017" }
        Default { "Visual Studio 17 2022" }
    }
    return @("-G", $name, "-A", "x64")
}

$msvcReady = Ensure-MsvcOnPath
$existingGen = Get-CMakeCacheGenerator $cacheFile
if (($existingGen -eq "Ninja") -and (-not $msvcReady) -and (Test-Path $buildDir) -and (-not $Clean)) {
    Write-Host "Existing build/ is a Ninja tree but cl.exe is unavailable; cleaning for Visual Studio generator."
    Remove-Item -Recurse -Force $buildDir
    $existingGen = ""
}

$needsConfigure = $Reconfigure -or $Clean -or -not (Test-Path $cacheFile)
if ($needsConfigure) {
    Write-Host "Configuring CMake (ARCHSTREAMER_BUILD_HOST=$hostFlag)..."

    # Only pick a generator for a new build tree. Reconfigure must keep the
    # existing generator (VS vs Ninja) or cmake errors out.
    $generatorArgs = @()
    $freshTree = $Clean -or -not (Test-Path $cacheFile)
    $preferNinja = $Ninja -or ($env:ARCHSTREAMER_WINDOWS_NINJA -in @("1", "true", "yes"))
    if ($freshTree -and $preferNinja -and (Get-Command ninja -ErrorAction SilentlyContinue) -and $msvcReady) {
        $generatorArgs = @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Config")
        Write-Host "Using Ninja generator."
    } elseif ($freshTree) {
        $generatorArgs = Get-VsCMakeGeneratorArgs
        Write-Host "Using $($generatorArgs[1]) (-A x64)."
    } else {
        Write-Host "Reconfigure: keeping existing generator from build cache."
    }

    cmake -S . -B $buildDir `
        @generatorArgs `
        -DARCHSTREAMER_BUILD_HOST=$hostFlag `
        -DCMAKE_TOOLCHAIN_FILE="$toolchain"
    if ($LASTEXITCODE -ne 0) {
        throw "cmake configure failed with exit $LASTEXITCODE"
    }
} else {
    Write-Host "Reusing existing build cache ($cacheFile)."
    Write-Host "Pass -Reconfigure to refresh cmake options, or -Clean for a full rebuild."
}

if ($Jobs -le 0) {
    $Jobs = [Math]::Max(2, [Environment]::ProcessorCount)
}

Write-Host "Building ($Config, -j$Jobs)..."
cmake --build $buildDir --config $Config --parallel $Jobs
if ($LASTEXITCODE -ne 0) {
    throw "cmake --build failed with exit $LASTEXITCODE"
}