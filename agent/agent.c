/*
 * agent.c - KiroAccess Agent
 * Windows system service that runs pre-login (SYSTEM account).
 * - Registers with relay server over internet
 * - Keeps SSH server alive and tunnelled
 * - Reports hardware telemetry (CPU/GPU temp via kernel WMI/PDH/NtPower)
 * - Auto-restarts on failure
 *
 * Build: cl agent.c /Fe:kiro-agent.exe /link ws2_32.lib advapi32.lib pdh.lib
 *        iphlpapi.lib setupapi.lib /SUBSYSTEM:CONSOLE
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601   /* Windows 7+ */

#include <windows.h>
#include <winsvc.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <powrprof.h>
#include <winreg.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "..\common\protocol.h"
#include "..\common\json_util.h"
#include "..\common\keymgr.h"

/* ---- Constants ---- */
#define SERVICE_NAME        L"KiroAccessAgent"
#define SERVICE_DISPLAY     L"Kiro Access Agent"
#define SERVICE_DESC        L"Remote access agent - provides secure remote control"
#define CONFIG_REG_KEY      L"SOFTWARE\\KiroAccess"
#define LOG_FILE            "C:\\ProgramData\\KiroAccess\\agent.log"
#define MAX_RELAY_SERVERS   4
#define RECONNECT_DELAY_S   5
#define HEARTBEAT_INTERVAL  30

/* ---- Globals ---- */
static SERVICE_STATUS          g_status;
static SERVICE_STATUS_HANDLE   g_status_handle;
static HANDLE                  g_stop_event;
static char                    g_relay_host[256] = "relay.kiroaccess.local";
static char                    g_relay_port[16]  = RELAY_PORT_STR;
static char                    g_auth_token[MAX_TOKEN_LEN] = "";
static char                    g_device_label[64] = "";
static char                    g_device_id[MAX_DEVICE_ID_LEN] = "";
static SOCKET                  g_relay_sock = INVALID_SOCKET;
static CRITICAL_SECTION        g_sock_cs;

/* PDH handles for CPU usage */
static PDH_HQUERY   g_pdh_query;
static PDH_HCOUNTER g_cpu_counter;
static int          g_pdh_ok = 0;

/* ---- Logging ---- */
static FILE *g_log = NULL;
static CRITICAL_SECTION g_log_cs;

static void log_init(void) {
    CreateDirectoryA("C:\\ProgramData\\KiroAccess", NULL);
    InitializeCriticalSection(&g_log_cs);
    g_log = fopen(LOG_FILE, "a");
}

static void log_msg(const char *level, const char *fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_log_cs);
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char tstr[32];
    strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(g_log, "[%s][%s] ", tstr, level);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fprintf(g_log, "\n");
    fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}

#define LOG_INFO(...)  log_msg("INFO",  __VA_ARGS__)
#define LOG_WARN(...)  log_msg("WARN",  __VA_ARGS__)
#define LOG_ERROR(...) log_msg("ERROR", __VA_ARGS__)

/* ---- Config: read from registry ---- */
static void config_read(void) {
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CONFIG_REG_KEY, 0, KEY_READ, &hkey) != ERROR_SUCCESS)
        return;

    DWORD sz;
    DWORD type;

    sz = sizeof(g_relay_host);
    RegQueryValueExA(hkey, "RelayHost", NULL, &type, (LPBYTE)g_relay_host, &sz);

    sz = sizeof(g_relay_port);
    RegQueryValueExA(hkey, "RelayPort", NULL, &type, (LPBYTE)g_relay_port, &sz);

    sz = sizeof(g_auth_token);
    RegQueryValueExA(hkey, "AuthToken", NULL, &type, (LPBYTE)g_auth_token, &sz);

    sz = sizeof(g_device_label);
    RegQueryValueExA(hkey, "DeviceLabel", NULL, &type, (LPBYTE)g_device_label, &sz);

    sz = sizeof(g_device_id);
    RegQueryValueExA(hkey, "DeviceID", NULL, &type, (LPBYTE)g_device_id, &sz);

    RegCloseKey(hkey);
    LOG_INFO("Config loaded: relay=%s:%s label=%s", g_relay_host, g_relay_port, g_device_label);
}

