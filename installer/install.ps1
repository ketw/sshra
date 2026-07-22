#Requires -Version 5.1
<#
.SYNOPSIS
    Mass - Fully Automatic Self-Contained Installer
    Supports PowerShell 5.1+. Self-elevates to admin automatically.
.EXAMPLE
    # One-liner from anywhere (even non-admin terminal):
    irm https://raw.githubusercontent.com/ketw/sshra/master/installer/install.ps1 | iex

    # Silent:
    .\install.ps1 -RelayHost "ra-u9qf.onrender.com" -RelayPort "10000" -Token "yourtoken" -Label "Gaming PC"

    # Uninstall:
    .\install.ps1 -Uninstall
#>
param(
    [string]$RelayHost = "",
    [string]$RelayPort = "10000",
    [string]$Token     = "",
    [string]$Label     = "",
    [switch]$Uninstall
)

# ── Self-elevate to Administrator if not already ──────────────────────────────
$currentPrincipal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "  Not running as Administrator. Re-launching elevated..." -ForegroundColor Yellow
    # Always download fresh copy to temp — works whether run via iex or as a file
    $tmpScript = "$env:TEMP\ms_install.ps1"
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest "https://raw.githubusercontent.com/ketw/sshra/master/installer/install.ps1" `
            -OutFile $tmpScript -UseBasicParsing -ErrorAction Stop
    } catch {
        # If download fails, copy self if we have a path
        if ($MyInvocation.MyCommand.Path) {
            Copy-Item $MyInvocation.MyCommand.Path $tmpScript -Force
        } else {
            Write-Host "  ERROR: Could not prepare installer for elevation." -ForegroundColor Red
            pause; exit 1
        }
    }

    # Build argument string — pass any params the user already gave
    $params = ""
    if ($RelayHost) { $params += " -RelayHost `"$RelayHost`"" }
    if ($RelayPort) { $params += " -RelayPort `"$RelayPort`"" }
    if ($Token)     { $params += " -Token `"$Token`"" }
    if ($Label)     { $params += " -Label `"$Label`"" }
    if ($Uninstall) { $params += " -Uninstall" }

    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$tmpScript`"$params"

    Start-Process powershell -Verb RunAs -ArgumentList $argList
    # Do NOT use -Wait here — just launch and exit this non-admin instance
    exit 0
}

$ErrorActionPreference = "Stop"

# Wrap everything in try/catch so the window never silently closes on error
trap {
    Write-Host ""
    Write-Host "  ================================================================" -ForegroundColor Red
    Write-Host "  ERROR: $_" -ForegroundColor Red
    Write-Host "  ================================================================" -ForegroundColor Red
    Write-Host ""
    Write-Host "  Press any key to close..." -ForegroundColor DarkGray
    try { $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown") } catch { Start-Sleep 30 }
    exit 1
}

# ── Paths & names ─────────────────────────────────────────────────────────────
$InstallDir  = "C:\Program Files\Mass"
$DataDir     = "C:\ProgramData\Mass"
$SshDir      = "C:\ProgramData\ssh"
$ServiceName = "MassAgent"
$RegKey      = "HKLM:\SOFTWARE\Mass"
$AccessGroup = "MassUsers"
$AgentExe    = "$InstallDir\msagent.exe"

# Where to download msagent.exe from (GitHub releases of your repo)
# Uses the specific tag URL so prereleases and latest both work
$AgentDownloadUrl = "https://github.com/ketw/sshra/releases/download/v1.0.0/msagent.exe"

# ── Output helpers ────────────────────────────────────────────────────────────
function Write-Step { param($m) Write-Host "  [..] $m" -ForegroundColor Cyan }
function Write-OK   { param($m) Write-Host "  [OK] $m" -ForegroundColor Green }
function Write-Warn { param($m) Write-Host "  [!!] $m" -ForegroundColor Yellow }
function Write-Fail { param($m) Write-Host "  [XX] $m" -ForegroundColor Red }

function Write-Banner {
    Clear-Host
    Write-Host ""
    Write-Host "  ╔══════════════════════════════════════════════╗" -ForegroundColor Magenta
    Write-Host "  ║        Mass - Auto Installer           ║" -ForegroundColor Magenta
    Write-Host "  ║   Sets up everything. No manual steps.       ║" -ForegroundColor Magenta
    Write-Host "  ╚══════════════════════════════════════════════╝" -ForegroundColor Magenta
    Write-Host ""
}

# ── Uninstall ─────────────────────────────────────────────────────────────────
if ($Uninstall) {
    Write-Host "  Uninstalling Mass..." -ForegroundColor Yellow
    Stop-Service  $ServiceName -Force -ErrorAction SilentlyContinue
    Start-Sleep 2
    if (Test-Path $AgentExe) { & $AgentExe --uninstall 2>$null }
    sc.exe delete $ServiceName 2>$null | Out-Null
    Remove-Item -Recurse -Force $InstallDir -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $DataDir    -ErrorAction SilentlyContinue
    if (Test-Path $RegKey) { Remove-Item -Recurse -Force $RegKey }
    Remove-NetFirewallRule -DisplayName "Mass*" -ErrorAction SilentlyContinue
    Remove-LocalGroup $AccessGroup -ErrorAction SilentlyContinue
    Write-Host "  Done. Mass removed." -ForegroundColor Green
    exit 0
}

Write-Banner

# ── Step 0: Gather the 3 required pieces of info ─────────────────────────────
Write-Host "  This installer will set up everything automatically." -ForegroundColor White
Write-Host "  It only needs 3 things from you:" -ForegroundColor White
Write-Host ""

if (-not $RelayHost) {
    $RelayHost = (Read-Host "  [1/3] Relay server host (e.g. ra-u9qf.onrender.com)").Trim()
}
if (-not $Token) {
    $Token = (Read-Host "  [2/3] Relay auth token").Trim()
}
if (-not $Label) {
    $suggested = $env:COMPUTERNAME
    $input = (Read-Host "  [3/3] Device label (press Enter for '$suggested')").Trim()
    $Label = if ($input) { $input } else { $suggested }
}

# Strip any https:// or http:// prefix — agent connects via raw TCP not HTTP
$RelayHost = $RelayHost -replace '^https?://', '' -replace '/$', ''

Write-Host ""
Write-Host "  Relay : ${RelayHost}:${RelayPort}" -ForegroundColor DarkGray
Write-Host "  Label : $Label"                    -ForegroundColor DarkGray
Write-Host ""

# ── Step 1: Create directories ────────────────────────────────────────────────
Write-Step "Creating directories..."
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path $DataDir    | Out-Null
New-Item -ItemType Directory -Force -Path $SshDir     | Out-Null
Write-OK "Directories ready"

# ── Step 2: Install OpenSSH Server (fully automatic) ─────────────────────────
Write-Step "Installing OpenSSH Server..."

$sshdInstalled = $false

# Method A: Windows optional feature (Windows 10 1809+ / Windows 11)
try {
    $cap = Get-WindowsCapability -Online -Name "OpenSSH.Server*" -ErrorAction Stop
    if ($cap.State -eq "Installed") {
        Write-OK "OpenSSH Server already installed (Windows feature)"
        $sshdInstalled = $true
    } else {
        Write-Step "Adding OpenSSH.Server Windows feature (downloading ~3MB)..."
        Add-WindowsCapability -Online -Name $cap.Name -ErrorAction Stop | Out-Null
        Write-OK "OpenSSH Server installed via Windows feature"
        $sshdInstalled = $true
    }
} catch {
    Write-Warn "Windows feature method failed, trying winget..."
}

# Method B: winget (fallback for older Windows)
if (-not $sshdInstalled) {
    try {
        $wg = Get-Command winget -ErrorAction Stop
        Write-Step "Installing OpenSSH via winget..."
        & winget install --id "Microsoft.OpenSSH.Beta" -e --silent --accept-package-agreements --accept-source-agreements 2>$null
        Start-Sleep 3
        if (Get-Service sshd -ErrorAction SilentlyContinue) {
            Write-OK "OpenSSH installed via winget"
            $sshdInstalled = $true
        }
    } catch {
        Write-Warn "winget not available, trying direct download..."
    }
}

# Method C: Direct download from GitHub (ultimate fallback, works on any Windows)
if (-not $sshdInstalled) {
    Write-Step "Downloading OpenSSH directly from GitHub (Microsoft release)..."
    $sshZipUrl  = "https://github.com/PowerShell/Win32-OpenSSH/releases/latest/download/OpenSSH-Win64.zip"
    $sshZipPath = "$env:TEMP\OpenSSH-Win64.zip"
    $sshExtract = "$env:TEMP\OpenSSH-Win64"
    $sshInstallPath = "C:\Program Files\OpenSSH"

    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $sshZipUrl -OutFile $sshZipPath -UseBasicParsing
        Write-Step "Extracting OpenSSH..."
        Expand-Archive -Path $sshZipPath -DestinationPath $env:TEMP -Force
        if (Test-Path $sshInstallPath) { Remove-Item -Recurse -Force $sshInstallPath }
        Move-Item "$sshExtract" $sshInstallPath -Force

        # Install the service using the bundled install script
        Push-Location $sshInstallPath
        powershell.exe -ExecutionPolicy Bypass -File ".\install-sshd.ps1" | Out-Null
        Pop-Location

        Write-OK "OpenSSH installed from GitHub release"
        $sshdInstalled = $true
    } catch {
        Write-Fail "Could not install OpenSSH automatically: $_"
        Write-Fail "Please install it manually from: https://github.com/PowerShell/Win32-OpenSSH/releases"
        exit 1
    }
}

# Configure sshd to auto-start
Set-Service sshd -StartupType Automatic -ErrorAction SilentlyContinue
Start-Service sshd -ErrorAction SilentlyContinue
Write-OK "sshd set to auto-start"

# Set default SSH shell to PowerShell
$pwshCmd = Get-Command pwsh -ErrorAction SilentlyContinue
$pwshPath = if ($pwshCmd) { $pwshCmd.Source } else { $null }
if (-not $pwshPath) {
    $psCmd = Get-Command powershell -ErrorAction SilentlyContinue
    $pwshPath = if ($psCmd) { $psCmd.Source } else { $null }
}
if ($pwshPath) {
    if (-not (Test-Path "HKLM:\SOFTWARE\OpenSSH")) {
        New-Item -Path "HKLM:\SOFTWARE\OpenSSH" -Force | Out-Null
    }
    New-ItemProperty -Path "HKLM:\SOFTWARE\OpenSSH" -Name "DefaultShell" `
        -Value $pwshPath -PropertyType String -Force | Out-Null
    Write-OK "Default SSH shell: $pwshPath"
}

# ── Step 3: Generate SSH keypair on THIS machine (so we have a key to work with)
#    We generate the key here, then the installer outputs the PUBLIC KEY so
#    the user can copy it to their laptop. The private key stays on this machine
#    only if needed, but more importantly we capture the owner's LAPTOP public key.
#    
#    Actually: we generate a keypair FOR THE OWNER'S LAPTOP right here,
#    then write it to authorized_keys. The owner copies the private key to their laptop.
#    This is the zero-manual-step approach — no need to bring a key FROM the laptop.
# ─────────────────────────────────────────────────────────────────────────────
Write-Step "Generating SSH keypair for owner access..."

$KeyDir     = "$DataDir\owner_key"
$PrivKeyPath = "$KeyDir\id_ed25519"
$PubKeyPath  = "$KeyDir\id_ed25519.pub"

New-Item -ItemType Directory -Force -Path $KeyDir | Out-Null

# Check if ssh-keygen is now available
$sshKeygen = Get-Command ssh-keygen -ErrorAction SilentlyContinue
if (-not $sshKeygen) {
    # Try known paths
    $candidates = @(
        "C:\Program Files\OpenSSH\ssh-keygen.exe",
        "C:\Windows\System32\OpenSSH\ssh-keygen.exe",
        "C:\Program Files\OpenSSH-Win64\ssh-keygen.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $sshKeygen = $c; break }
    }
}

if (-not $sshKeygen) {
    Write-Fail "ssh-keygen not found after OpenSSH install. Please restart and re-run."
    exit 1
}

$sshKeygenExe = if ($sshKeygen -is [string]) { $sshKeygen } else { $sshKeygen.Source }

if (-not (Test-Path $PrivKeyPath)) {
    & $sshKeygenExe -t ed25519 -f $PrivKeyPath -N '""' -C "Mass-owner" 2>$null
    Write-OK "SSH keypair generated"
} else {
    Write-OK "SSH keypair already exists"
}

$OwnerPublicKey = (Get-Content $PubKeyPath -Raw).Trim()
Write-OK "Owner public key ready"

# ── Step 4: Deploy hardened sshd_config ───────────────────────────────────────
Write-Step "Deploying hardened SSH config (your key only, no passwords)..."

$progDataFwd = $env:ProgramData.Replace('\','/')
$sshdConfig = @"
# Mass hardened sshd_config - auto-generated, do not edit manually
Port 22
AuthorizedKeysFile          $progDataFwd/ssh/administrators_authorized_keys
PubkeyAuthentication        yes
PasswordAuthentication      no
PermitEmptyPasswords        no
ChallengeResponseAuthentication no
KbdInteractiveAuthentication    no
GSSAPIAuthentication            no
HostbasedAuthentication         no
PermitRootLogin                 prohibit-password
MaxAuthTries                    3
MaxSessions                     10
LoginGraceTime                  20
ClientAliveInterval             60
ClientAliveCountMax             3
StrictModes                     yes
AllowTcpForwarding              yes
AllowAgentForwarding            yes
X11Forwarding                   no
Subsystem sftp                  sftp-server.exe
"@

Set-Content -Path "$SshDir\sshd_config" -Value $sshdConfig -Encoding UTF8
Write-OK "Hardened sshd_config written"

# ── Step 5: Write authorized_keys (ONLY the generated owner key) ──────────────
Write-Step "Writing authorized_keys (owner key only)..."

$adminAuthKeys = "$SshDir\administrators_authorized_keys"
Set-Content -Path $adminAuthKeys -Value $OwnerPublicKey -Encoding UTF8
Write-OK "authorized_keys written"

# Lock down file permissions
try {
    $acl = New-Object System.Security.AccessControl.FileSecurity
    $acl.SetAccessRuleProtection($true, $false)
    $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule("SYSTEM","FullControl","Allow")))
    $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule("Administrators","FullControl","Allow")))
    Set-Acl -Path $adminAuthKeys -AclObject $acl
    Write-OK "authorized_keys permissions locked"
} catch {
    # icacls fallback
    icacls $adminAuthKeys /inheritance:r /grant "SYSTEM:F" /grant "Administrators:F" 2>$null | Out-Null
    Write-OK "authorized_keys permissions set via icacls"
}

# ── Step 6: Create MassUsers group ─────────────────────────────────────
Write-Step "Setting up access control group..."
if (-not (Get-LocalGroup $AccessGroup -ErrorAction SilentlyContinue)) {
    New-LocalGroup -Name $AccessGroup -Description "Mass SSH access group" | Out-Null
}
Add-LocalGroupMember -Group $AccessGroup -Member "Administrators" -ErrorAction SilentlyContinue
Write-OK "Access group configured"

# Restart sshd with new config
Write-Step "Restarting sshd with hardened config..."
Restart-Service sshd -Force -ErrorAction SilentlyContinue
Start-Sleep 2
Write-OK "sshd restarted"

# ── Step 7: Download msagent.exe ───────────────────────────────────────────
Write-Step "Downloading msagent.exe..."

$downloaded = $false

# Force TLS 1.2 — required for GitHub, older Windows defaults to TLS 1.0
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# Try GitHub release
try {
    $wc = New-Object System.Net.WebClient
    $wc.Headers.Add("User-Agent", "ms-installer/1.0")
    $wc.DownloadFile($AgentDownloadUrl, $AgentExe)
    if ((Test-Path $AgentExe) -and (Get-Item $AgentExe).Length -gt 10000) {
        Write-OK "Downloaded msagent.exe from GitHub"
        $downloaded = $true
    }
} catch {
    Write-Warn "GitHub release download failed: $_"
    Write-Warn "Trying Invoke-WebRequest fallback..."
    try {
        Invoke-WebRequest -Uri $AgentDownloadUrl -OutFile $AgentExe `
            -UseBasicParsing -Headers @{"User-Agent"="ms-installer/1.0"} -ErrorAction Stop
        if ((Test-Path $AgentExe) -and (Get-Item $AgentExe).Length -gt 10000) {
            Write-OK "Downloaded msagent.exe (fallback)"
            $downloaded = $true
        }
    } catch {
        Write-Warn "Both download methods failed: $_"
    }
}

