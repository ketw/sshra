<#
.SYNOPSIS
    Mass (ms) - Laptop Setup
    Run once on your laptop. After this, just type 'ms' in any terminal.

.EXAMPLE
    irm https://raw.githubusercontent.com/ketw/sshra/master/setup-laptop.ps1 | iex
#>

$ErrorActionPreference = "Stop"

trap {
    Write-Host ""
    Write-Host "  ERROR: $_" -ForegroundColor Red
    Write-Host ""
    Write-Host "  Press any key to close..." -ForegroundColor DarkGray
    try { $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown") } catch { Start-Sleep 15 }
    exit 1
}

# Self-elevate
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $tmp = "$env:TEMP\ms_setup_laptop.ps1"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest "https://raw.githubusercontent.com/ketw/sshra/master/setup-laptop.ps1" -OutFile $tmp -UseBasicParsing
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$tmp`""
    exit 0
}

# â”€â”€ Paths â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
$MsDir      = "C:\Program Files\Mass"          # install dir, added to PATH
$MsConfig   = "$env:USERPROFILE\.ms"           # user config dir
$MsKeys     = "$MsConfig\keys"                 # private keys per device
$MsCfgFile  = "$MsConfig\config.json"          # relay config
$MgrExe     = "$MsDir\msmgr.exe"              # the manager binary
$MgrUrl         = "https://raw.githubusercontent.com/ketw/sshra/master/.temp/msmgr.exe"
$MgrUrlFallback = "https://raw.githubusercontent.com/ketw/sshra/master/.temp/msmgr.exe"
$TempUrl        = "https://raw.githubusercontent.com/ketw/sshra/master/.temp"

function Write-Step { param($m) Write-Host "  [..] $m" -ForegroundColor Cyan }
function Write-OK   { param($m) Write-Host "  [OK] $m" -ForegroundColor Green }
function Write-Warn { param($m) Write-Host "  [!!] $m" -ForegroundColor Yellow }

Clear-Host
Write-Host ""
Write-Host "  â•”â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•—" -ForegroundColor Magenta
Write-Host "  â•‘        Mass (ms) - Laptop Setup              â•‘" -ForegroundColor Magenta
Write-Host "  â•‘        After this, just type: ms             â•‘" -ForegroundColor Magenta
Write-Host "  â•šâ•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•" -ForegroundColor Magenta
Write-Host ""

# â”€â”€ Step 1: Directories â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
Write-Step "Creating directories..."
New-Item -ItemType Directory -Force -Path $MsDir    | Out-Null
New-Item -ItemType Directory -Force -Path $MsConfig | Out-Null
New-Item -ItemType Directory -Force -Path $MsKeys   | Out-Null
Write-OK "Directories ready"

# â”€â”€ Step 2: Download msmgr.exe â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
Write-Step "Downloading msmgr.exe..."
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$downloaded = $false

foreach ($url in @($MgrUrl, $MgrUrlFallback)) {
    try {
        $wc = New-Object System.Net.WebClient
        $wc.Headers.Add("User-Agent", "ms-setup/1.0")
        $wc.DownloadFile($url, $MgrExe)
        if ((Test-Path $MgrExe) -and (Get-Item $MgrExe).Length -gt 10000) {
            Write-OK "Downloaded msmgr.exe"
            $downloaded = $true; break
        }
    } catch { Write-Warn "Download failed from ${url}: $_" }
}

if (-not $downloaded) {
    # Try local build
    $localBuild = "p:\Projects\ssh-access\build\msmgr.exe"
    $localBuild2 = "p:\Projects\ssh-access\build\msmgr.exe"
    foreach ($lb in @($localBuild2, $localBuild)) {
        if (Test-Path $lb) {
            Copy-Item $lb $MgrExe -Force
            Write-OK "Copied from local build: $lb"
            $downloaded = $true; break
        }
    }
}

if (-not $downloaded) {
    throw "Could not obtain msmgr.exe. Build it first with: build\build-manager.bat"
}

# â”€â”€ Step 3: Add to system PATH â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
Write-Step "Adding 'ms' to system PATH..."
$syspath = [Environment]::GetEnvironmentVariable("PATH", "Machine")
if ($syspath -notlike "*$MsDir*") {
    [Environment]::SetEnvironmentVariable("PATH", "$syspath;$MsDir", "Machine")
    $env:PATH = "$env:PATH;$MsDir"
    Write-OK "Added to system PATH"
} else {
    Write-OK "Already in PATH"
}

# â”€â”€ Step 4: Create 'ms' command wrapper â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
Write-Step "Creating 'ms' command..."
Set-Content -Path "$MsDir\ms.cmd" -Value "@echo off`r`n`"$MgrExe`" %*" -Encoding ASCII
Write-OK "Created ms.cmd â€” type 'ms' in any new terminal"

# â”€â”€ Step 5: Load relay config from repo .temp/.env (no prompt) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
Write-Step "Loading relay config from repo..."
$relayHost  = ""
$relayPort  = "443"
$relayToken = ""

# First check if local config already exists and is complete
if (Test-Path $MsCfgFile) {
    try {
        $existing = Get-Content $MsCfgFile -Raw | ConvertFrom-Json
        if ($existing.relay_host -and $existing.relay_token) {
            $relayHost  = $existing.relay_host
            $relayPort  = if ($existing.relay_port) { $existing.relay_port } else { "443" }
            $relayToken = $existing.relay_token
            Write-OK "Loaded from existing config: ${relayHost}:${relayPort}"
        }
    } catch {}
}

# Fetch from .temp/.env if not already set
if (-not $relayHost -or -not $relayToken) {
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $envRaw = (New-Object System.Net.WebClient).DownloadString("$TempUrl/.env")
        foreach ($line in ($envRaw -split "`n")) {
            $line = $line.Trim()
            if ($line -match '^\s*#' -or $line -eq '') { continue }
            if ($line -match '^([^=]+)=(.*)$') {
                $k = $Matches[1].Trim(); $v = $Matches[2].Trim()
                if ($k -eq 'RELAY_HOST')  { $relayHost  = $v }
                if ($k -eq 'RELAY_PORT')  { $relayPort  = $v }
                if ($k -eq 'RELAY_TOKEN') { $relayToken = $v }
            }
        }
        Write-OK "Config loaded from repo: ${relayHost}:${relayPort}"
    } catch {
        Write-Warn "Could not fetch .env from repo: $_"
        # Fall back to prompt only if still empty
        if (-not $relayHost) {
            $relayHost = (Read-Host "  Relay host (e.g. ra-u9qf.onrender.com)").Trim()
            $relayHost = $relayHost -replace '^https?://', '' -replace '/$', ''
        }
        if (-not $relayToken) { $relayToken = (Read-Host "  Relay auth token").Trim() }
    }
}

