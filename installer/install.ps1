<#
.SYNOPSIS
    Mass Agent - Zero-Prompt Auto Installer
    Reads config from .temp/.env in the repo. No questions. Self-elevates.
.EXAMPLE
    irm https://raw.githubusercontent.com/ketw/sshra/master/installer/install.ps1 | iex
#>

$ErrorActionPreference = "Stop"

trap {
    $msg = $_.ToString()
    Write-Host ""
    Write-Host "  ERROR: $msg" -ForegroundColor Red
    Add-Content -Path "$env:ProgramData\Mass\install.log" -Value "[$(Get-Date -f 'yyyy-MM-dd HH:mm:ss')][ERROR] $msg" -ErrorAction SilentlyContinue
    Write-Host ""
    Write-Host "  Press any key to close..." -ForegroundColor DarkGray
    try { $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown") } catch { Start-Sleep 15 }
    exit 1
}

# Self-elevate
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $tmp = "$env:TEMP\ms_install.ps1"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    (New-Object System.Net.WebClient).DownloadFile(
        "https://raw.githubusercontent.com/ketw/sshra/master/installer/install.ps1", $tmp)
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$tmp`""
    exit 0
}

# Paths
$InstallDir  = "C:\Program Files\Mass"
$DataDir     = "$env:ProgramData\Mass"
$SshDir      = "$env:ProgramData\ssh"
$RegKey      = "HKLM:\SOFTWARE\Mass"
$ServiceName = "MassAgent"
$TempUrl     = "https://raw.githubusercontent.com/ketw/sshra/master/.temp"
$AgentExe    = "$InstallDir\msagent.exe"
$UpdateExe   = "$InstallDir\msupdate.exe"
$LogFile     = "$DataDir\install.log"

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path $DataDir    | Out-Null
New-Item -ItemType Directory -Force -Path $SshDir     | Out-Null

function Log  { param($m) "[$(Get-Date -f 'yyyy-MM-dd HH:mm:ss')] $m" | Add-Content $LogFile -ErrorAction SilentlyContinue }
function Step { param($m) Write-Host "  [..] $m" -ForegroundColor Cyan;   Log "STEP $m" }
function OK   { param($m) Write-Host "  [OK] $m" -ForegroundColor Green;  Log "OK   $m" }
function Warn { param($m) Write-Host "  [!!] $m" -ForegroundColor Yellow; Log "WARN $m" }

Clear-Host
Write-Host ""
Write-Host "  +----------------------------------------------+" -ForegroundColor Magenta
Write-Host "  |       Mass Agent - Auto Installer            |" -ForegroundColor Magenta
Write-Host "  +----------------------------------------------+" -ForegroundColor Magenta
Write-Host ""
Log "=== Install started on $env:COMPUTERNAME ==="

# Step 1: Fetch .env config
Step "Fetching config from repo..."
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$wc = New-Object System.Net.WebClient
$wc.Headers.Add("User-Agent", "ms-installer/1.0")
$envRaw = $wc.DownloadString("$TempUrl/.env")

$cfg = @{}
foreach ($line in ($envRaw -split "`n")) {
    $line = $line.Trim()
    if ($line -match '^\s*#' -or $line -eq '') { continue }
    if ($line -match '^([^=]+)=(.*)$') { $cfg[$Matches[1].Trim()] = $Matches[2].Trim() }
}

$RelayHost = $cfg['RELAY_HOST']
$RelayPort = if ($cfg['RELAY_PORT']) { $cfg['RELAY_PORT'] } else { '443' }
$Token     = $cfg['RELAY_TOKEN']
$TempBase  = if ($cfg['TEMP_URL'])   { $cfg['TEMP_URL']   } else { $TempUrl }
OK "Config: relay=$RelayHost port=$RelayPort"

# Step 2: Device identity
$Label    = $env:COMPUTERNAME
$MachGuid = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Cryptography").MachineGuid
$DeviceId = ($MachGuid -replace '-','').Substring(0, [Math]::Min(32, ($MachGuid -replace '-','').Length))
OK "Device: $Label ($DeviceId)"

# Step 3: Install OpenSSH (GitHub first - fastest and most reliable)
Step "Checking OpenSSH Server..."
$sshdOk = $false