# Fallback: check next to this script (if distributed as a zip)
if (-not $downloaded) {
    $scriptPath = $MyInvocation.MyCommand.Path
    if ($scriptPath) {
        $localCopy = Join-Path (Split-Path $scriptPath) "msagent.exe"
        if (Test-Path $localCopy) {
            Copy-Item $localCopy $AgentExe -Force
            Write-OK "Copied bundled msagent.exe"
            $downloaded = $true
        }
    }
}

# Fallback: check temp dir (in case it was downloaded there)
if (-not $downloaded) {
    $tempCopy = "$env:TEMP\msagent.exe"
    if (Test-Path $tempCopy) {
        Copy-Item $tempCopy $AgentExe -Force
        Write-OK "Found msagent.exe in temp"
        $downloaded = $true
    }
}

if (-not $downloaded) {
    Write-Host ""
    Write-Host "  ================================================================" -ForegroundColor Yellow
    Write-Host "  msagent.exe could not be downloaded automatically." -ForegroundColor Yellow
    Write-Host "  This is because no GitHub Release exists yet for the repo." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  TO FIX: On your laptop, run:" -ForegroundColor White
    Write-Host "    cd p:\Projects\ssh-access" -ForegroundColor Cyan
    Write-Host "    build\build-agent.bat" -ForegroundColor Cyan
    Write-Host "  Then go to https://github.com/ketw/sshra/releases" -ForegroundColor White
    Write-Host "  Click 'Draft a new release', tag v1.0.0, upload build\msagent.exe" -ForegroundColor White
    Write-Host "  Then re-run this installer." -ForegroundColor White
    Write-Host "  ================================================================" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  Press any key to close..." -ForegroundColor DarkGray
    try { $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown") } catch { Start-Sleep 10 }
    exit 1
}

# ── Step 8: Write registry config ─────────────────────────────────────────────
Write-Step "Writing service configuration..."

$machineGuid = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Cryptography").MachineGuid
$DeviceId = $machineGuid.Replace("-","")
if ($DeviceId.Length -gt 32) { $DeviceId = $DeviceId.Substring(0,32) }

if (-not (Test-Path $RegKey)) { New-Item -Path $RegKey -Force | Out-Null }
Set-ItemProperty -Path $RegKey -Name "RelayHost"   -Value $RelayHost  -Type String
Set-ItemProperty -Path $RegKey -Name "RelayPort"   -Value $RelayPort  -Type String
Set-ItemProperty -Path $RegKey -Name "AuthToken"   -Value $Token      -Type String
Set-ItemProperty -Path $RegKey -Name "DeviceLabel" -Value $Label      -Type String
Set-ItemProperty -Path $RegKey -Name "DeviceID"    -Value $DeviceId   -Type String
Write-OK "Registry config written"

# ── Step 9: Install Windows service ───────────────────────────────────────────
Write-Step "Installing Mass Windows service..."

# Remove old service if present
$old = Get-Service $ServiceName -ErrorAction SilentlyContinue
if ($old) {
    Stop-Service $ServiceName -Force -ErrorAction SilentlyContinue
    Start-Sleep 2
    & $AgentExe --uninstall 2>$null
    Start-Sleep 1
}

& $AgentExe --install
Start-Sleep 2

# Confirm running
$svc = Get-Service $ServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq "Running") {
    Write-OK "Mass Agent service is running"
} else {
    Start-Service $ServiceName -ErrorAction SilentlyContinue
    Start-Sleep 2
    $svc = Get-Service $ServiceName -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -eq "Running") {
        Write-OK "Service started"
    } else {
        Write-Warn "Service installed but not running yet. Check: $DataDir\agent.log"
    }
}