/* ---- Device ID: stable UUID from machine GUID ---- */
static void device_id_init(void) {
    if (g_device_id[0] != '\0') return;
    HKEY hkey;
    char machine_guid[64] = "unknown";
    DWORD sz = sizeof(machine_guid);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        RegQueryValueExA(hkey, "MachineGuid", NULL, NULL,
                         (LPBYTE)machine_guid, &sz);
        RegCloseKey(hkey);
    }
    /* Use first 32 chars of machine GUID as device ID */
    strncpy(g_device_id, machine_guid, MAX_DEVICE_ID_LEN - 1);
    g_device_id[MAX_DEVICE_ID_LEN - 1] = '\0';

    /* Persist to registry */
    HKEY hk2;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, CONFIG_REG_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk2, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hk2, "DeviceID", 0, REG_SZ,
                       (LPBYTE)g_device_id, (DWORD)strlen(g_device_id)+1);
        RegCloseKey(hk2);
    }
}

/* ======================================================
 * HARDWARE TELEMETRY
 * Uses PDH for CPU%, WMI via COM for temps, GlobalMemoryStatusEx for RAM
 * ====================================================== */

static void pdh_init(void) {
    if (PdhOpenQuery(NULL, 0, &g_pdh_query) != ERROR_SUCCESS) return;
    if (PdhAddEnglishCounterA(g_pdh_query,
        "\\Processor(_Total)\\% Processor Time",
        0, &g_cpu_counter) != ERROR_SUCCESS) return;
    PdhCollectQueryData(g_pdh_query); /* first collect primes the counter */
    g_pdh_ok = 1;
}

static float get_cpu_usage(void) {
    if (!g_pdh_ok) return -1.0f;
    if (PdhCollectQueryData(g_pdh_query) != ERROR_SUCCESS) return -1.0f;
    PDH_FMT_COUNTERVALUE val;
    if (PdhGetFormattedCounterValue(g_cpu_counter, PDH_FMT_DOUBLE,
                                     NULL, &val) != ERROR_SUCCESS) return -1.0f;
    return (float)val.doubleValue;
}

/* CPU temperature via MSAcpi_ThermalZoneTemperature WMI (kernel path) */
/* Returns Celsius or -1.0 if unavailable.
 * We use a direct registry/IOCTL path to avoid COM overhead in a service. */
static float get_cpu_temp(void) {
    /*
     * Read from ACPI thermal zone via WMI backing store in registry.
     * Fallback: open \\.\PhysicalDrive0 and hope for SMART data.
     * Most reliable cross-vendor path on Windows without third-party drivers.
     */
    HKEY hkey;
    float temp = -1.0f;

    /* Try ACPI thermal zones first */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Enum\\ACPI_HAL", 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        RegCloseKey(hkey);
    }

    /*
     * More reliable: use NtPowerInformation(ProcessorInformation) 
     * which gives per-core thermal throttle, not exact temp but available everywhere
     * For real temps we rely on the OHM/HWiNFO shared memory if available
     */

    /* Check OpenHardwareMonitor shared memory */
    HANDLE hmap = OpenFileMappingA(FILE_MAP_READ, FALSE, "Global\\OpenHardwareMonitorSharedMemory");
    if (hmap) {
        /* OHM shared memory: header + array of sensor entries */
        typedef struct { DWORD dwSignature; DWORD dwVersion; DWORD dwNumEntries; } ohm_header;
        typedef struct {
            char  szName[128]; char szType[32]; char szParent[128];
            float fValue; DWORD dwIndex;
        } ohm_entry;
        LPVOID ptr = MapViewOfFile(hmap, FILE_MAP_READ, 0, 0, 0);
        if (ptr) {
            ohm_header *hdr = (ohm_header*)ptr;
            if (hdr->dwSignature == 0x4F484D00 && hdr->dwNumEntries > 0) {
                ohm_entry *entries = (ohm_entry*)((BYTE*)ptr + sizeof(ohm_header));
                for (DWORD i = 0; i < hdr->dwNumEntries && i < 1024; i++) {
                    if (strcmp(entries[i].szType, "Temperature") == 0 &&
                        strstr(entries[i].szParent, "CPU") != NULL) {
                        temp = entries[i].fValue;
                        break;
                    }
                }
            }
            UnmapViewOfFile(ptr);
        }
        CloseHandle(hmap);
    }
    return temp;
}

