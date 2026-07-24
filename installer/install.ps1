<#
.SYNOPSIS
    Mass Agent - Zero-Prompt Auto Installer
    Reads all config from .temp/.env in the repo. No questions asked.
    Self-elevates to admin automatically.

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

# ── Self-elevate ──────────────────────────────────────────────────────────────
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $tmp = "$env:TEMP\ms_install.ps1"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    (New-Object System.Net.WebClient).DownloadFile(
        "https://raw.githubusercontent.com/ketw/sshra/master/installer/install.ps1", $tmp)
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$tmp`""
    exit 0
}

# ── Paths ─────────────────────────────────────────────────────────────────────
$InstallDir  = "C:\Program Files\Mass"
$DataDir     = "$env:ProgramData\Mass"
$SshDir      = "$env:ProgramData\ssh"
$RegKey      = "HKLM:\SOFTWARE\Mass"
$ServiceName = "MassAgent"
$TempUrl     = "https://raw.githubusercontent.com/ketw/sshra/master/.temp"
$AgentExe    = "$InstallDir\msagent.exe"
$UpdateExe   = "$InstallDir\msupdate.exe"

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path $DataDir    | Out-Null
New-Item -ItemType Directory -Force -Path $SshDir     | Out-Null

# ── Logging ───────────────────────────────────────────────────────────────────
$LogFile = "$DataDir\install.log"
function Log  { param($m) "[$(Get-Date -f 'yyyy-MM-dd HH:mm:ss')] $m" | Add-Content $LogFile -ErrorAction SilentlyContinue }
function Step { param($m) Write-Host "  [..] $m" -ForegroundColor Cyan;   Log "STEP $m" }
function OK   { param($m) Write-Host "  [OK] $m" -ForegroundColor Green;  Log "OK   $m" }
function Warn { param($m) Write-Host "  [!!] $m" -ForegroundColor Yellow; Log "WARN $m" }

Clear-Host
Write-Host ""
Write-Host "  ╔══════════════════════════════════════════════╗" -ForegroundColor Magenta
Write-Host "  ║       Mass Agent - Auto Installer            ║" -ForegroundColor Magenta
Write-Host "  ╚══════════════════════════════════════════════╝" -ForegroundColor Magenta
Write-Host ""
Log "=== Install started on $env:COMPUTERNAME ==="

# ── Step 1: Fetch .env from repo ──────────────────────────────────────────────
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

# ── Step 2: Device identity (no prompt — use computer name) ───────────────────
$Label    = $env:COMPUTERNAME
$MachGuid = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Cryptography").MachineGuid
$DeviceId = ($MachGuid -replace '-','').Substring(0, [Math]::Min(32,($MachGuid -replace '-','').Length))
OK "Device: $Label ($DeviceId)"

# ── Step 3: Install OpenSSH ───────────────────────────────────────────────────
Step "Checking OpenSSH Server..."
$sshdOk = $false
try {
    $cap = Get-WindowsCapability -Online -Name "OpenSSH.Server*" -ErrorAction Stop
    if ($cap.State -eq 'Installed') { OK "OpenSSH already installed"; $sshdOk = $true }
    else {
        Step "Installing OpenSSH (Windows feature)..."
        Add-WindowsCapability -Online -Name $cap.Name -ErrorAction Stop | Out-Null
        OK "OpenSSH installed"; $sshdOk = $true
    }
} catch { Warn "Feature method failed, trying GitHub download..." }

if (-not $sshdOk) {
    $zip = "$env:TEMP\OpenSSH.zip"
    $wc.DownloadFile("https://github.com/PowerShell/Win32-OpenSSH/releases/latest/download/OpenSSH-Win64.zip", $zip)
    Expand-Archive $zip "$env:TEMP\OpenSSH-Win64" -Force
    $dest = "C:\Program Files\OpenSSH"
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    Move-Item "$env:TEMP\OpenSSH-Win64\OpenSSH-Win64" $dest -Force
    Push-Location $dest
    powershell -ExecutionPolicy Bypass -File ".\install-sshd.ps1" | Out-Null
    Pop-Location
    OK "OpenSSH installed from GitHub"
}

Set-Service sshd -StartupType Automatic -ErrorAction SilentlyContinue
Start-Service sshd -ErrorAction SilentlyContinue
$shPath = (Get-Command pwsh -ErrorAction SilentlyContinue)
if (-not $shPath) { $shPath = Get-Command powershell }
if (-not (Test-Path "HKLM:\SOFTWARE\OpenSSH")) { New-Item "HKLM:\SOFTWARE\OpenSSH" -Force | Out-Null }
New-ItemProperty "HKLM:\SOFTWARE\OpenSSH" -Name DefaultShell -Value $shPath.Source -Force | Out-Null
OK "sshd running, default shell: $($shPath.Source)"

# ── Step 4: SSH keypair ───────────────────────────────────────────────────────
Step "Generating SSH keypair..."
$KeyDir  = "$DataDir\owner_key"
$PrivKey = "$KeyDir\id_ed25519"
$PubKey  = "$KeyDir\id_ed25519.pub"
New-Item -ItemType Directory -Force -Path $KeyDir | Out-Null
$sshkg = @("C:\Windows\System32\OpenSSH\ssh-keygen.exe",
           "C:\Program Files\OpenSSH\ssh-keygen.exe") |
         Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $sshkg) { $sshkg = (Get-Command ssh-keygen -ErrorAction SilentlyContinue).Source }
if (-not $sshkg) { throw "ssh-keygen not found" }
if (-not (Test-Path $PrivKey)) {
    & $sshkg -t ed25519 -f $PrivKey -N '""' -C "ms-owner" 2>$null
    OK "Keypair generated"
} else { OK "Keypair already exists" }
$OwnerPubKey = (Get-Content $PubKey -Raw).Trim()

# ── Step 5: Hardened sshd_config + authorized_keys ───────────────────────────
Step "Writing hardened sshd_config..."
$progFwd = $env:ProgramData.Replace('\','/')
Set-Content "$SshDir\sshd_config" @"
Port 22
AuthorizedKeysFile          $progFwd/ssh/administrators_authorized_keys
PubkeyAuthentication        yes
PasswordAuthentication      no
PermitEmptyPasswords        no
ChallengeResponseAuthentication no
KbdInteractiveAuthentication    no
GSSAPIAuthentication            no
HostbasedAuthentication         no
MaxAuthTries                    3
LoginGraceTime                  20
StrictModes                     yes
AllowTcpForwarding              yes
AllowGroups                     MassUsers Administrators
Subsystem sftp                  sftp-server.exe
"@ -Encoding UTF8
OK "sshd_config written"

$authKeys = "$SshDir\administrators_authorized_keys"
Set-Content $authKeys $OwnerPubKey -Encoding UTF8
try {
    $acl = New-Object System.Security.AccessControl.FileSecurity
    $acl.SetAccessRuleProtection($true,$false)
    $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule("SYSTEM","FullControl","Allow")))
    $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule("Administrators","FullControl","Allow")))
    Set-Acl $authKeys $acl
} catch { icacls $authKeys /inheritance:r /grant "SYSTEM:F" /grant "Administrators:F" 2>$null | Out-Null }
OK "authorized_keys locked (owner only)"

if (-not (Get-LocalGroup MassUsers -ErrorAction SilentlyContinue)) {
    New-LocalGroup MassUsers -Description "Mass SSH access" | Out-Null
}
Add-LocalGroupMember -Group MassUsers -Member Administrators -ErrorAction SilentlyContinue
Restart-Service sshd -Force -ErrorAction SilentlyContinue
OK "sshd restarted with hardened config"

# ── Step 6: Download binaries from .temp/ ────────────────────────────────────
Step "Downloading binaries from .temp/..."

# Helper: download a base64-encoded file and decode to binary
function Get-BinaryFromB64 {
    param([string]$Url, [string]$Dest)
    $fname = [System.IO.Path]::GetFileName($Dest)
    for ($i = 1; $i -le 3; $i++) {
        try {
            $wc2 = New-Object System.Net.WebClient
            $wc2.Headers.Add("User-Agent","ms-installer/1.0")
            $b64 = $wc2.DownloadString($Url).Trim()
            $bytes = [Convert]::FromBase64String($b64)
            [IO.File]::WriteAllBytes($Dest, $bytes)
            if ((Test-Path $Dest) -and (Get-Item $Dest).Length -gt 10000) {
                OK "Downloaded $fname ($([math]::Round((Get-Item $Dest).Length/1KB))KB)"
                return $true
            }
        } catch { Warn "Attempt $i for ${fname}: $_" }
        Start-Sleep 3
    }
    return $false
}

foreach ($item in @(
    [PSCustomObject]@{ url="$TempBase/msagent.exe.b64";  dest=$AgentExe  },
    [PSCustomObject]@{ url="$TempBase/msupdate.exe.b64"; dest=$UpdateExe }
)) {
    if (-not (Get-BinaryFromB64 $item.url $item.dest)) {
        throw "Could not download $([System.IO.Path]::GetFileName($item.dest))"
    }
}

# ── Step 7: Registry config ───────────────────────────────────────────────────
Step "Writing registry config..."
if (-not (Test-Path $RegKey)) { New-Item $RegKey -Force | Out-Null }
Set-ItemProperty $RegKey RelayHost   $RelayHost  -Type String
Set-ItemProperty $RegKey RelayPort   $RelayPort  -Type String
Set-ItemProperty $RegKey AuthToken   $Token      -Type String
Set-ItemProperty $RegKey DeviceLabel $Label      -Type String
Set-ItemProperty $RegKey DeviceID    $DeviceId   -Type String
Set-ItemProperty $RegKey RepoRaw     "https://raw.githubusercontent.com/ketw/sshra/master" -Type String
Set-ItemProperty $RegKey Version     "1.0.1"     -Type String
OK "Registry written"

# ── Step 8: Install MassAgent service ────────────────────────────────────────
Step "Installing MassAgent service..."
$old = Get-Service $ServiceName -ErrorAction SilentlyContinue
if ($old) {
    Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue; Start-Sleep 2
    & $AgentExe --uninstall 2>$null; Start-Sleep 1
}
& $AgentExe --install; Start-Sleep 2
$svc = Get-Service $ServiceName -ErrorAction SilentlyContinue
if (-not ($svc -and $svc.Status -eq 'Running')) {
    Start-Service $ServiceName -ErrorAction SilentlyContinue; Start-Sleep 2
}
sc.exe config $ServiceName start= auto | Out-Null
$svc = Get-Service $ServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq 'Running') { OK "MassAgent running (auto-start)" }
else { Warn "MassAgent installed but not running — check $DataDir\agent.log" }

# ── Step 9: Install MassUpdater service ──────────────────────────────────────
Step "Installing MassUpdater service..."
$oldU = Get-Service MassUpdater -ErrorAction SilentlyContinue
if ($oldU) {
    Stop-Service MassUpdater -Force -ErrorAction SilentlyContinue; Start-Sleep 1
    & $UpdateExe --uninstall 2>$null; Start-Sleep 1
}
& $UpdateExe --install; Start-Sleep 2
$svcU = Get-Service MassUpdater -ErrorAction SilentlyContinue
if ($svcU -and $svcU.Status -eq 'Running') { OK "MassUpdater running (polls every 5 min)" }
else { Warn "MassUpdater installed but not running" }

# ── Step 10: Firewall ─────────────────────────────────────────────────────────
Step "Configuring firewall..."
Remove-NetFirewallRule -DisplayName "Mass*" -ErrorAction SilentlyContinue
New-NetFirewallRule -DisplayName "Mass SSH In"    -Direction Inbound  -Protocol TCP -LocalPort 22        -Action Allow | Out-Null
New-NetFirewallRule -DisplayName "Mass Relay Out" -Direction Outbound -Protocol TCP -RemotePort 443,7744 -Action Allow | Out-Null
OK "Firewall rules set"

# ── Done ──────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "  ╔══════════════════════════════════════════════════╗" -ForegroundColor Green
Write-Host "  ║          Installation Complete!                  ║" -ForegroundColor Green
Write-Host "  ╚══════════════════════════════════════════════════╝" -ForegroundColor Green
Write-Host ""
Write-Host "  Device  : $Label ($DeviceId)" -ForegroundColor Cyan
Write-Host "  Relay   : ${RelayHost}:${RelayPort}" -ForegroundColor Cyan
Write-Host "  SSH     : Public-key only, locked to owner key"  -ForegroundColor Green
Write-Host "  Updates : Auto every 5 min (MassUpdater service)" -ForegroundColor Green
Write-Host ""
Write-Host "  ┌──────────────────────────────────────────────────┐" -ForegroundColor Yellow
Write-Host "  │  Copy this private key to your laptop once:      │" -ForegroundColor Yellow
Write-Host "  │  Save as: ~/.ms/keys/id_$Label" -ForegroundColor Yellow
Write-Host "  └──────────────────────────────────────────────────┘" -ForegroundColor Yellow
Write-Host ""

$ans = Read-Host "  Show private key now for copy-paste? [Y/N]"
if ($ans -match '^[Yy]') {
    Write-Host ""
    Write-Host "  ===== PRIVATE KEY — copy everything below =====" -ForegroundColor Yellow
    Get-Content $PrivKey
    Write-Host "  ===== END PRIVATE KEY =====" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  On your laptop:" -ForegroundColor Cyan
    Write-Host "    1. Save above as: ~/.ms/keys/id_$Label" -ForegroundColor White
    Write-Host "    2. Run: ms   (auto-discovers devices, no config needed)" -ForegroundColor White
}

Write-Host ""
Write-Host "  Log: $LogFile" -ForegroundColor DarkGray
Log "=== Install complete ==="
Write-Host ""
Write-Host "  Press any key to close..." -ForegroundColor DarkGray
try { $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown") } catch { Start-Sleep 5 }
