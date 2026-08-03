# Finish a Program Files install after:
#   cmake --install build --prefix "C:\Program Files\ArchStreamer"
#
# Run from the repo root. Prefer Admin PowerShell if writing under Program Files
# (required for All Users Start Menu / Public Desktop shortcuts).
#
# Usage:
#   .\deploy\windows\finish-install.ps1
#   .\deploy\windows\finish-install.ps1 -Prefix "C:\Program Files\ArchStreamer" -VcpkgRoot "C:\dev\vcpkg"
#   .\deploy\windows\finish-install.ps1 -Launch
#   .\deploy\windows\finish-install.ps1 -Shortcuts -CurrentUserOnly

param(
    [string]$Prefix = "C:\Program Files\ArchStreamer",
    [string]$VcpkgRoot = "",
    [switch]$Launch,
    [switch]$Shortcuts,
    # Default with -Shortcuts is All Users (Common Start Menu + Public Desktop).
    # Pass -CurrentUserOnly for per-user shortcuts only.
    [switch]$CurrentUserOnly
)

$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function New-ArchStreamerShortcut {
    param(
        [Parameter(Mandatory = $true)][string]$LinkPath,
        [Parameter(Mandatory = $true)][string]$TargetPath,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [string]$Description = "ArchStreamer"
    )
    $dir = Split-Path -Parent $LinkPath
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $w = New-Object -ComObject WScript.Shell
    $lnk = $w.CreateShortcut($LinkPath)
    $lnk.TargetPath = $TargetPath
    $lnk.WorkingDirectory = $WorkingDirectory
    $lnk.Description = $Description
    if (Test-Path $TargetPath) {
        # Prefer the icon embedded in archstreamer_gui.exe (branding .ico at build time).
        $lnk.IconLocation = "$TargetPath,0"
    }
    $lnk.Save()
    Write-Host "Shortcut: $LinkPath"
}

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
    $wantAllUsers = -not $CurrentUserOnly
    $isAdmin = Test-IsAdmin
    if ($wantAllUsers -and -not $isAdmin) {
        Write-Warning "All Users Start Menu / Public Desktop need Administrator. Falling back to current-user shortcuts."
        $wantAllUsers = $false
    }

    $shortcutDirs = @()
    if ($wantAllUsers) {
        # Visible to every account on this PC.
        $commonPrograms = [Environment]::GetFolderPath("CommonPrograms")
        $commonDesktop = [Environment]::GetFolderPath("CommonDesktopDirectory")
        if (-not [string]::IsNullOrWhiteSpace($commonPrograms)) {
            $shortcutDirs += (Join-Path $commonPrograms "ArchStreamer")
        }
        if (-not [string]::IsNullOrWhiteSpace($commonDesktop)) {
            $shortcutDirs += $commonDesktop
        }
        Write-Host "Installing All Users shortcuts (system-wide)..."
    } else {
        $userPrograms = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\ArchStreamer"
        $userDesktop = [Environment]::GetFolderPath("Desktop")
        $shortcutDirs += $userPrograms
        if (-not [string]::IsNullOrWhiteSpace($userDesktop)) {
            $shortcutDirs += $userDesktop
        }
        Write-Host "Installing current-user shortcuts only..."
    }

    foreach ($dir in $shortcutDirs) {
        if ([string]::IsNullOrWhiteSpace($dir)) { continue }
        try {
            New-ArchStreamerShortcut `
                -LinkPath (Join-Path $dir "ArchStreamer.lnk") `
                -TargetPath $exe `
                -WorkingDirectory $binDir `
                -Description "ArchStreamer"
        } catch {
            Write-Warning "Could not write shortcut under $dir : $($_.Exception.Message)"
        }
    }

    $hostExe = Join-Path $binDir "host_runner.exe"
    if ((Test-Path $hostExe) -and $wantAllUsers) {
        $commonPrograms = [Environment]::GetFolderPath("CommonPrograms")
        if (-not [string]::IsNullOrWhiteSpace($commonPrograms)) {
            $hostDir = Join-Path $commonPrograms "ArchStreamer"
            try {
                New-ArchStreamerShortcut `
                    -LinkPath (Join-Path $hostDir "ArchStreamer Host (CLI).lnk") `
                    -TargetPath $hostExe `
                    -WorkingDirectory $binDir `
                    -Description "ArchStreamer host_runner"
            } catch {
                Write-Warning "Could not write host_runner shortcut: $($_.Exception.Message)"
            }
        }
    }
}

Write-Host ""
Write-Host "Install ready: $exe"
Write-Host "GStreamer must still be on PATH (gst-launch-1.0)."
if ($Shortcuts) {
    if (-not $CurrentUserOnly -and (Test-IsAdmin)) {
        Write-Host "Start Menu (all users): Programs\ArchStreamer\ArchStreamer"
        Write-Host "Desktop (all users): Public Desktop\ArchStreamer"
    } else {
        Write-Host "Start Menu (this user): Programs\ArchStreamer\ArchStreamer"
    }
}
if ($Launch) {
    Start-Process -FilePath $exe -WorkingDirectory $binDir
}