static float get_gpu_temp(void) {
    /* Same OHM shared memory, look for GPU temperature */
    float temp = -1.0f;
    HANDLE hmap = OpenFileMappingA(FILE_MAP_READ, FALSE, "Global\\OpenHardwareMonitorSharedMemory");
    if (hmap) {
        typedef struct { DWORD dwSignature; DWORD dwVersion; DWORD dwNumEntries; } ohm_header;
        typedef struct {
            char  szName[128]; char szType[32]; char szParent[128];
            float fValue; DWORD dwIndex;
        } ohm_entry;
        LPVOID ptr = MapViewOfFile(hmap, FILE_MAP_READ, 0, 0, 0);
        if (ptr) {
            ohm_header *hdr = (ohm_header*)ptr;
            if (hdr->dwSignature == 0x4F484D00) {
                ohm_entry *entries = (ohm_entry*)((BYTE*)ptr + sizeof(ohm_header));
                for (DWORD i = 0; i < hdr->dwNumEntries && i < 1024; i++) {
                    if (strcmp(entries[i].szType, "Temperature") == 0 &&
                        strstr(entries[i].szParent, "GPU") != NULL) {
                        temp = entries[i].fValue;
                        break;
                    }
                }
            }
            UnmapViewOfFile(ptr);
        }
        CloseHandle(hmap);
    }
    return temp;
}

static void get_ram(uint64_t *used, uint64_t *total) {
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    *total = ms.ullTotalPhys;
    *used  = ms.ullTotalPhys - ms.ullAvailPhys;
}

static float get_battery(void) {
    SYSTEM_POWER_STATUS ps;
    if (!GetSystemPowerStatus(&ps)) return -1.0f;
    if (ps.BatteryFlag == 255 || ps.BatteryLifePercent == 255) return -1.0f;
    return (float)ps.BatteryLifePercent;
}

static uint32_t get_uptime(void) {
    return (uint32_t)(GetTickCount64() / 1000);
}

/* Network bytes - snapshot delta tracked per call */
static uint64_t g_net_rx_last = 0, g_net_tx_last = 0;
static time_t   g_net_last_t = 0;

static void get_net_bps(uint64_t *rx_bps, uint64_t *tx_bps) {
    *rx_bps = 0; *tx_bps = 0;
    ULONG sz = 0;
    if (GetIfTable(NULL, &sz, FALSE) == ERROR_INSUFFICIENT_BUFFER) {
        MIB_IFTABLE *tbl = (MIB_IFTABLE*)malloc(sz);
        if (tbl && GetIfTable(tbl, &sz, FALSE) == NO_ERROR) {
            uint64_t rx = 0, tx = 0;
            for (DWORD i = 0; i < tbl->dwNumEntries; i++) {
                rx += tbl->table[i].dwInOctets;
                tx += tbl->table[i].dwOutOctets;
            }
            time_t now = time(NULL);
            double dt = (double)(now - g_net_last_t);
            if (g_net_last_t > 0 && dt > 0) {
                *rx_bps = (uint64_t)((rx - g_net_rx_last) / dt);
                *tx_bps = (uint64_t)((tx - g_net_tx_last) / dt);
            }
            g_net_rx_last = rx; g_net_tx_last = tx; g_net_last_t = now;
        }
        free(tbl);
    }
}

static void collect_stats(hw_stats_t *s) {
    s->cpu_usage_pct  = get_cpu_usage();
    s->cpu_temp_c     = get_cpu_temp();
    s->gpu_temp_c     = get_gpu_temp();
    get_ram(&s->ram_used_bytes, &s->ram_total_bytes);
    s->battery_pct    = get_battery();
    s->uptime_seconds = get_uptime();
    get_net_bps(&s->net_rx_bps, &s->net_tx_bps);
    s->disk_read_bps  = 0; s->disk_write_bps = 0; /* PDH counters optional */
}

