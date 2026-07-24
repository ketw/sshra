<#
.SYNOPSIS
    Mass (ms) - Laptop Setup
    Run once on your laptop. After this, just type: ms
.EXAMPLE
    irm https://raw.githubusercontent.com/ketw/sshra/master/setup-laptop.ps1 | iex
#>

$ErrorActionPreference = "Stop"

trap {
    $msg = $_.ToString()
    Write-Host ""
    Write-Host "  ERROR: $msg" -ForegroundColor Red
    Write-Host "  Press any key to close..." -ForegroundColor DarkGray
    try { $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown") } catch { Start-Sleep 15 }
    exit 1
}

# Self-elevate
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $tmp = "$env:TEMP\ms_setup_laptop.ps1"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    (New-Object System.Net.WebClient).DownloadFile(
        "https://raw.githubusercontent.com/ketw/sshra/master/setup-laptop.ps1", $tmp)
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$tmp`""
    exit 0
}

# Paths
$MsDir     = "C:\Program Files\Mass"
$MsConfig  = "$env:USERPROFILE\.ms"
$MsKeys    = "$MsConfig\keys"
$MsCfgFile = "$MsConfig\config.json"
$MgrExe    = "$MsDir\msmgr.exe"
$TempUrl   = "https://raw.githubusercontent.com/ketw/sshra/master/.temp"
$MgrB64Url = "$TempUrl/msmgr.exe.b64"

function Write-Step { param($m) Write-Host "  [..] $m" -ForegroundColor Cyan }
function Write-OK   { param($m) Write-Host "  [OK] $m" -ForegroundColor Green }
function Write-Warn { param($m) Write-Host "  [!!] $m" -ForegroundColor Yellow }

Clear-Host
Write-Host ""
Write-Host "  +----------------------------------------------+" -ForegroundColor Magenta
Write-Host "  |      Mass (ms) - Laptop Setup                |" -ForegroundColor Magenta
Write-Host "  |      After this, just type: ms               |" -ForegroundColor Magenta
Write-Host "  +----------------------------------------------+" -ForegroundColor Magenta
Write-Host ""

# Step 1: Directories
Write-Step "Creating directories..."
New-Item -ItemType Directory -Force -Path $MsDir    | Out-Null
New-Item -ItemType Directory -Force -Path $MsConfig | Out-Null
New-Item -ItemType Directory -Force -Path $MsKeys   | Out-Null
Write-OK "Directories ready"

# Step 2: Download msmgr.exe from .temp/ (base64 encoded)
Write-Step "Downloading msmgr.exe..."
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$downloaded = $false
for ($i = 1; $i -le 3; $i++) {
    try {
        $wc = New-Object System.Net.WebClient
        $wc.Headers.Add("User-Agent", "ms-setup/1.0")
        $b64   = $wc.DownloadString($MgrB64Url).Trim()
        $bytes = [Convert]::FromBase64String($b64)
        [IO.File]::WriteAllBytes($MgrExe, $bytes)
        if ((Test-Path $MgrExe) -and (Get-Item $MgrExe).Length -gt 10000) {
            Write-OK "Downloaded msmgr.exe ($([math]::Round((Get-Item $MgrExe).Length/1KB))KB)"
            $downloaded = $true
            break
        }
    } catch { Write-Warn "Attempt $i failed: $_" }
    Start-Sleep 3
}
if (-not $downloaded) {
    # Fallback: try local build copy
    $local = "p:\Projects\ssh-access\build\msmgr.exe"
    if (Test-Path $local) {
        Copy-Item $local $MgrExe -Force
        Write-OK "Copied msmgr.exe from local build"
    } else {
        throw "Could not download msmgr.exe"
    }
}

# Step 3: Add to system PATH
Write-Step "Adding ms to system PATH..."
$syspath = [Environment]::GetEnvironmentVariable("PATH", "Machine")
if ($syspath -notlike "*$MsDir*") {
    [Environment]::SetEnvironmentVariable("PATH", "$syspath;$MsDir", "Machine")
    $env:PATH = "$env:PATH;$MsDir"
    Write-OK "Added to system PATH"
} else {
    Write-OK "Already in PATH"
}

# Step 4: Create ms.cmd wrapper
Write-Step "Creating ms command..."
Set-Content -Path "$MsDir\ms.cmd" -Value "@echo off`r`n`"$MgrExe`" %*" -Encoding ASCII
Write-OK "Created ms.cmd - type ms in any new terminal"

# Step 5: Load relay config from .temp/.env (no prompt needed)
Write-Step "Loading relay config from repo..."
$relayHost  = ""
$relayPort  = "443"
$relayToken = ""

# Check existing local config first
if (Test-Path $MsCfgFile) {
    try {
        $existing = Get-Content $MsCfgFile -Raw | ConvertFrom-Json
        if ($existing.relay_host -and $existing.relay_token) {
            $relayHost  = $existing.relay_host
            $relayPort  = if ($existing.relay_port) { $existing.relay_port } else { "443" }
            $relayToken = $existing.relay_token
            Write-OK "Loaded from existing config: $relayHost`:$relayPort"
        }
    } catch {}
}

# Fetch from .temp/.env if not already set
if (-not $relayHost -or -not $relayToken) {
    try {
        $wc2 = New-Object System.Net.WebClient
        $wc2.Headers.Add("User-Agent", "ms-setup/1.0")
        $envRaw = $wc2.DownloadString("$TempUrl/.env")
        foreach ($line in ($envRaw -split "`n")) {
            $line = $line.Trim()
            if ($line -match "^\s*#" -or $line -eq "") { continue }
            if ($line -match "^([^=]+)=(.*)$") {
                $k = $Matches[1].Trim()
                $v = $Matches[2].Trim()
                if ($k -eq "RELAY_HOST")  { $relayHost  = $v }
                if ($k -eq "RELAY_PORT")  { $relayPort  = $v }
                if ($k -eq "RELAY_TOKEN") { $relayToken = $v }
            }
        }
        Write-OK "Config from repo: $relayHost`:$relayPort"
    } catch {
        Write-Warn "Could not fetch .env from repo: $_"
        # Only prompt if still empty
        if (-not $relayHost) {
            $relayHost = (Read-Host "  Relay host (e.g. ra-u9qf.onrender.com)").Trim()
            $relayHost = $relayHost -replace "^https?://", "" -replace "/$", ""
        }
        if (-not $relayToken) {
            $relayToken = (Read-Host "  Relay auth token").Trim()
        }
    }
}

# Step 6: Import device private keys
Write-Host ""
Write-Host "  ----------------------------------------------------" -ForegroundColor DarkGray
Write-Host "  Import private keys from your remote devices" -ForegroundColor White
Write-Host "  Each PC generated a key during msagent install." -ForegroundColor DarkGray
Write-Host "  Paste the private key when prompted." -ForegroundColor DarkGray
Write-Host "  Press Enter with no name when done." -ForegroundColor DarkGray
Write-Host "  ----------------------------------------------------" -ForegroundColor DarkGray
Write-Host ""

$importedKeys = @()

while ($true) {
    $deviceName = (Read-Host "  Device name (e.g. NISHU) or Enter to finish").Trim()
    if (-not $deviceName) { break }

    $safeName = $deviceName -replace "[^a-zA-Z0-9_-]", "_"
    $keyPath  = "$MsKeys\id_$safeName"

    # Skip duplicates
    if ($importedKeys | Where-Object { $_.SafeName -eq $safeName }) {
        Write-Warn "$deviceName already imported - skipping"
        continue
    }

    Write-Host ""
    Write-Host "  Paste the PRIVATE KEY for $deviceName" -ForegroundColor Yellow
    Write-Host "  Include the -----BEGIN----- and -----END----- lines." -ForegroundColor DarkGray
    Write-Host "  Type a single dot . on its own line when done." -ForegroundColor DarkGray
    Write-Host ""

    $lines = @()
    while ($true) {
        $line = Read-Host "  "
        if ($line -eq ".") { break }
        $lines += $line
    }
    $keyContent = ($lines -join "`n").Trim()

    if ($keyContent -match "BEGIN OPENSSH PRIVATE KEY|BEGIN RSA PRIVATE KEY|BEGIN EC PRIVATE KEY") {
        [IO.File]::WriteAllText($keyPath, $keyContent + "`n")
        try {
            $acl = New-Object System.Security.AccessControl.FileSecurity
            $acl.SetAccessRuleProtection($true, $false)
            $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(
                $env:USERNAME, "FullControl", "Allow")))
            Set-Acl -Path $keyPath -AclObject $acl
        } catch {
            icacls $keyPath /inheritance:r /grant "${env:USERNAME}:F" 2>$null | Out-Null
        }
        Write-OK "Key saved: $keyPath"
        $importedKeys += [PSCustomObject]@{ Device = $deviceName; SafeName = $safeName; KeyPath = $keyPath }
    } else {
        Write-Warn "Does not look like a valid private key - skipped"
    }
    Write-Host ""
}

