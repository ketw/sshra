#Requires -RunAsAdministrator
<#
.SYNOPSIS
    KiroAccess Self-Contained Installer
    Run this once on each machine you want to remotely access.
    It will: install OpenSSH, compile+install the agent service,
    configure firewall, generate auth token, register with relay.

    SECURITY: Only YOUR laptop's SSH public key will ever be able to connect.
    No passwords. No other keys. The agent audits this every 60 seconds.

.EXAMPLE
    # Full silent install
    .\install.ps1 -RelayHost "sshra.onrender.com" -Label "Gaming PC" `
                  -Token "mysecret" -OwnerPublicKey "ssh-ed25519 AAAA... laptop"

    # Interactive (prompts for everything including your public key)
    .\install.ps1
#>
param(
    [string]$RelayHost      = "",
    [string]$RelayPort      = "7744",
    [string]$Label          = "",
    [string]$Token          = "",
    [string]$OwnerPublicKey = "",   # Your laptop's ~/.ssh/id_ed25519.pub content
    [string]$AgentUrl       = "",   # URL to download prebuilt kiro-agent.exe
    [switch]$Uninstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$InstallDir  = "C:\Program Files\KiroAccess"
$DataDir     = "C:\ProgramData\KiroAccess"
$SshDataDir  = "C:\ProgramData\ssh"
$ServiceName = "KiroAccessAgent"
$RegKey      = "HKLM:\SOFTWARE\KiroAccess"
$AccessGroup = "KiroAccessUsers"

# ── Colours ──────────────────────────────────────────────────────────────────
function Write-Step   { param($m) Write-Host "  [>>] $m" -ForegroundColor Cyan }
function Write-OK     { param($m) Write-Host "  [ OK] $m" -ForegroundColor Green }
function Write-Fail   { param($m) Write-Host "  [!!] $m" -ForegroundColor Red }
function Write-Banner {
    Write-Host ""
    Write-Host "  ╔══════════════════════════════════════╗" -ForegroundColor Magenta
    Write-Host "  ║      KiroAccess Agent Installer      ║" -ForegroundColor Magenta
    Write-Host "  ╚══════════════════════════════════════╝" -ForegroundColor Magenta
    Write-Host ""
}

# ── Uninstall path ────────────────────────────────────────────────────────────
if ($Uninstall) {
    Write-Step "Stopping and removing service..."
    Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
    sc.exe delete $ServiceName | Out-Null
    Remove-Item -Recurse -Force $InstallDir  -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $DataDir     -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $RegKey      -ErrorAction SilentlyContinue
    # Remove firewall rules
    Remove-NetFirewallRule -DisplayName "KiroAccess*" -ErrorAction SilentlyContinue
    # Remove access group
    Remove-LocalGroup -Name $AccessGroup -ErrorAction SilentlyContinue
    Write-OK "Uninstalled."
    exit 0
}

Write-Banner

# ── Gather config interactively if not supplied ───────────────────────────────
if (-not $RelayHost) {
    $RelayHost = Read-Host "  Relay server hostname/IP (e.g. relay.example.com)"
}
if (-not $Label) {
    $Label = Read-Host "  Device label (e.g. 'Gaming PC', 'Bedroom PC')"
}
if (-not $Token) {
    # Generate a random token if not provided
    $bytes = New-Object byte[] 32
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $Token = [System.Convert]::ToBase64String($bytes).Replace("+","").Replace("/","").Replace("=","").Substring(0,32)
    Write-Host "  Generated auth token: " -NoNewline
    Write-Host $Token -ForegroundColor Yellow
    Write-Host "  SAVE THIS - you need it in the manager on your laptop"
    Write-Host ""
}

# ── Owner SSH public key (MANDATORY for security) ─────────────────────────────
if (-not $OwnerPublicKey) {
    Write-Host ""
    Write-Host "  ┌─────────────────────────────────────────────────────────┐" -ForegroundColor Yellow
    Write-Host "  │  SECURITY: Your SSH public key is required              │" -ForegroundColor Yellow
    Write-Host "  │  ONLY this key will ever be able to SSH into this PC.   │" -ForegroundColor Yellow
    Write-Host "  │  No passwords. No other keys. Ever.                     │" -ForegroundColor Yellow
    Write-Host "  └─────────────────────────────────────────────────────────┘" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  On your laptop, run:  cat ~/.ssh/id_ed25519.pub" -ForegroundColor Cyan
    Write-Host "  (or id_rsa.pub if you use RSA)" -ForegroundColor Cyan
    Write-Host "  Then paste the full line here (starts with ssh-ed25519 or ssh-rsa):"
    Write-Host ""
    $OwnerPublicKey = Read-Host "  Your public key"
    $OwnerPublicKey = $OwnerPublicKey.Trim()
}

# Validate key format
if (-not ($OwnerPublicKey -match '^(ssh-ed25519|ssh-rsa|ecdsa-sha2-nistp\d+|sk-ssh-ed25519)\s+[A-Za-z0-9+/=]+')) {
    Write-Fail "That does not look like a valid SSH public key."
    Write-Fail "Expected format: ssh-ed25519 AAAA... [comment]"
    Write-Fail "Generate one on your laptop with:  ssh-keygen -t ed25519 -C laptop"
    exit 1
}
Write-OK "Owner public key validated"

# ── Create directories ────────────────────────────────────────────────────────
Write-Step "Creating install directories..."
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path $DataDir    | Out-Null
Write-OK "Directories ready"

# ── Install OpenSSH ───────────────────────────────────────────────────────────
Write-Step "Checking OpenSSH Server..."
$sshCap = Get-WindowsCapability -Online | Where-Object { $_.Name -like "OpenSSH.Server*" }
if ($sshCap -and $sshCap.State -ne "Installed") {
    Write-Step "Installing OpenSSH Server (Windows optional feature)..."
    Add-WindowsCapability -Online -Name $sshCap.Name | Out-Null
    Write-OK "OpenSSH Server installed"
} elseif ($sshCap) {
    Write-OK "OpenSSH Server already installed"
} else {
    # Fallback: try winget
    Write-Step "Trying winget for OpenSSH..."
    winget install --id Microsoft.OpenSSH.Beta -e --silent 2>$null
}

# Ensure sshd service is set to auto-start
Set-Service -Name sshd -StartupType Automatic -ErrorAction SilentlyContinue
Start-Service sshd -ErrorAction SilentlyContinue
Write-OK "sshd service configured (auto-start)"

# Set default shell to PowerShell for SSH sessions
$pwshPath = (Get-Command pwsh -ErrorAction SilentlyContinue)?.Source
if (-not $pwshPath) { $pwshPath = (Get-Command powershell).Source }
New-ItemProperty -Path "HKLM:\SOFTWARE\OpenSSH" -Name "DefaultShell" `
    -Value $pwshPath -PropertyType String -Force | Out-Null