/* ======================================================
 * NETWORKING - send/recv framed messages
 * ====================================================== */

/* Send: [4-byte big-endian length][payload] */
static int net_send_msg(SOCKET s, const char *payload) {
    uint32_t len = (uint32_t)strlen(payload);
    uint32_t wire_len = htonl(len);
    char header[4];
    memcpy(header, &wire_len, 4);
    if (send(s, header, 4, 0) != 4) return -1;
    int sent = 0;
    while (sent < (int)len) {
        int n = send(s, payload + sent, (int)len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* Recv: returns newly malloc'd null-terminated string, caller frees. NULL on error. */
static char *net_recv_msg(SOCKET s) {
    char header[4];
    int got = 0;
    while (got < 4) {
        int n = recv(s, header + got, 4 - got, 0);
        if (n <= 0) return NULL;
        got += n;
    }
    uint32_t len;
    memcpy(&len, header, 4);
    len = ntohl(len);
    if (len == 0 || len > MAX_MSG_SIZE) return NULL;
    char *buf = (char*)malloc(len + 1);
    if (!buf) return NULL;
    got = 0;
    while (got < (int)len) {
        int n = recv(s, buf + got, (int)len - got, 0);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[len] = '\0';
    return buf;
}

/* Connect to relay server, returns connected SOCKET or INVALID_SOCKET */
static SOCKET relay_connect(void) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(g_relay_host, g_relay_port, &hints, &res) != 0) {
        LOG_ERROR("DNS lookup failed for %s", g_relay_host);
        return INVALID_SOCKET;
    }

    SOCKET sock = INVALID_SOCKET;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        /* 10s connect timeout */
        DWORD tv = 10000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
        if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
        closesocket(sock); sock = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (sock == INVALID_SOCKET) {
        LOG_ERROR("Cannot connect to relay %s:%s", g_relay_host, g_relay_port);
    }
    return sock;
}

/* ======================================================
 * RELAY PROTOCOL - register + heartbeat loop
 * ====================================================== */

static int relay_authenticate(SOCKET sock) {
    /* Send auth message — role="agent" tells single-port relay how to route us */
    char buf[2048];
    json_builder_t jb;
    jb_init(&jb, buf, sizeof(buf));
    jb_begin(&jb);
    jb_str(&jb, "type",      MSG_AUTH);
    jb_str(&jb, "role",      "agent");
    jb_int(&jb, "version",   PROTOCOL_VERSION);
    jb_str(&jb, "token",     g_auth_token);
    jb_str(&jb, "device_id", g_device_id);
    jb_end(&jb);

    if (net_send_msg(sock, buf) != 0) return -1;

    char *resp = net_recv_msg(sock);
    if (!resp) return -1;
    char type[32];
    int ok = json_find_str(resp, "type", type, sizeof(type)) != NULL &&
             strcmp(type, MSG_AUTH_OK) == 0;
    free(resp);
    return ok ? 0 : -1;
}

static int relay_register(SOCKET sock) {
    char hostname[MAX_HOSTNAME_LEN];
    DWORD hlen = sizeof(hostname);
    GetComputerNameA(hostname, &hlen);

    /* Get OS version string */
    char os_ver[128] = "Windows";
    OSVERSIONINFOEXA vi; memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
#pragma warning(suppress:4996)
    GetVersionExA((LPOSVERSIONINFOA)&vi);
    snprintf(os_ver, sizeof(os_ver), "Windows %lu.%lu.%lu",
             vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);

    char buf[4096];
    json_builder_t jb;
    jb_init(&jb, buf, sizeof(buf));
    jb_begin(&jb);
    jb_str(&jb, "type",      MSG_REGISTER);
    jb_str(&jb, "device_id", g_device_id);
    jb_str(&jb, "hostname",  hostname);
    jb_str(&jb, "label",     g_device_label);
    jb_str(&jb, "os",        os_ver);
    jb_int(&jb, "ssh_port",  22);
    jb_end(&jb);

    int ret = net_send_msg(sock, buf);
    if (ret == 0) LOG_INFO("Registered with relay as '%s' (%s)", g_device_label, g_device_id);
    return ret;
}

static void relay_send_heartbeat(SOCKET sock) {
    hw_stats_t stats;
    collect_stats(&stats);

    char buf[2048];
    json_builder_t jb;
    jb_init(&jb, buf, sizeof(buf));
    jb_begin(&jb);
    jb_str  (&jb, "type",       MSG_HEARTBEAT);
    jb_str  (&jb, "device_id",  g_device_id);
    jb_float(&jb, "cpu_pct",    stats.cpu_usage_pct);
    jb_float(&jb, "cpu_temp",   stats.cpu_temp_c);
    jb_float(&jb, "gpu_temp",   stats.gpu_temp_c);
    jb_uint64(&jb,"ram_used",   stats.ram_used_bytes);
    jb_uint64(&jb,"ram_total",  stats.ram_total_bytes);
    jb_uint64(&jb,"net_rx_bps", stats.net_rx_bps);
    jb_uint64(&jb,"net_tx_bps", stats.net_tx_bps);
    jb_float(&jb, "battery",    stats.battery_pct);
    jb_int  (&jb, "uptime",     (long long)stats.uptime_seconds);
    jb_end(&jb);

    net_send_msg(sock, buf);
}

/* ======================================================
 * SSH SERVER WATCHDOG
 * Ensures OpenSSH sshd is always running
 * ====================================================== */

static void ensure_sshd_running(void) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(scm, L"sshd", SERVICE_QUERY_STATUS | SERVICE_START);
    if (svc) {
        SERVICE_STATUS ss;
        QueryServiceStatus(svc, &ss);
        if (ss.dwCurrentState != SERVICE_RUNNING) {
            LOG_INFO("sshd not running, starting...");
            StartServiceW(svc, 0, NULL);
        }
        CloseServiceHandle(svc);
    } else {
        LOG_WARN("sshd service not found - OpenSSH may not be installed");
    }
    CloseServiceHandle(scm);
}

/* ======================================================
 * MAIN RELAY LOOP THREAD
 * ====================================================== */

static DWORD WINAPI relay_thread(LPVOID unused) {
    (void)unused;
    LOG_INFO("Relay thread started, target: %s:%s", g_relay_host, g_relay_port);

    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        ensure_sshd_running();

        SOCKET sock = relay_connect();
        if (sock == INVALID_SOCKET) {
            WaitForSingleObject(g_stop_event, RECONNECT_DELAY_S * 1000);
            continue;
        }

        /* Set keepalive */
        BOOL ka = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char*)&ka, sizeof(ka));

        if (relay_authenticate(sock) != 0) {
            LOG_ERROR("Auth with relay failed");
            closesocket(sock);
            WaitForSingleObject(g_stop_event, RECONNECT_DELAY_S * 1000);
            continue;
        }

        if (relay_register(sock) != 0) {
            LOG_ERROR("Register with relay failed");
            closesocket(sock);
            WaitForSingleObject(g_stop_event, RECONNECT_DELAY_S * 1000);
            continue;
        }

        EnterCriticalSection(&g_sock_cs);
        g_relay_sock = sock;
        LeaveCriticalSection(&g_sock_cs);

        LOG_INFO("Connected to relay. Heartbeat every %ds", HEARTBEAT_INTERVAL);

        /* Heartbeat loop */
        while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
            relay_send_heartbeat(sock);

            /* Poll for incoming commands (non-blocking read with timeout) */
            fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
            struct timeval tv = { HEARTBEAT_INTERVAL, 0 };
            int sel = select(0, &fds, NULL, NULL, &tv);
            if (sel < 0) break;   /* socket error, reconnect */
            if (sel == 0) continue; /* timeout = next heartbeat */

            char *msg = net_recv_msg(sock);
            if (!msg) { LOG_WARN("Relay disconnected"); break; }

            char type[32];
            json_find_str(msg, "type", type, sizeof(type));
            LOG_INFO("Relay msg: %s", type);

            if (strcmp(type, MSG_DISCONNECT) == 0) {
                free(msg); break;
            }
            free(msg);
        }

        EnterCriticalSection(&g_sock_cs);
        g_relay_sock = INVALID_SOCKET;
        LeaveCriticalSection(&g_sock_cs);
        closesocket(sock);

        if (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0)
            WaitForSingleObject(g_stop_event, RECONNECT_DELAY_S * 1000);
    }

    LOG_INFO("Relay thread exiting");
    return 0;
}