# Step 7: Write config.json
Write-Step "Writing config..."
$keysMap = @{}
if (Test-Path $MsCfgFile) {
    try {
        $old = Get-Content $MsCfgFile -Raw | ConvertFrom-Json
        if ($old.keys) {
            $old.keys.PSObject.Properties | ForEach-Object { $keysMap[$_.Name] = $_.Value }
        }
    } catch {}
}
foreach ($k in $importedKeys) { $keysMap[$k.Device] = $k.KeyPath }

$cfgObj = [ordered]@{
    relay_host  = $relayHost
    relay_port  = $relayPort
    relay_token = $relayToken
    key_dir     = $MsKeys
    keys        = $keysMap
}
$cfgObj | ConvertTo-Json -Depth 4 | Set-Content -Path $MsCfgFile -Encoding UTF8
Write-OK "Config written: $MsCfgFile"

# Done
Write-Host ""
Write-Host "  +----------------------------------------------------+" -ForegroundColor Green
Write-Host "  |           Laptop Setup Complete!                   |" -ForegroundColor Green
Write-Host "  +----------------------------------------------------+" -ForegroundColor Green
Write-Host ""
Write-Host "  Install dir : $MsDir"     -ForegroundColor Cyan
Write-Host "  Config      : $MsCfgFile" -ForegroundColor Cyan
Write-Host "  Keys dir    : $MsKeys"    -ForegroundColor Cyan
Write-Host "  Relay       : $relayHost`:$relayPort" -ForegroundColor Cyan
Write-Host ""
if ($importedKeys.Count -gt 0) {
    Write-Host "  Imported keys:" -ForegroundColor White
    foreach ($k in $importedKeys) {
        Write-Host "    $($k.Device) -> $($k.KeyPath)" -ForegroundColor DarkGray
    }
    Write-Host ""
}
Write-Host "  Open a NEW terminal and type:" -ForegroundColor White
Write-Host ""
Write-Host "      ms" -ForegroundColor Green
Write-Host ""
Write-Host "  To add more device keys later, just run this setup again." -ForegroundColor DarkGray
Write-Host ""
Write-Host "  Press any key to close..." -ForegroundColor DarkGray
try { $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown") } catch { Start-Sleep 5 }