# â”€â”€ Step 6: Import device private keys â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
Write-Host ""
Write-Host "  â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€" -ForegroundColor DarkGray
Write-Host "  Import private keys from your remote devices" -ForegroundColor White
Write-Host "  Each device generated a key during msagent install." -ForegroundColor DarkGray
Write-Host "  Paste the private key contents when prompted." -ForegroundColor DarkGray
Write-Host "  Press Enter with no device name when done." -ForegroundColor DarkGray
Write-Host "  â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€" -ForegroundColor DarkGray
Write-Host ""

$importedKeys = @()

while ($true) {
    $deviceName = (Read-Host "  Device name (e.g. 'Gaming PC') or Enter to finish").Trim()
    if (-not $deviceName) { break }

    $safeName = $deviceName -replace '[^a-zA-Z0-9_-]', '_'
    $keyPath  = "$MsKeys\id_$safeName"

    # Skip if already imported this session
    if ($importedKeys | Where-Object { $_.SafeName -eq $safeName }) {
        Write-Warn "'$deviceName' already imported this session â€” skipping."
        continue
    }

    Write-Host ""
    Write-Host "  Paste the PRIVATE KEY for '$deviceName'." -ForegroundColor Yellow
    Write-Host "  Include the -----BEGIN----- and -----END----- lines." -ForegroundColor DarkGray
    Write-Host "  Type a single dot '.' on its own line when done." -ForegroundColor DarkGray
    Write-Host ""

    $lines = @()
    while ($true) {
        $line = Read-Host "  "
        if ($line -eq ".") { break }
        $lines += $line
    }
    $keyContent = ($lines -join "`n").Trim()

    if ($keyContent -match "BEGIN OPENSSH PRIVATE KEY|BEGIN RSA PRIVATE KEY|BEGIN EC PRIVATE KEY") {
        [System.IO.File]::WriteAllText($keyPath, $keyContent + "`n")

        # Lock permissions â€” SSH refuses world-readable keys
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
        Write-Warn "Doesn't look like a valid private key â€” skipped. Try again."
    }
    Write-Host ""
}

# â”€â”€ Step 7: Write config.json â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
Write-Step "Writing config..."

# Merge with existing keys
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

# â”€â”€ Done â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
Write-Host ""
Write-Host "  â•”â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•—" -ForegroundColor Green
Write-Host "  â•‘             Laptop Setup Complete!                   â•‘" -ForegroundColor Green
Write-Host "  â•šâ•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•" -ForegroundColor Green
Write-Host ""
Write-Host "  Install dir : $MsDir"     -ForegroundColor Cyan
Write-Host "  Config      : $MsCfgFile" -ForegroundColor Cyan
Write-Host "  Keys dir    : $MsKeys"    -ForegroundColor Cyan
Write-Host "  Relay       : ${relayHost}:${relayPort}" -ForegroundColor Cyan
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
Write-Host "  To add more device keys later, run setup again or:" -ForegroundColor DarkGray
Write-Host "      ms --add-key" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  Press any key to close..." -ForegroundColor DarkGray
try { $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown") } catch { Start-Sleep 5 }
