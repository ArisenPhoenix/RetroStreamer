# Finish a Program Files install after:
#   cmake --install build --prefix "C:\Program Files\ArchStreamer"
#
# Run from the repo root. Prefer Admin PowerShell if writing under Program Files.
#
# Usage:
#   .\deploy\windows\finish-install.ps1
#   .\deploy\windows\finish-install.ps1 -Prefix "C:\Program Files\ArchStreamer" -VcpkgRoot "C:\dev\vcpkg"
#   .\deploy\windows\finish-install.ps1 -Launch

param(
    [string]$Prefix = "C:\Program Files\ArchStreamer",
    [string]$VcpkgRoot = "",
    [switch]$Launch,
    [switch]$Shortcuts
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

$vcpkgBin = Join-Path $VcpkgRoot "installed\x64-windows\bin"
if (-not (Test-Path $vcpkgBin)) {
    throw "vcpkg bin not found: $vcpkgBin (pass -VcpkgRoot)"
}

# --- SDL2.dll (build tree first, then vcpkg) ---
$sdlCandidates = @(
    (Join-Path (Get-Location) "build\SDL2.dll"),
    (Join-Path (Get-Location) "build\Release\SDL2.dll"),
    (Join-Path $vcpkgBin "SDL2.dll")
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

# --- windeployqt (Qt plugins + core Qt DLLs) ---
$deployCandidates = @(
    (Join-Path $VcpkgRoot "installed\x64-windows\tools\Qt6\bin\windeployqt.exe"),
    (Join-Path $VcpkgRoot "installed\x64-windows\tools\Qt6\bin\windeployqt6.exe")
)
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
    throw "windeployqt.exe not found under $VcpkgRoot"
}

Write-Host "Using windeployqt: $foundDeploy"
& $foundDeploy --release $exe
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

# windeployqt often skips Qt's vcpkg transitive deps; copy them explicitly.
$qtDeps = @(
    "libpng16.dll",
    "harfbuzz.dll",
    "md4c.dll",
    "freetype.dll",
    "zlib1.dll",
    "double-conversion.dll",
    "pcre2-16.dll",
    "zstd.dll",
    "bz2.dll",
    "brotlidec.dll",
    "brotlicommon.dll",
    "brotlienc.dll"
)
foreach ($name in $qtDeps) {
    $src = Join-Path $vcpkgBin $name
    if (Test-Path $src) {
        Copy-Item $src $binDir -Force
        Write-Host "Copied Qt dep $name"
    } else {
        Write-Warning "Optional Qt dep missing in vcpkg: $name"
    }
}

if (-not (Test-Path (Join-Path $binDir "platforms\qwindows.dll"))) {
    throw "platforms\qwindows.dll missing after windeployqt"
}

if ($Shortcuts) {
    $w = New-Object -ComObject WScript.Shell
    foreach ($dir in @(
        [Environment]::GetFolderPath("Desktop"),
        (Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs")
    )) {
        if (-not (Test-Path $dir)) { continue }
        $lnk = $w.CreateShortcut((Join-Path $dir "ArchStreamer.lnk"))
        $lnk.TargetPath = $exe
        $lnk.WorkingDirectory = $binDir
        $lnk.Description = "ArchStreamer"
        $lnk.Save()
        Write-Host "Shortcut: $(Join-Path $dir 'ArchStreamer.lnk')"
    }
}

Write-Host ""
Write-Host "Install ready: $exe"
Write-Host "GStreamer must still be on PATH (gst-launch-1.0)."
if ($Launch) {
    Start-Process -FilePath $exe -WorkingDirectory $binDir
}