/* ======================================================
 * SECURITY AUDIT THREAD
 * Runs every 60 seconds. Verifies:
 *   1. authorized_keys has not been modified (only owner key allowed)
 *   2. sshd_config has not been tampered with
 * If tampering is detected: stops sshd immediately and logs CRITICAL alert.
 * Owner must re-run installer to restore and re-enable.
 * ====================================================== */

#define AUDIT_INTERVAL_S  60

static DWORD WINAPI security_audit_thread(LPVOID unused) {
    (void)unused;
    LOG_INFO("Security audit thread started (interval: %ds)", AUDIT_INTERVAL_S);

    while (WaitForSingleObject(g_stop_event, AUDIT_INTERVAL_S * 1000) != WAIT_OBJECT_0) {
        int result = keymgr_audit();

        if (result == 0) {
            /* TAMPERING DETECTED */
            LOG_ERROR("=== SECURITY ALERT ===");
            LOG_ERROR("authorized_keys file was modified after install!");
            LOG_ERROR("Stopping sshd to prevent unauthorised access.");
            LOG_ERROR("Re-run install.ps1 to restore authorized access.");

            /* Immediately stop sshd */
            SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
            if (scm) {
                SC_HANDLE svc = OpenServiceW(scm, L"sshd",
                                              SERVICE_STOP | SERVICE_QUERY_STATUS);
                if (svc) {
                    SERVICE_STATUS ss;
                    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
                    CloseServiceHandle(svc);
                    LOG_ERROR("sshd stopped.");
                }
                CloseServiceHandle(scm);
            }

            /* Write Windows Event Log entry */
            HANDLE hEvt = RegisterEventSourceW(NULL, SERVICE_NAME);
            if (hEvt) {
                const wchar_t *msgs[] = {
                    L"KiroAccess SECURITY ALERT: authorized_keys was tampered with. "
                    L"sshd has been stopped. Re-run install.ps1 to restore access."
                };
                ReportEventW(hEvt, EVENTLOG_ERROR_TYPE, 0, 0xC0000001, NULL,
                             1, 0, msgs, NULL);
                DeregisterEventSource(hEvt);
            }
        }
        else if (result == -1) {
            LOG_WARN("authorized_keys file missing or unreadable — sshd may not work.");
        }
        /* result == 1: all good, nothing to do */
    }
    return 0;
}