# Already installed?
$existingCap = Get-WindowsCapability -Online -Name "OpenSSH.Server*" -ErrorAction SilentlyContinue
if ($existingCap -and $existingCap.State -eq 'Installed') {
    OK "OpenSSH already installed"
    $sshdOk = $true
}

# Method 1: GitHub download (always works, no Windows Update needed)
if (-not $sshdOk) {
    Step "Downloading OpenSSH from GitHub..."
    try {
        $zip  = "$env:TEMP\OpenSSH.zip"
        $extr = "$env:TEMP\OpenSSH-Win64"
        $dest = "C:\Program Files\OpenSSH"
        if (Test-Path $extr) { Remove-Item $extr -Recurse -Force -ErrorAction SilentlyContinue }
        $wc.DownloadFile("https://github.com/PowerShell/Win32-OpenSSH/releases/latest/download/OpenSSH-Win64.zip", $zip)
        Expand-Archive -Path $zip -DestinationPath $env:TEMP -Force
        if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
        if (Test-Path "$extr\OpenSSH-Win64") {
            Move-Item "$extr\OpenSSH-Win64" $dest -Force
        } else {
            Move-Item $extr $dest -Force
        }
        Push-Location $dest
        powershell -ExecutionPolicy Bypass -File ".\install-sshd.ps1" | Out-Null
        Pop-Location
        OK "OpenSSH installed from GitHub"
        $sshdOk = $true
    } catch { Warn "GitHub download failed: $_" }
}

# Method 2: Windows optional feature (slow but works on newer Windows)
if (-not $sshdOk) {
    Step "Trying Windows feature install..."
    try {
        $cap = Get-WindowsCapability -Online -Name "OpenSSH.Server*" -ErrorAction Stop
        Add-WindowsCapability -Online -Name $cap.Name -ErrorAction Stop | Out-Null
        OK "OpenSSH installed via Windows feature"
        $sshdOk = $true
    } catch { Warn "Windows feature install failed: $_" }
}

# Method 3: winget
if (-not $sshdOk) {
    try {
        $wg = Get-Command winget -ErrorAction Stop
        & $wg.Source install --id Microsoft.OpenSSH.Beta -e --silent --accept-package-agreements --accept-source-agreements 2>$null
        Start-Sleep 3
        if (Get-Service sshd -ErrorAction SilentlyContinue) {
            OK "OpenSSH installed via winget"
            $sshdOk = $true
        }
    } catch {}
}

if (-not $sshdOk) { throw "Could not install OpenSSH by any method" }

Set-Service sshd -StartupType Automatic -ErrorAction SilentlyContinue
Start-Service sshd -ErrorAction SilentlyContinue
$shCmd = Get-Command pwsh -ErrorAction SilentlyContinue
if (-not $shCmd) { $shCmd = Get-Command powershell -ErrorAction SilentlyContinue }
if (-not (Test-Path "HKLM:\SOFTWARE\OpenSSH")) { New-Item "HKLM:\SOFTWARE\OpenSSH" -Force | Out-Null }
New-ItemProperty "HKLM:\SOFTWARE\OpenSSH" -Name DefaultShell -Value $shCmd.Source -PropertyType String -Force | Out-Null
OK "sshd running, shell: $($shCmd.Source)"

