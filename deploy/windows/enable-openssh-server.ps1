# Enable OpenSSH Server on Windows (for LAN log pulls / remote admin).
# Run elevated (Administrator):
#   .\deploy\windows\enable-openssh-server.ps1
#
# Then from Linux:
#   ssh merk@<host>

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdmin)) {
    Write-Error "Run this script as Administrator (right-click PowerShell → Run as administrator)."
}

$capabilityName = "OpenSSH.Server~~~~0.0.1.0"
$cap = Get-WindowsCapability -Online -Name $capabilityName -ErrorAction SilentlyContinue
if ($null -eq $cap) {
    Write-Error "OpenSSH.Server capability not found on this Windows edition."
}

if ($cap.State -ne "Installed") {
    Write-Host "Installing OpenSSH Server..."
    Add-WindowsCapability -Online -Name $capabilityName | Out-Null
} else {
    Write-Host "OpenSSH Server is already installed."
}

$sshd = Get-Service -Name sshd -ErrorAction SilentlyContinue
if ($null -eq $sshd) {
    Write-Error "sshd service not found after install."
}

Write-Host "Configuring sshd to start automatically..."
Set-Service -Name sshd -StartupType Automatic
if ($sshd.Status -ne "Running") {
    Start-Service sshd
} else {
    Write-Host "sshd is already running."
}

# Optional auth agent (harmless if unused).
$agent = Get-Service -Name ssh-agent -ErrorAction SilentlyContinue
if ($null -ne $agent) {
    Set-Service -Name ssh-agent -StartupType Manual -ErrorAction SilentlyContinue
}

$ruleName = "OpenSSH-Server-In-TCP"
$existing = Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue
if ($null -eq $existing) {
    Write-Host "Adding firewall rule for TCP 22..."
    New-NetFirewallRule `
        -Name $ruleName `
        -DisplayName "OpenSSH Server (sshd)" `
        -Enabled True `
        -Direction Inbound `
        -Protocol TCP `
        -Action Allow `
        -LocalPort 22 | Out-Null
} else {
    Enable-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue | Out-Null
    Write-Host "Firewall rule already present; ensured enabled."
}

$listening = Get-NetTCPConnection -LocalPort 22 -State Listen -ErrorAction SilentlyContinue
if ($null -eq $listening) {
    Write-Warning "sshd is running but nothing is listening on port 22 yet. Wait a second and re-check."
} else {
    Write-Host "Listening on TCP 22."
}

Write-Host ""
Write-Host "Done. From another machine:"
Write-Host ("  ssh {0}@{1}" -f $env:USERNAME, (Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
    Select-Object -First 1 -ExpandProperty IPAddress))
Write-Host "Service status:"
Get-Service sshd | Format-Table -AutoSize Name, Status, StartType
