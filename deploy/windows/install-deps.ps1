# Windows dependency installer for ArchStreamer client + host.
# Run elevated (Administrator) for ViGEmBus driver install.
#
# Usage:
#   .\deploy\windows\install-deps.ps1
#   .\deploy\windows\install-deps.ps1 -OpenFirewall
#   .\deploy\windows\install-deps.ps1 -InstallBuildTools
#   .\build_windows.ps1 -InstallDeps

param(
    [switch]$OpenFirewall,
    [switch]$InstallBuildTools,
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\dev\vcpkg" })
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Ensure-Winget {
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        return $true
    }
    Write-Warning "winget not found. Install 'App Installer' from the Microsoft Store, then re-run."
    return $false
}

function Add-UserPath([string]$Dir) {
    if (-not (Test-Path $Dir)) { return }
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($userPath -split ';' | Where-Object { $_ -ieq $Dir }) {
        return
    }
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$Dir", "User")
    $env:Path = "$env:Path;$Dir"
    Write-Host "Added to user PATH: $Dir"
}

Write-Host "=== ArchStreamer Windows deps ==="
if (-not (Test-IsAdmin)) {
    Write-Warning "Not elevated — ViGEmBus driver install may fail. Re-run in an Admin PowerShell."
}

# --- Build tools ---
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "CMake missing."
    if ($InstallBuildTools -and (Ensure-Winget)) {
        winget install -e --id Kitware.CMake --accept-package-agreements --accept-source-agreements
    } else {
        Write-Host "  Install: winget install Kitware.CMake   (or pass -InstallBuildTools)"
    }
} else {
    Write-Host "CMake: OK"
}

if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    Write-Host "Ninja missing (optional but recommended)."
    if ($InstallBuildTools -and (Ensure-Winget)) {
        winget install -e --id Ninja-build.Ninja --accept-package-agreements --accept-source-agreements
    } else {
        Write-Host "  Install: winget install Ninja-build.Ninja"
    }
} else {
    Write-Host "Ninja: OK"
}

# --- vcpkg Qt/SDL2 ---
$toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $toolchain)) {
    Write-Warning "vcpkg not found at $VcpkgRoot"
    Write-Host "  Clone https://github.com/microsoft/vcpkg and bootstrap, or set VCPKG_ROOT."
} else {
    Write-Host "vcpkg: $VcpkgRoot"
    $vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
    if (Test-Path $vcpkg) {
        Write-Host "Installing vcpkg packages: qtbase[widgets], sdl2 ..."
        & $vcpkg install qtbase[widgets]:x64-windows sdl2:x64-windows
    }
}

# --- GStreamer MSVC ---
$gstLaunch = Get-Command gst-launch-1.0.exe -ErrorAction SilentlyContinue
if (-not $gstLaunch) {
    Write-Host "GStreamer MSVC not on PATH."
    $gstUrls = @(
        "https://gstreamer.freedesktop.org/data/pkg/windows/1.24.12/msvc/gstreamer-1.0-msvc-x86_64-1.24.12.msi",
        "https://gstreamer.freedesktop.org/data/pkg/windows/1.24.12/msvc/gstreamer-1.0-devel-msvc-x86_64-1.24.12.msi"
    )
    Write-Host "  Manual MSI downloads (runtime + devel):"
    foreach ($u in $gstUrls) { Write-Host "    $u" }
    Write-Host "  After install, add e.g. C:\Program Files\gstreamer\1.0\msvc_x86_64\bin to PATH."
    if (Ensure-Winget) {
        Write-Host "  Trying winget GStreamer packages (IDs vary by catalog)..."
        winget search GStreamer 2>$null | Select-Object -First 15
    }
} else {
    Write-Host "GStreamer: $($gstLaunch.Source)"
    $bin = Split-Path $gstLaunch.Source -Parent
    Add-UserPath $bin
    Write-Host "  Checking host plugins..."
    foreach ($el in @("d3d11screencapturesrc", "wasapisrc", "x264enc", "opusenc", "multiudpsink")) {
        $probe = & gst-inspect-1.0.exe $el 2>$null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "    $el OK"
        } else {
            Write-Warning "    $el missing — install GStreamer plugins / complete package"
        }
    }
}

# --- ViGEmBus ---
$vigemSvc = Get-Service -Name "ViGEmBus" -ErrorAction SilentlyContinue
if (-not $vigemSvc) {
    Write-Host "ViGEmBus driver not detected."
    if (Ensure-Winget) {
        Write-Host "  Installing Nefarius ViGEmBus via winget..."
        winget install -e --id Nefarius.ViGEmBus --accept-package-agreements --accept-source-agreements
    } else {
        Write-Host "  Download: https://github.com/nefarius/ViGEmBus/releases"
    }
    Write-Host "  Also place ViGEmClient.dll on PATH (from Nefarius.ViGEm.Client NuGet / SDK)."
} else {
    Write-Host "ViGEmBus service: $($vigemSvc.Status)"
}

# --- Yuzu (manual) ---
$yuzuManaged = Join-Path $env:LOCALAPPDATA "archstreamer\yuzu\yuzu.exe"
Write-Host ""
Write-Host "Yuzu is NOT auto-downloaded."
Write-Host "  Copy your yuzu-windows-msvc folder to:"
Write-Host "    $env:LOCALAPPDATA\archstreamer\yuzu\"
Write-Host "  or set ARCHSTREAMER_YUZU to yuzu.exe (or its folder)."
Write-Host "  Keys: %APPDATA%\yuzu\keys\prod.keys  (or ARCHSTREAMER_YUZU_KEYS)"
if (Test-Path $yuzuManaged) {
    Write-Host "  Managed yuzu.exe found: $yuzuManaged"
} else {
    Write-Warning "  Managed yuzu.exe not found yet."
}

# --- Firewall ---
if ($OpenFirewall) {
    if (-not (Test-IsAdmin)) {
        Write-Warning "-OpenFirewall requires elevation."
    } else {
        $rules = @(
            @{ Name = "ArchStreamer Control TCP"; Port = 45555; Proto = "TCP" },
            @{ Name = "ArchStreamer Input UDP"; Port = 45454; Proto = "UDP" },
            @{ Name = "ArchStreamer Video RTP"; Port = 5004; Proto = "UDP" },
            @{ Name = "ArchStreamer Audio RTP"; Port = 6004; Proto = "UDP" },
            @{ Name = "ArchStreamer LAN Advertise"; Port = 45550; Proto = "UDP" }
        )
        foreach ($r in $rules) {
            $existing = Get-NetFirewallRule -DisplayName $r.Name -ErrorAction SilentlyContinue
            if (-not $existing) {
                New-NetFirewallRule -DisplayName $r.Name -Direction Inbound -Action Allow `
                    -Protocol $r.Proto -LocalPort $r.Port | Out-Null
                Write-Host "Firewall allowed: $($r.Name) $($r.Proto)/$($r.Port)"
            } else {
                Write-Host "Firewall already present: $($r.Name)"
            }
        }
    }
} else {
    Write-Host ""
    Write-Host "Firewall (optional): re-run with -OpenFirewall, or:"
    Write-Host '  New-NetFirewallRule -DisplayName "ArchStreamer Control TCP" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 45555'
}

Write-Host ""
Write-Host "Done. Build with:"
Write-Host "  .\build_windows.ps1 -Reconfigure"
Write-Host "  # host-capable:"
Write-Host "  cmake -S . -B build -DARCHSTREAMER_BUILD_HOST=ON -DCMAKE_TOOLCHAIN_FILE=`"$toolchain`""