# Step 4: SSH keypair
Step "Generating SSH keypair..."
$KeyDir  = "$DataDir\owner_key"
$PrivKey = "$KeyDir\id_ed25519"
$PubKey  = "$KeyDir\id_ed25519.pub"
New-Item -ItemType Directory -Force -Path $KeyDir | Out-Null
$sshkg = @(
    "C:\Windows\System32\OpenSSH\ssh-keygen.exe",
    "C:\Program Files\OpenSSH\ssh-keygen.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $sshkg) { $sshkg = (Get-Command ssh-keygen -ErrorAction SilentlyContinue).Source }
if (-not $sshkg) { throw "ssh-keygen not found after OpenSSH install" }
if (-not (Test-Path $PrivKey)) {
    & $sshkg -t ed25519 -f $PrivKey -N '""' -C "ms-owner" 2>$null
    OK "Keypair generated"
} else { OK "Keypair already exists" }
$OwnerPubKey = (Get-Content $PubKey -Raw).Trim()

# Step 5: Hardened sshd_config + authorized_keys
Step "Writing hardened sshd_config..."
$progFwd = $env:ProgramData.Replace('\', '/')
$sshdCfg = "Port 22`n"
$sshdCfg += "AuthorizedKeysFile          $progFwd/ssh/administrators_authorized_keys`n"
$sshdCfg += "PubkeyAuthentication        yes`n"
$sshdCfg += "PasswordAuthentication      no`n"
$sshdCfg += "PermitEmptyPasswords        no`n"
$sshdCfg += "ChallengeResponseAuthentication no`n"
$sshdCfg += "KbdInteractiveAuthentication    no`n"
$sshdCfg += "GSSAPIAuthentication            no`n"
$sshdCfg += "HostbasedAuthentication         no`n"
$sshdCfg += "MaxAuthTries                    3`n"
$sshdCfg += "LoginGraceTime                  20`n"
$sshdCfg += "StrictModes                     yes`n"
$sshdCfg += "AllowTcpForwarding              yes`n"
$sshdCfg += "AllowGroups                     MassUsers Administrators`n"
$sshdCfg += "Subsystem sftp                  sftp-server.exe`n"
Set-Content "$SshDir\sshd_config" $sshdCfg -Encoding UTF8
OK "sshd_config written"

$authKeys = "$SshDir\administrators_authorized_keys"
Set-Content $authKeys $OwnerPubKey -Encoding UTF8
try {
    $acl = New-Object System.Security.AccessControl.FileSecurity
    $acl.SetAccessRuleProtection($true, $false)
    $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule("SYSTEM", "FullControl", "Allow")))
    $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule("Administrators", "FullControl", "Allow")))
    Set-Acl $authKeys $acl
} catch {
    icacls $authKeys /inheritance:r /grant "SYSTEM:F" /grant "Administrators:F" 2>$null | Out-Null
}
OK "authorized_keys locked (owner only)"

if (-not (Get-LocalGroup MassUsers -ErrorAction SilentlyContinue)) {
    New-LocalGroup MassUsers -Description "Mass SSH access" | Out-Null
}
Add-LocalGroupMember -Group MassUsers -Member Administrators -ErrorAction SilentlyContinue
Restart-Service sshd -Force -ErrorAction SilentlyContinue
OK "sshd restarted with hardened config"

# Step 6: Stop existing services BEFORE downloading (prevents file-locked errors)
Step "Stopping existing Mass services..."
foreach ($svcName in @('MassAgent', 'MassUpdater')) {
    $s = Get-Service $svcName -ErrorAction SilentlyContinue
    if ($s) {
        Stop-Service $svcName -Force -ErrorAction SilentlyContinue
        Start-Sleep 2
        $exePath = if ($svcName -eq 'MassAgent') { $AgentExe } else { $UpdateExe }
        if (Test-Path $exePath) {
            try { & $exePath --uninstall 2>$null } catch {}
            Start-Sleep 1
        }
    }
}
OK "Services stopped (files unlocked)"

# Step 7: Download binaries from .temp/ as base64, decode to binary
Step "Downloading binaries..."

function Get-BinaryFromB64 {
    param([string]$Url, [string]$Dest)
    $fname = [System.IO.Path]::GetFileName($Dest)
    for ($i = 1; $i -le 3; $i++) {
        try {
            $wc2 = New-Object System.Net.WebClient
            $wc2.Headers.Add("User-Agent", "ms-installer/1.0")
            $b64   = $wc2.DownloadString($Url).Trim()
            $bytes = [Convert]::FromBase64String($b64)
            [IO.File]::WriteAllBytes($Dest, $bytes)
            if ((Test-Path $Dest) -and (Get-Item $Dest).Length -gt 10000) {
                OK "Downloaded $fname ($([math]::Round((Get-Item $Dest).Length / 1KB))KB)"
                return $true
            }
        } catch { Warn "Attempt $i for ${fname}: $_" }
        Start-Sleep 3
    }
    return $false
}

$downloads = @(
    @{ url = "$TempBase/msagent.exe.b64";  dest = $AgentExe  },
    @{ url = "$TempBase/msupdate.exe.b64"; dest = $UpdateExe }
)
foreach ($d in $downloads) {
    $ok = Get-BinaryFromB64 $d.url $d.dest
    if (-not $ok) { throw "Could not download $([System.IO.Path]::GetFileName($d.dest))" }
}