Write-OK "Default SSH shell: $pwshPath"

# ── Deploy kiro-agent.exe ─────────────────────────────────────────────────────
Write-Step "Deploying kiro-agent.exe..."
$AgentExe = Join-Path $InstallDir "kiro-agent.exe"

if ($AgentUrl) {
    # Download prebuilt binary
    Write-Step "Downloading from $AgentUrl..."
    Invoke-WebRequest -Uri $AgentUrl -OutFile $AgentExe -UseBasicParsing
    Write-OK "Downloaded kiro-agent.exe"
} else {
    # Try to compile from source if MSVC or MinGW is available
    $scriptDir  = Split-Path $MyInvocation.MyCommand.Path -Parent
    $sourceDir  = Join-Path $scriptDir ".."
    $agentSrc   = Join-Path $sourceDir "agent\agent.c"

    $compiled = $false

    # Try cl.exe (MSVC)
    $cl = Get-Command cl -ErrorAction SilentlyContinue
    if ($cl -and (Test-Path $agentSrc)) {
        Write-Step "Compiling with MSVC cl.exe..."
        $commonDir = Join-Path $sourceDir "common"
        Push-Location (Join-Path $sourceDir "agent")
        cl.exe agent.c /Fe:"$AgentExe" /I"$commonDir" /O2 `
            /link ws2_32.lib advapi32.lib pdh.lib iphlpapi.lib 2>&1 | Out-Null
        Pop-Location
        if (Test-Path $AgentExe) { $compiled = $true; Write-OK "Compiled with MSVC" }
    }

    # Try gcc (MinGW/MSYS2)
    if (-not $compiled) {
        $gcc = Get-Command gcc -ErrorAction SilentlyContinue
        if ($gcc -and (Test-Path $agentSrc)) {
            Write-Step "Compiling with GCC..."
            $commonDir = Join-Path $sourceDir "common"
            gcc "$agentSrc" -I"$commonDir" -O2 -o "$AgentExe" `
                -lws2_32 -ladvapi32 -lpdh -liphlpapi -lpowrprof 2>&1 | Out-Null
            if (Test-Path $AgentExe) { $compiled = $true; Write-OK "Compiled with GCC" }
        }
    }

    if (-not $compiled) {
        # Last resort: copy from same directory as installer if it was distributed together
        $localExe = Join-Path $scriptDir "kiro-agent.exe"
        if (Test-Path $localExe) {
            Copy-Item $localExe $AgentExe -Force
            Write-OK "Copied bundled kiro-agent.exe"
        } else {
            Write-Fail "Cannot find or compile kiro-agent.exe."
            Write-Fail "Either provide -AgentUrl, ensure MSVC/GCC is in PATH, or place kiro-agent.exe next to install.ps1"
            exit 1
        }
    }
}