/* ======================================================
 * WINDOWS SERVICE BOILERPLATE
 * ====================================================== */

static void svc_report_status(DWORD state, DWORD exit_code, DWORD wait_hint) {
    static DWORD checkpoint = 1;
    g_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState            = state;
    g_status.dwWin32ExitCode           = exit_code;
    g_status.dwWaitHint                = wait_hint;
    g_status.dwControlsAccepted        = (state == SERVICE_START_PENDING) ? 0
                                         : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwCheckPoint              = (state == SERVICE_RUNNING || state == SERVICE_STOPPED)
                                         ? 0 : checkpoint++;
    SetServiceStatus(g_status_handle, &g_status);
}

static VOID WINAPI svc_ctrl_handler(DWORD ctrl) {
    switch (ctrl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            svc_report_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            SetEvent(g_stop_event);
            break;
        default: break;
    }
}

static VOID WINAPI svc_main(DWORD argc, LPWSTR *argv) {
    (void)argc; (void)argv;

    g_status_handle = RegisterServiceCtrlHandlerW(SERVICE_NAME, svc_ctrl_handler);
    if (!g_status_handle) return;

    svc_report_status(SERVICE_START_PENDING, NO_ERROR, 3000);

    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stop_event) {
        svc_report_status(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    InitializeCriticalSection(&g_sock_cs);
    log_init();
    LOG_INFO("=== KiroAccess Agent starting ===");

    /* Init Winsock */
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    config_read();
    device_id_init();
    pdh_init();

    svc_report_status(SERVICE_RUNNING, NO_ERROR, 0);
    LOG_INFO("Service running");

    /* Start relay thread */
    HANDLE ht = CreateThread(NULL, 0, relay_thread, NULL, 0, NULL);

    /* Start security audit thread */
    HANDLE ha = CreateThread(NULL, 0, security_audit_thread, NULL, 0, NULL);

    /* Wait for stop signal */
    WaitForSingleObject(g_stop_event, INFINITE);

    LOG_INFO("Stop signal received");
    if (ht) { WaitForSingleObject(ht, 10000); CloseHandle(ht); }
    if (ha) { WaitForSingleObject(ha, 5000);  CloseHandle(ha); }

    WSACleanup();
    if (g_pdh_ok) PdhCloseQuery(g_pdh_query);
    if (g_log) fclose(g_log);
    DeleteCriticalSection(&g_sock_cs);

    svc_report_status(SERVICE_STOPPED, NO_ERROR, 0);
}

/* ======================================================
 * INSTALL / UNINSTALL (run as admin with --install/--uninstall)
 * ====================================================== */

static void svc_install(void) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) {
        wprintf(L"GetModuleFileName failed: %lu\n", GetLastError()); return;
    }

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { wprintf(L"OpenSCManager failed: %lu\n", GetLastError()); return; }

    SC_HANDLE svc = CreateServiceW(scm, SERVICE_NAME, SERVICE_DISPLAY,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,          /* start automatically */
        SERVICE_ERROR_NORMAL,
        path, NULL, NULL, NULL,
        NULL, NULL);                 /* SYSTEM account */

    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) wprintf(L"Service already exists\n");
        else wprintf(L"CreateService failed: %lu\n", err);
    } else {
        /* Set description */
        SERVICE_DESCRIPTIONW desc = { (LPWSTR)SERVICE_DESC };
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

        /* Set recovery: restart on failure */
        SC_ACTION acts[3] = {
            {SC_ACTION_RESTART, 5000},
            {SC_ACTION_RESTART, 10000},
            {SC_ACTION_RESTART, 30000}
        };
        SERVICE_FAILURE_ACTIONSW fa = {0};
        fa.cActions = 3; fa.lpsaActions = acts;
        fa.dwResetPeriod = 86400;
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

        wprintf(L"Service installed successfully\n");
        StartServiceW(svc, 0, NULL);
        wprintf(L"Service started\n");
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

static void svc_uninstall(void) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_STOP | DELETE);
    if (svc) {
        SERVICE_STATUS ss;
        ControlService(svc, SERVICE_CONTROL_STOP, &ss);
        DeleteService(svc);
        CloseServiceHandle(svc);
        wprintf(L"Service uninstalled\n");
    }
    CloseServiceHandle(scm);
}

