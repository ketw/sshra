# Mass

Remote access system written in C. Lets you control any of your Windows PCs
from your laptop, anywhere on the internet — SSH shell, file transfer, RDP,
and live hardware telemetry (CPU/GPU temps, RAM, network).

```
┌─────────────┐        internet / LAN        ┌──────────────────┐
│  Your Laptop│ ◄──── msmgr.exe ──────►│  Relay Server    │
│             │                               │  (VPS / cloud)   │
└─────────────┘                               └────────┬─────────┘
                                                       │
                    ┌──────────────────────────────────┤
                    │                    │             │
             ┌──────▼──────┐    ┌────────▼─────┐  ┌───▼──────────┐
             │ Gaming PC   │    │ Bedroom PC   │  │ Workstation  │
             │ msagent  │    │ msagent   │  │ msagent   │
             └─────────────┘    └──────────────┘  └──────────────┘
```

---

## Files

```
ssh-access/
├── common/
│   ├── protocol.h      Wire protocol constants and types
│   └── json_util.h     Zero-dependency JSON builder/parser
├── agent/
│   └── agent.c         Windows service installed on each PC
├── relay/
│   └── relay.c         Relay server (runs on a VPS)
├── manager/
│   └── manager.c       TUI manager (runs on your laptop)
├── installer/
│   └── install.ps1     Self-contained installer for each PC
├── build/
│   ├── build-all.bat   Build agent + manager (Windows)
│   ├── build-agent.bat Build agent only
│   ├── build-manager.bat Build manager only
│   └── build-relay.sh  Build relay (Linux/macOS VPS)
└── deploy/
    └── msrelay.service  systemd unit for the relay
```

---

## Quick Start

### Step 1 — Deploy the relay server (one time)

You need a VPS with a public IP (any $5/month Linux VPS works).

```bash
# On your VPS:
git clone <this repo>  # or scp the files
cd ssh-access
bash build/build-relay.sh

# Copy the binary and run it:
sudo cp build/msrelay /usr/local/bin/
sudo cp deploy/msrelay.service /etc/systemd/system/

# Edit the service file — set your secret token:
sudo nano /etc/systemd/system/msrelay.service
# Change:  --token REPLACE_WITH_YOUR_TOKEN
# To:      --token mysupersecrettoken123

sudo systemctl daemon-reload
sudo systemctl enable --now msrelay
```

The relay listens on:
- Port **7744** — agents connect here
- Port **7745** — manager connects here

Open both ports in your VPS firewall/security group.

---

### Step 2 — Install agent on each PC (Gaming PC, Bedroom PC, Workstation)

Run this once on each machine you want to access. Open PowerShell as
Administrator and run:

```powershell
# Option A — if you've built msagent.exe already, put it next to install.ps1
.\installer\install.ps1 -RelayHost "your.vps.ip" -Label "Gaming PC" -Token "mysupersecrettoken123"

# Option B — completely interactive (prompts for everything)
.\installer\install.ps1
```

The installer will:
1. Install **Windows OpenSSH Server** (if not present)
2. Deploy `msagent.exe` as a **Windows service** (SYSTEM account)
3. Configure the service to **start before the login screen**
4. Add **firewall rules** for SSH (port 22) and relay (port 7744)
5. Write config to the registry
6. Print your **auth token** — save it for the manager

After install, the agent connects to the relay immediately and stays connected.
It survives reboots, crashes (auto-restart), and runs even before anyone logs in.

---

### Step 3 — Add your SSH public key

On each PC you installed the agent on, add your laptop's public key so you can
log in without a password:

```powershell
# On each target PC (or do it via install.ps1 prompt):
notepad "C:\ProgramData\ssh\administrators_authorized_keys"
# Paste your public key (from: cat ~/.ssh/id_ed25519.pub on your laptop)
```

Generate a key on your laptop if you don't have one:
```powershell
ssh-keygen -t ed25519 -C "laptop"
```

---

### Step 4 — Run the manager on your laptop

```powershell
# Build first:
.\build\build-manager.bat

# Run (first time — pass relay info as arguments):
.\build\msmgr.exe your.vps.ip mysupersecrettoken123

# After that, config is saved — just run:
.\build\msmgr.exe
```