# ── Write registry config ─────────────────────────────────────────────────────
Write-Step "Writing configuration to registry..."

# Generate stable device ID from machine GUID
$machineGuid = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Cryptography").MachineGuid
$DeviceId    = $machineGuid.Replace("-","").Substring(0, [Math]::Min(32, $machineGuid.Replace("-","").Length))

if (-not (Test-Path $RegKey)) {
    New-Item -Path $RegKey -Force | Out-Null
}
Set-ItemProperty -Path $RegKey -Name "RelayHost"   -Value $RelayHost  -Type String
Set-ItemProperty -Path $RegKey -Name "RelayPort"   -Value $RelayPort  -Type String
Set-ItemProperty -Path $RegKey -Name "AuthToken"   -Value $Token      -Type String
Set-ItemProperty -Path $RegKey -Name "DeviceLabel" -Value $Label      -Type String
Set-ItemProperty -Path $RegKey -Name "DeviceID"    -Value $DeviceId   -Type String
Write-OK "Registry config written"

# ── Configure OpenSSH: owner-only hardened setup ─────────────────────────────
Write-Step "Configuring SSH security (owner-only access)..."

New-Item -ItemType Directory -Force -Path $SshDataDir | Out-Null

$adminAuthKeys = "$SshDataDir\administrators_authorized_keys"

# Write ONLY the owner's public key — nothing else
Set-Content -Path $adminAuthKeys -Value $OwnerPublicKey -Encoding UTF8 -NoNewline
Add-Content -Path $adminAuthKeys -Value ""  # trailing newline
Write-OK "Owner public key written to authorized_keys"

# Lock down authorized_keys: SYSTEM + Administrators only, Everyone denied write
$acl = New-Object System.Security.AccessControl.FileSecurity
$acl.SetAccessRuleProtection($true, $false)
$acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(
    "SYSTEM",          "FullControl", "Allow")))
$acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(
    "Administrators",  "FullControl", "Allow")))
$acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(
    "Everyone",        "Write,Modify,Delete,TakeOwnership,ChangePermissions", "Deny")))
Set-Acl -Path $adminAuthKeys -AclObject $acl
Write-OK "authorized_keys permissions locked (SYSTEM+Admins only, Everyone write-denied)"

# Deploy hardened sshd_config
Write-Step "Deploying hardened sshd_config (public-key auth only)..."
$scriptDir    = Split-Path $MyInvocation.MyCommand.Path -Parent
$hardenedConf = Join-Path $scriptDir "sshd_config.hardened"

$sshdConfigPath = "$SshDataDir\sshd_config"

if (Test-Path $hardenedConf) {
    $confContent = Get-Content $hardenedConf -Raw
    # Replace placeholder with actual ProgramData path (forward slashes for sshd)
    $confContent = $confContent -replace '__PROGRAMDATA__', $env:ProgramData.Replace('\','/')
    Set-Content -Path $sshdConfigPath -Value $confContent -Encoding UTF8
    Write-OK "Hardened sshd_config deployed"
} else {
    # Fallback: write minimal hardened config inline
    $minimalConf = @"
Port 22
AuthorizedKeysFile          __PROGRAMDATA__/ssh/administrators_authorized_keys
PubkeyAuthentication        yes
PasswordAuthentication      no
PermitEmptyPasswords        no
ChallengeResponseAuthentication no
KbdInteractiveAuthentication    no
HostbasedAuthentication         no
MaxAuthTries                3
LoginGraceTime              20
StrictModes                 yes
AllowGroups                 KiroAccessUsers Administrators
Subsystem sftp              sftp-server.exe
"@
    $minimalConf = $minimalConf -replace '__PROGRAMDATA__', $env:ProgramData.Replace('\','/')
    Set-Content -Path $sshdConfigPath -Value $minimalConf -Encoding UTF8
    Write-OK "Minimal hardened sshd_config written"
}

# Create KiroAccessUsers local group (only members can SSH in via AllowGroups)
Write-Step "Creating KiroAccessUsers local group..."
$grp = Get-LocalGroup -Name $AccessGroup -ErrorAction SilentlyContinue
if (-not $grp) {
    New-LocalGroup -Name $AccessGroup `
        -Description "KiroAccess: only members of this group may connect via SSH" | Out-Null
    Write-OK "Created local group: $AccessGroup"
} else {
    Write-OK "Local group already exists: $AccessGroup"
}

# Add Administrators to the group so admin accounts can connect
Add-LocalGroupMember -Group $AccessGroup -Member "Administrators" `
    -ErrorAction SilentlyContinue
Write-OK "Administrators added to $AccessGroup"

# Restart sshd to apply config
Write-Step "Restarting sshd to apply hardened config..."
Restart-Service sshd -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
$sshdSvc = Get-Service sshd -ErrorAction SilentlyContinue
if ($sshdSvc -and $sshdSvc.Status -eq "Running") {
    Write-OK "sshd restarted with hardened config"
} else {
    Write-Host "  [!!] sshd failed to restart. Check: $sshdConfigPath" -ForegroundColor Red
}

# ── Firewall rules ────────────────────────────────────────────────────────────
Write-Step "Configuring firewall..."
$existingSSH = Get-NetFirewallRule -DisplayName "KiroAccess SSH" -ErrorAction SilentlyContinue
if (-not $existingSSH) {
    New-NetFirewallRule -DisplayName "KiroAccess SSH" `
        -Direction Inbound -Protocol TCP -LocalPort 22 -Action Allow | Out-Null
}
$existingRelay = Get-NetFirewallRule -DisplayName "KiroAccess Relay" -ErrorAction SilentlyContinue
if (-not $existingRelay) {
    New-NetFirewallRule -DisplayName "KiroAccess Relay" `
        -Direction Outbound -Protocol TCP -RemotePort $RelayPort -Action Allow | Out-Null
}
Write-OK "Firewall rules set"

# ── Install + start the agent Windows service ─────────────────────────────────
Write-Step "Installing KiroAccess service..."

# Stop existing service if running
$existingSvc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($existingSvc) {
    Write-Step "Removing previous installation..."
    Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    & "$AgentExe" --uninstall 2>$null
    Start-Sleep -Seconds 1
}

# Install and start
& "$AgentExe" --install
Start-Sleep -Seconds 2

$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq "Running") {
    Write-OK "KiroAccess Agent service is running"
} else {
    Write-Step "Starting service manually..."
    Start-Service -Name $ServiceName -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -eq "Running") {
        Write-OK "Service running"
    } else {
        Write-Fail "Service failed to start. Check: $DataDir\agent.log"
    }
}

# ── Configure service to start before login (pre-session) ────────────────────
Write-Step "Configuring pre-login autostart..."
# SERVICE_AUTO_START is already set, but we also need to ensure it starts
# before the login screen (type = SERVICE_WIN32_OWN_PROCESS with SYSTEM account
# and auto-start achieves this on Windows - sshd also uses this pattern)
sc.exe config $ServiceName start= boot 2>$null
# Note: 'boot' only works for kernel drivers; for win32 services 'auto' is correct
# and they start before user login. Reset to auto:
sc.exe config $ServiceName start= auto | Out-Null
# Ensure service is not delayed:
sc.exe config $ServiceName start= auto delayed= false 2>$null | Out-Null
Write-OK "Service configured for pre-login autostart (SYSTEM account)"

# ── Optional: install OpenHardwareMonitor for better temp readings ────────────
Write-Step "Checking for hardware temp sensor support..."
$ohmPath = "C:\Program Files\OpenHardwareMonitor\OpenHardwareMonitor.exe"
if (-not (Test-Path $ohmPath)) {
    Write-Host "  OpenHardwareMonitor (OHM) is NOT installed." -ForegroundColor Yellow
    Write-Host "  CPU/GPU temperatures will show as N/A without it." -ForegroundColor Yellow
    Write-Host "  Download from: https://openhardwaremonitor.org/downloads/" -ForegroundColor Yellow
    Write-Host "  Install and set it to run at startup for full telemetry." -ForegroundColor Yellow
} else {
    Write-OK "OpenHardwareMonitor found"
}

# ── Summary ───────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "  ╔══════════════════════════════════════════════════╗" -ForegroundColor Green
Write-Host "  ║           Installation Complete!                 ║" -ForegroundColor Green
Write-Host "  ╚══════════════════════════════════════════════════╝" -ForegroundColor Green
Write-Host ""
Write-Host "  Device ID  : $DeviceId"              -ForegroundColor Cyan
Write-Host "  Label      : $Label"                 -ForegroundColor Cyan
Write-Host "  Relay      : ${RelayHost}:${RelayPort}" -ForegroundColor Cyan
Write-Host "  Auth Token : $Token"                 -ForegroundColor Yellow
Write-Host ""
Write-Host "  Security:"                           -ForegroundColor White
Write-Host "  - SSH locked to YOUR public key only (no passwords, no other keys)" -ForegroundColor Green
Write-Host "  - Agent audits authorized_keys every 60s and stops sshd if tampered" -ForegroundColor Green
Write-Host "  - sshd_config: PubkeyAuthentication only, AllowGroups restricted" -ForegroundColor Green
Write-Host ""
Write-Host "  NEXT STEPS on your laptop:" -ForegroundColor White
Write-Host "  1. Copy the auth token above into kiro-manager" -ForegroundColor White
Write-Host "  2. Run: kiro-manager.exe ${RelayHost} ${Token}" -ForegroundColor White
Write-Host ""
Write-Host "  Log file: $DataDir\agent.log" -ForegroundColor DarkGray