# Step 8: Registry config
Step "Writing registry config..."
if (-not (Test-Path $RegKey)) { New-Item $RegKey -Force | Out-Null }
Set-ItemProperty $RegKey -Name RelayHost   -Value $RelayHost  -Type String
Set-ItemProperty $RegKey -Name RelayPort   -Value $RelayPort  -Type String
Set-ItemProperty $RegKey -Name AuthToken   -Value $Token      -Type String
Set-ItemProperty $RegKey -Name DeviceLabel -Value $Label      -Type String
Set-ItemProperty $RegKey -Name DeviceID    -Value $DeviceId   -Type String
Set-ItemProperty $RegKey -Name RepoRaw     -Value "https://raw.githubusercontent.com/ketw/sshra/master" -Type String
Set-ItemProperty $RegKey -Name Version     -Value "1.0.1"     -Type String
OK "Registry written"

# Step 9: Install MassAgent service
Step "Installing MassAgent service..."
try { & $AgentExe --install } catch {}
Start-Sleep 2
$svc = Get-Service $ServiceName -ErrorAction SilentlyContinue
if (-not ($svc -and $svc.Status -eq 'Running')) {
    Start-Service $ServiceName -ErrorAction SilentlyContinue
    Start-Sleep 2
}
sc.exe config $ServiceName start= auto | Out-Null
$svc = Get-Service $ServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq 'Running') { OK "MassAgent running (auto-start pre-login)" }
else { Warn "MassAgent installed but not yet running - check $DataDir\agent.log" }

# Step 10: Install MassUpdater service
Step "Installing MassUpdater service..."
try { & $UpdateExe --install } catch {}
Start-Sleep 2
$svcU = Get-Service MassUpdater -ErrorAction SilentlyContinue
if ($svcU -and $svcU.Status -eq 'Running') { OK "MassUpdater running (polls for updates every 5 min)" }
else { Warn "MassUpdater installed but not yet running" }

# Step 11: Firewall
Step "Configuring firewall..."
Remove-NetFirewallRule -DisplayName "Mass*" -ErrorAction SilentlyContinue
New-NetFirewallRule -DisplayName "Mass SSH In"    -Direction Inbound  -Protocol TCP -LocalPort 22        -Action Allow | Out-Null
New-NetFirewallRule -DisplayName "Mass Relay Out" -Direction Outbound -Protocol TCP -RemotePort 443,7744 -Action Allow | Out-Null
OK "Firewall rules set"

# Done
Write-Host ""
Write-Host "  +--------------------------------------------------+" -ForegroundColor Green
Write-Host "  |          Installation Complete!                  |" -ForegroundColor Green
Write-Host "  +--------------------------------------------------+" -ForegroundColor Green
Write-Host ""
Write-Host "  Device  : $Label ($DeviceId)" -ForegroundColor Cyan
Write-Host "  Relay   : $RelayHost`:$RelayPort" -ForegroundColor Cyan
Write-Host "  SSH     : Public-key only, locked to owner key"  -ForegroundColor Green
Write-Host "  Updates : Auto every 5 min via MassUpdater service" -ForegroundColor Green
Write-Host ""
Write-Host "  Private key location: $PrivKey" -ForegroundColor Yellow
Write-Host "  Copy it to your laptop as: ~/.ms/keys/id_$Label" -ForegroundColor Yellow
Write-Host ""

$ans = Read-Host "  Show private key now for copy-paste? [Y/N]"
if ($ans -match '^[Yy]') {
    Write-Host ""
    Write-Host "  ===== BEGIN PRIVATE KEY =====" -ForegroundColor Yellow
    Get-Content $PrivKey
    Write-Host "  ===== END PRIVATE KEY =====" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  On your laptop:" -ForegroundColor Cyan
    Write-Host "    1. Save above key as: ~/.ms/keys/id_$Label" -ForegroundColor White
    Write-Host "    2. Run: ms   (auto-discovers all devices)" -ForegroundColor White
}

Write-Host ""
Write-Host "  Log: $LogFile" -ForegroundColor DarkGray
Log "=== Install complete ==="
Write-Host ""
Write-Host "  Press any key to close..." -ForegroundColor DarkGray
try { $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown") } catch { Start-Sleep 5 }