---

## Manager Keybinds

| Key       | Action                                      |
|-----------|---------------------------------------------|
| `↑` / `↓` | Navigate device list                        |
| `Enter`   | SSH into selected device (new window)       |
| `F`       | File transfer — SFTP (WinSCP if installed)  |
| `R`       | Remote Desktop (RDP via mstsc)              |
| `T`       | Open another SSH window to same device      |
| `A`       | Add a relay server                          |
| `Q` / Esc | Quit                                        |

---

## Hardware Telemetry

The agent reports live stats every 30 seconds:

| Metric       | Source                                          |
|--------------|-------------------------------------------------|
| CPU usage %  | PDH `\\Processor(_Total)\\% Processor Time`    |
| CPU temp     | OpenHardwareMonitor shared memory (if running)  |
| GPU temp     | OpenHardwareMonitor shared memory (if running)  |
| RAM used/total | `GlobalMemoryStatusEx`                        |
| Network RX/TX | `GetIfTable` delta                             |
| Battery %    | `GetSystemPowerStatus`                          |
| Uptime       | `GetTickCount64`                                |

For CPU/GPU temperatures to show up, install
[OpenHardwareMonitor](https://openhardwaremonitor.org/downloads/) on each PC
and set it to **run at Windows startup**. It exposes a shared memory segment
that the agent reads without any additional dependencies.

---

## How Internet Access Works

```
Agent (behind home router NAT)
  └─► connects OUT to relay:7744  (outbound TCP — no port forward needed)
       └─► relay keeps socket open, records device IP + stats

Manager (your laptop, anywhere)
  └─► connects OUT to relay:7745
       └─► sends LIST → relay returns all device IPs + stats
       └─► sends CONNECT device_id → relay returns device public IP + SSH port
       └─► manager launches ssh.exe directly to that IP
```

**No port forwarding required on your home router** for the agent — it initiates
the outbound connection. You do need port 22 open on the home router for direct
SSH (or the relay can be extended to proxy the TCP stream if you're behind
strict NAT — see the Relay Proxy section below).

### Strict NAT / No port forwarding?

If you cannot open port 22 on your home router, set up a **reverse SSH tunnel**
from each agent to your VPS:

```powershell
# Add to the agent's startup (or the install script can configure this):
# This creates a tunnel: connecting to vps:2222 goes to that PC's local port 22
ssh -R 2222:localhost:22 -N -o StrictHostKeyChecking=accept-new relay-user@your.vps.ip
```

Then in the manager, set `ssh_port` to 2222 and connect to the VPS IP instead.
The installer can be extended to set this up automatically.

---

## Building

### Windows (agent + manager)

Requires **Visual Studio Build Tools** or **MinGW-w64**.

```batch
:: Install VS Build Tools (free):
:: https://aka.ms/vs/17/release/vs_BuildTools.exe
:: Select: "C++ build tools" workload

:: Then from a Developer Command Prompt:
build\build-all.bat
```

With MinGW from [MSYS2](https://www.msys2.org/):
```bash
pacman -S mingw-w64-x86_64-gcc
build/build-all.bat
```

### Linux/macOS (relay only)

```bash
# Requires only gcc and glibc
bash build/build-relay.sh
```

---

## Security Notes

- The **auth token** is a shared secret between agents, relay, and manager.
  Use a long random string (32+ chars). The installer generates one for you.
- All connections are TCP. **Add TLS** by fronting the relay with nginx/stunnel
  for production use — the wire format is already length-prefixed so it wraps
  cleanly.
- SSH keys are used for actual shell access — the auth token only controls
  who can see your device list via the relay.
- The agent runs as **SYSTEM** — it has full access to the machine. Only
  install on machines you own.
- The `administrators_authorized_keys` file controls who can SSH in.
  Keep your private key safe.

---

## Relay Token Setup (summary)

All three components must use the same token:

| Component      | Where set                                       |
|----------------|-------------------------------------------------|
| Relay server   | `--token` flag (or systemd unit file)           |
| Agent (each PC)| Set during `install.ps1` run → stored in registry |
| Manager (laptop)| First argument to `msmgr.exe` → stored in HKCU registry |