# Ensure auto-start (not delayed)
sc.exe config $ServiceName start= auto | Out-Null
sc.exe config $ServiceName start= auto delayed= false 2>$null | Out-Null
Write-OK "Service set to auto-start (pre-login)"

# ── Step 10: Firewall rules ────────────────────────────────────────────────────
Write-Step "Configuring firewall..."
Remove-NetFirewallRule -DisplayName "Mass*" -ErrorAction SilentlyContinue
New-NetFirewallRule -DisplayName "Mass SSH In" `
    -Direction Inbound -Protocol TCP -LocalPort 22 -Action Allow | Out-Null
New-NetFirewallRule -DisplayName "Mass Relay Out" `
    -Direction Outbound -Protocol TCP -RemotePort $RelayPort -Action Allow | Out-Null
Write-OK "Firewall rules set"

# ── Step 11: Try to install OpenHardwareMonitor silently (for temps) ──────────
Write-Step "Checking OpenHardwareMonitor (for CPU/GPU temps)..."
$ohmExe = "C:\Program Files\OpenHardwareMonitor\OpenHardwareMonitor.exe"
if (-not (Test-Path $ohmExe)) {
    try {
        $ohmZipUrl  = "https://openhardwaremonitor.org/files/openhardwaremonitor-r0.9.6.zip"
        $ohmZip     = "$env:TEMP\ohm.zip"
        $ohmExtract = "$env:TEMP\ohm_extract"
        Invoke-WebRequest -Uri $ohmZipUrl -OutFile $ohmZip -UseBasicParsing -TimeoutSec 30
        Expand-Archive -Path $ohmZip -DestinationPath $ohmExtract -Force
        $ohmDir = "C:\Program Files\OpenHardwareMonitor"
        New-Item -ItemType Directory -Force -Path $ohmDir | Out-Null
        Get-ChildItem "$ohmExtract\OpenHardwareMonitor" | Copy-Item -Destination $ohmDir -Recurse -Force
        # Add to startup (HKLM run key) so it starts with Windows and exposes shared memory
        $ohmRunKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run"
        Set-ItemProperty -Path $ohmRunKey -Name "OpenHardwareMonitor" -Value "`"$ohmExe`" /startup"
        # Start it now
        Start-Process $ohmExe -WindowStyle Hidden -ErrorAction SilentlyContinue
        Write-OK "OpenHardwareMonitor installed and started"
    } catch {
        Write-Warn "Could not auto-install OpenHardwareMonitor (CPU/GPU temps will show N/A)"
        Write-Warn "Install manually from: https://openhardwaremonitor.org/downloads/"
    }
} else {
    Write-OK "OpenHardwareMonitor already installed"
}

# ── Done ──────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "  ╔══════════════════════════════════════════════════════╗" -ForegroundColor Green
Write-Host "  ║              Installation Complete!                  ║" -ForegroundColor Green
Write-Host "  ╚══════════════════════════════════════════════════════╝" -ForegroundColor Green
Write-Host ""
Write-Host "  Device     : $Label ($DeviceId)" -ForegroundColor Cyan
Write-Host "  Relay      : ${RelayHost}:${RelayPort}" -ForegroundColor Cyan
Write-Host "  Service    : Running (auto-starts before login)" -ForegroundColor Cyan
Write-Host "  SSH        : Locked to owner key only, no passwords" -ForegroundColor Cyan
Write-Host ""
Write-Host "  ┌─────────────────────────────────────────────────────┐" -ForegroundColor Yellow
Write-Host "  │  ACTION NEEDED - copy this private key to your      │" -ForegroundColor Yellow
Write-Host "  │  laptop to be able to SSH into this machine:        │" -ForegroundColor Yellow
Write-Host "  └─────────────────────────────────────────────────────┘" -ForegroundColor Yellow
Write-Host ""
Write-Host "  Private key is at:" -ForegroundColor White
Write-Host "    $PrivKeyPath" -ForegroundColor White
Write-Host ""
Write-Host "  Copy it to your laptop:" -ForegroundColor White
Write-Host "    - Open the file above in Notepad, copy the contents" -ForegroundColor White
Write-Host "    - On your laptop, save it as: ~/.ssh/id_ms_${Label.Replace(' ','_')}" -ForegroundColor White
Write-Host "    - Then SSH with: ssh -i ~/.ssh/id_ms_${Label.Replace(' ','_')} Administrator@<ip>" -ForegroundColor White
Write-Host "    - msmgr will use it automatically" -ForegroundColor White
Write-Host ""

# Display the public key for reference
Write-Host "  Public key (already locked to this PC):" -ForegroundColor DarkGray
Write-Host "  $OwnerPublicKey" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  Log file: $DataDir\agent.log" -ForegroundColor DarkGray
Write-Host ""

# Offer to display private key contents directly in terminal for easy copy
$show = Read-Host "  Show private key contents in terminal now for copy-paste? [Y/N]"
if ($show -match '^[Yy]') {
    Write-Host ""
    Write-Host "  ===== PRIVATE KEY (copy everything between the lines) =====" -ForegroundColor Yellow
    Get-Content $PrivKeyPath
    Write-Host "  ===== END PRIVATE KEY =====" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  Save this as ~/.ssh/id_ed25519 on your laptop (or any filename you want)" -ForegroundColor Cyan
    Write-Host "  Set permissions: chmod 600 ~/.ssh/id_ed25519  (on Linux/macOS)" -ForegroundColor Cyan
    Write-Host "  On Windows: the file just needs to be in your .ssh folder" -ForegroundColor Cyan
}

Write-Host ""
Write-Host "  All done. This device will connect to the relay automatically on every boot." -ForegroundColor Green
Write-Host ""
Write-Host "  Press any key to close this window..." -ForegroundColor DarkGray
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