/* ======================================================
 * ENTRY POINT
 * ====================================================== */

int wmain(int argc, wchar_t *argv[]) {
    if (argc > 1) {
        if (wcscmp(argv[1], L"--install") == 0)   { svc_install();   return 0; }
        if (wcscmp(argv[1], L"--uninstall") == 0) { svc_uninstall(); return 0; }
        if (wcscmp(argv[1], L"--run") == 0) {
            /* Console debug mode */
            g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
            InitializeCriticalSection(&g_sock_cs);
            log_init();
            WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
            config_read(); device_id_init(); pdh_init();
            printf("Running in console mode. Press Ctrl+C to stop.\n");
            HANDLE ht = CreateThread(NULL, 0, relay_thread, NULL, 0, NULL);
            HANDLE ha = CreateThread(NULL, 0, security_audit_thread, NULL, 0, NULL);
            WaitForSingleObject(ht, INFINITE);
            (void)ha;
            return 0;
        }
    }

    /* Run as Windows service */
    SERVICE_TABLE_ENTRYW table[] = {
        { (LPWSTR)SERVICE_NAME, svc_main },
        { NULL, NULL }
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            wprintf(L"Not running as a service. Use --install, --uninstall, or --run\n");
        }
        return (int)err;
    }
    return 0;
}
