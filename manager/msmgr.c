/*
 * manager.c - Mass Manager (TUI)
 *
 * Terminal UI for your laptop. Shows all registered devices anywhere on the
 * internet, live hardware stats, and lets you:
 *   [Enter]   SSH into selected device
 *   [F]       File browser (SCP/SFTP)
 *   [R]       RDP/screen-share (launches mstsc or VNC)
 *   [T]       Open new SSH tab/window
 *   [A]       Add auth token / relay server
 *   [D]       Forget device
 *   [Q]       Quit
 *
 * Dual discovery: Internet (relay) + LAN mDNS broadcast simultaneously.
 *
 * Build: cl manager.c /Fe:msmgr.exe /I..\common
 *        /link ws2_32.lib advapi32.lib /SUBSYSTEM:CONSOLE
 *
 * Or gcc: gcc manager.c -I../common -O2 -o msmgr.exe -lws2_32 -ladvapi32
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <winhttp.h>
#include <winreg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>
#include <ctype.h>

#include "..\common\protocol.h"
#include "..\common\json_util.h"
#include "..\common\ws.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "iphlpapi.lib")

/* ── Config ────────────────────────────────────────────────────────────────── */
#define CONFIG_REG_KEY      "SOFTWARE\\Mass\\Manager"
#define MAX_KNOWN_RELAYS    8
#define MAX_DEVICES_UI      64
#define REFRESH_INTERVAL_MS 5000
#define LAN_DISCOVERY_PORT  7745
#define LAN_BEACON_INTERVAL 10000

/* Forward declaration — defined in config section below */
static const char *find_key_for_device(const char *label);

/* ── Device as seen by manager ─────────────────────────────────────────────── */
typedef struct {
    int      valid;
    char     device_id[MAX_DEVICE_ID_LEN];
    char     label[64];
    char     hostname[MAX_HOSTNAME_LEN];
    char     os[128];
    char     ip[48];
    uint16_t ssh_port;
    int      online;
    time_t   last_seen;
    /* Stats */
    float    cpu_pct;
    float    cpu_temp;
    float    gpu_temp;
    uint64_t ram_used;
    uint64_t ram_total;
    uint64_t net_rx_bps;
    uint64_t net_tx_bps;
    float    battery;
    uint32_t uptime;
    /* Source */
    int      via_lan;    /* 1 = found on LAN, 0 = found via relay */
} ui_device_t;

/* ── Relay config ──────────────────────────────────────────────────────────── */
typedef struct {
    char host[256];
    char port[16];
    char token[MAX_TOKEN_LEN];
} relay_cfg_t;

/* ── Global state ──────────────────────────────────────────────────────────── */
static ui_device_t  g_devices[MAX_DEVICES_UI];
static int          g_device_count  = 0;
static CRITICAL_SECTION g_dev_cs;

static relay_cfg_t  g_relays[MAX_KNOWN_RELAYS];
static int          g_relay_count   = 0;

static int          g_selected      = 0;
static int          g_scroll_offset = 0;
static volatile int g_running       = 1;
static HANDLE       g_refresh_event;

static HANDLE       g_console_out;
static HANDLE       g_console_in;
static CONSOLE_SCREEN_BUFFER_INFO g_csbi;
static int          g_width  = 120;
static int          g_height = 30;

/* ============================================================
 * CONSOLE / TUI RENDERING
 * ============================================================ */

/* ANSI/VT100 colour codes — enabled via SetConsoleMode ENABLE_VIRTUAL_TERMINAL_PROCESSING */
#define COL_RESET       "\033[0m"
#define COL_BOLD        "\033[1m"
#define COL_DIM         "\033[2m"
#define COL_RED         "\033[31m"
#define COL_GREEN       "\033[32m"
#define COL_YELLOW      "\033[33m"
#define COL_BLUE        "\033[34m"
#define COL_MAGENTA     "\033[35m"
#define COL_CYAN        "\033[36m"
#define COL_WHITE       "\033[37m"
#define COL_BRIGHT_GREEN "\033[92m"
#define COL_BRIGHT_RED   "\033[91m"
#define COL_BG_BLUE     "\033[44m"
#define COL_BG_RESET    "\033[49m"
#define COL_BG_SEL      "\033[48;5;236m"   /* dark grey bg for selected row */

static void con_init(void) {
    g_console_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_console_in  = GetStdHandle(STD_INPUT_HANDLE);

    /* Enable VT processing */
    DWORD mode = 0;
    GetConsoleMode(g_console_out, &mode);
    SetConsoleMode(g_console_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                                       | DISABLE_NEWLINE_AUTO_RETURN);

    /* Raw input */
    GetConsoleMode(g_console_in, &mode);
    SetConsoleMode(g_console_in, ENABLE_VIRTUAL_TERMINAL_INPUT
                                | ENABLE_WINDOW_INPUT);

    /* Get terminal size */
    GetConsoleScreenBufferInfo(g_console_out, &g_csbi);
    g_width  = g_csbi.srWindow.Right  - g_csbi.srWindow.Left + 1;
    g_height = g_csbi.srWindow.Bottom - g_csbi.srWindow.Top  + 1;
    if (g_width  < 80)  g_width  = 80;
    if (g_height < 20)  g_height = 20;
}

static void con_cls(void) { printf("\033[2J\033[H"); }
static void con_move(int row, int col) { printf("\033[%d;%dH", row, col); }
static void con_hide_cursor(void) { printf("\033[?25l"); }
static void con_show_cursor(void) { printf("\033[?25h"); }

/* Print a string padded/truncated to exactly `width` chars */
static void print_fixed(const char *s, int width) {
    int len = (int)strlen(s);
    if (len >= width) {
        printf("%.*s", width, s);
    } else {
        printf("%s%*s", s, width - len, "");
    }
}

/* Human-readable bytes */
static void fmt_bytes(uint64_t b, char *out, size_t cap) {
    if      (b >= 1073741824ULL) snprintf(out, cap, "%.1fG", b / 1073741824.0);
    else if (b >= 1048576ULL)    snprintf(out, cap, "%.1fM", b / 1048576.0);
    else if (b >= 1024ULL)       snprintf(out, cap, "%.1fK", b / 1024.0);
    else                         snprintf(out, cap, "%lluB", (unsigned long long)b);
}

/* Human-readable uptime */
static void fmt_uptime(uint32_t secs, char *out, size_t cap) {
    uint32_t d = secs/86400, h = (secs%86400)/3600, m = (secs%3600)/60;
    if (d > 0)      snprintf(out, cap, "%ud%02uh",  d, h);
    else if (h > 0) snprintf(out, cap, "%uh%02um",  h, m);
    else            snprintf(out, cap, "%um",        m);
}

/* Bar graph [████░░░░] for a 0-100 percent value */
static void fmt_bar(float pct, int width, char *out) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (int)(pct / 100.0f * width);
    int i = 0;
    out[i++] = '[';
    for (int j = 0; j < width; j++)
        out[i++] = j < filled ? '\xDB' : '\xB0';   /* ASCII block chars */
    out[i++] = ']';
    out[i] = '\0';
}

/* ── Draw the full UI ──────────────────────────────────────────────────────── */
static void ui_draw(void) {
    con_cls();
    con_hide_cursor();

    int list_rows = g_height - 12;  /* rows available for device list */
    if (list_rows < 4) list_rows = 4;

    /* ── Header ── */
    printf(COL_BOLD COL_CYAN);
    printf("  Mass Manager");
    printf(COL_DIM "  %d device(s) known", g_device_count);
    printf(COL_RESET "\n");
    printf(COL_DIM);
    for (int i = 0; i < g_width - 1; i++) printf("\xC4"); /* ─ */
    printf(COL_RESET "\n");

    /* ── Column headers ── */
    printf(COL_BOLD COL_WHITE);
    printf("  %-3s  %-20s  %-14s  %-8s  %-12s  %-12s  %-10s  %-7s  %s\n",
           "#", "Label / Hostname", "IP : Port", "CPU%", "CPU/GPU °C",
           "RAM Used", "Net RX/TX", "Battery", "Status");
    printf(COL_DIM);
    for (int i = 0; i < g_width - 1; i++) printf("\xC4");
    printf(COL_RESET "\n");

    /* ── Device rows ── */
    EnterCriticalSection(&g_dev_cs);
    int shown = 0;
    for (int i = g_scroll_offset; i < g_device_count && shown < list_rows; i++, shown++) {
        ui_device_t *d = &g_devices[i];
        int selected = (i == g_selected);

        if (selected) printf(COL_BG_SEL COL_BOLD);

        /* Status dot */
        if (d->online) printf(COL_BRIGHT_GREEN "  ● " COL_RESET);
        else           printf(COL_BRIGHT_RED   "  ○ " COL_RESET);
        if (selected)  printf(COL_BG_SEL);

        /* Row number + label */
        char label_host[40];
        snprintf(label_host, sizeof(label_host), "%s / %s", d->label, d->hostname);
        printf("%-2d  ", i + 1);
        print_fixed(label_host, 20);
        printf("  ");

        /* IP:port */
        char ip_port[24];
        snprintf(ip_port, sizeof(ip_port), "%s:%u", d->ip, d->ssh_port);
        print_fixed(ip_port, 14);
        printf("  ");

        /* CPU% */
        if (d->online && d->cpu_pct >= 0) {
            const char *col = d->cpu_pct > 80 ? COL_RED :
                              d->cpu_pct > 50 ? COL_YELLOW : COL_GREEN;
            printf("%s%-6.1f%%  " COL_RESET, col, d->cpu_pct);
        } else {
            printf("%-8s  ", "N/A");
        }

        /* CPU/GPU temps */
        if (d->online && d->cpu_temp > 0) {
            const char *cc = d->cpu_temp > 85 ? COL_RED :
                             d->cpu_temp > 70 ? COL_YELLOW : COL_GREEN;
            printf("%s%.0f°C" COL_RESET, cc, d->cpu_temp);
            if (d->gpu_temp > 0) {
                const char *gc = d->gpu_temp > 85 ? COL_RED :
                                 d->gpu_temp > 70 ? COL_YELLOW : COL_CYAN;
                printf("/%s%.0f°C" COL_RESET, gc, d->gpu_temp);
            } else printf("/N/A");
        } else printf("%-12s", "N/A");
        printf("  ");

        /* RAM */
        if (d->online && d->ram_total > 0) {
            char ru[16], rt[16];
            fmt_bytes(d->ram_used,  ru, sizeof(ru));
            fmt_bytes(d->ram_total, rt, sizeof(rt));
            char ram_str[24];
            snprintf(ram_str, sizeof(ram_str), "%s/%s", ru, rt);
            print_fixed(ram_str, 12);
        } else print_fixed("N/A", 12);
        printf("  ");

        /* Net RX/TX */
        if (d->online) {
            char rx[16], tx[16];
            fmt_bytes(d->net_rx_bps, rx, sizeof(rx));
            fmt_bytes(d->net_tx_bps, tx, sizeof(tx));
            char net_str[24];
            snprintf(net_str, sizeof(net_str), "%s/%s", rx, tx);
            print_fixed(net_str, 10);
        } else print_fixed("N/A", 10);
        printf("  ");

        /* Battery */
        if (d->battery >= 0) {
            const char *bc = d->battery < 20 ? COL_RED :
                             d->battery < 50 ? COL_YELLOW : COL_GREEN;
            printf("%s%.0f%%%s", bc, d->battery, COL_RESET);
        } else printf("  AC  ");
        printf("  ");

        /* Via LAN indicator */
        if (d->via_lan) printf(COL_DIM "[LAN]" COL_RESET);
        else             printf(COL_DIM "[NET]" COL_RESET);

        printf(COL_RESET "\n");
    }
    LeaveCriticalSection(&g_dev_cs);

    /* Empty state */
    if (g_device_count == 0) {
        printf("\n  " COL_DIM "No devices found. Waiting for connections...\n"
               "  Check relay server address and auth token (press A to configure)."
               COL_RESET "\n");
    }

    /* ── Separator ── */
    printf(COL_DIM);
    for (int i = 0; i < g_width - 1; i++) printf("\xC4");
    printf(COL_RESET "\n");

    /* ── Detail panel for selected device ── */
    EnterCriticalSection(&g_dev_cs);
    if (g_selected >= 0 && g_selected < g_device_count) {
        ui_device_t *d = &g_devices[g_selected];
        char uptime_str[32] = "N/A";
        if (d->uptime > 0) fmt_uptime(d->uptime, uptime_str, sizeof(uptime_str));

        printf("  " COL_BOLD "%s" COL_RESET " — %s  |  %s  |  Uptime: %s\n",
               d->label, d->os, d->online ? COL_GREEN "ONLINE" COL_RESET
                                          : COL_RED "OFFLINE" COL_RESET,
               uptime_str);

        /* CPU bar */
        if (d->online && d->cpu_pct >= 0) {
            char bar[24]; fmt_bar(d->cpu_pct, 16, bar);
            float ram_pct = d->ram_total > 0
                          ? (float)d->ram_used / d->ram_total * 100.f : 0;
            char rbar[24]; fmt_bar(ram_pct, 16, rbar);
            printf("  CPU %s %.1f%%  RAM %s %.0f%%\n",
                   bar, d->cpu_pct, rbar, ram_pct);
        }
    }
    LeaveCriticalSection(&g_dev_cs);

    /* ── Keybind footer ── */
    printf(COL_DIM);
    for (int i = 0; i < g_width - 1; i++) printf("\xC4");
    printf(COL_RESET "\n");
    printf("  " COL_BOLD "[↑↓]" COL_RESET " Navigate  "
                  COL_BOLD "[Enter]" COL_RESET " SSH  "
                  COL_BOLD "[F]" COL_RESET " Files  "
                  COL_BOLD "[R]" COL_RESET " RDP  "
                  COL_BOLD "[T]" COL_RESET " New window  "
                  COL_BOLD "[U]" COL_RESET " Update device  "
                  COL_BOLD "[Ctrl+U]" COL_RESET " Update ALL  "
                  COL_BOLD "[A]" COL_RESET " Add relay  "
                  COL_BOLD "[Q]" COL_RESET " Quit\n");
    fflush(stdout);
}

/* ============================================================
 * NETWORKING: WebSocket relay client
 * Connects to port 443 (Render's public HTTPS port).
 * Render terminates TLS at its edge and forwards plain WS bytes to container.
 * Cold-start: polls GET /health until 200 before upgrading to WebSocket.
 * ============================================================ */

static int net_send_msg(SOCKET s, const char *payload) {
    return ws_send_msg((ws_sock_t)s, payload, 1 /* client = masked */);
}

static char *net_recv_msg(SOCKET s) {
    return ws_recv_msg((ws_sock_t)s);
}

/* Wake up Render free-tier — polls /health until the service responds */
static void relay_wakeup(const char *host) {
    char req[512];
    snprintf(req, sizeof(req),
        "GET /health HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);

    for (int attempt = 1; attempt <= 30 && g_running; attempt++) {
        /* Try port 80 first (Render redirects), then 443 */
        const char *ports[] = {"80", "443", NULL};
        int awake = 0;
        for (int pi = 0; ports[pi] && !awake; pi++) {
            struct addrinfo hints, *res;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host, ports[pi], &hints, &res) != 0) continue;
            SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (s != INVALID_SOCKET) {
                DWORD tv = 5000;
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
                setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
                if (connect(s, res->ai_addr, (int)res->ai_addrlen) == 0) {
                    send(s, req, (int)strlen(req), 0);
                    char resp[256] = {0};
                    recv(s, resp, sizeof(resp)-1, 0);
                    if (strstr(resp, "200") || strstr(resp, "301") || strstr(resp, "101"))
                        awake = 1;
                }
                closesocket(s);
            }
            freeaddrinfo(res);
        }
        if (awake) return;
        if (attempt < 30) Sleep(3000);
    }
}

/* Connect to relay on port 443 and perform WebSocket upgrade */
static SOCKET relay_connect_and_auth(relay_cfg_t *cfg) {
    /* Wake up the Render free-tier service if it spun down */
    relay_wakeup(cfg->host);
    if (!g_running) return INVALID_SOCKET;

    /* TCP connect to port 443 */
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(cfg->host, "443", &hints, &res) != 0) return INVALID_SOCKET;

    SOCKET s = INVALID_SOCKET;
    for (rp = res; rp; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        DWORD tv = 15000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
        if (connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
        closesocket(s); s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    /* WebSocket handshake */
    if (ws_client_handshake((ws_sock_t)s, cfg->host, "/ms") != 0) {
        closesocket(s); return INVALID_SOCKET;
    }

    /* Send auth message */
    char buf[512];
    json_builder_t jb; jb_init(&jb, buf, sizeof(buf));
    jb_begin(&jb);
    jb_str(&jb, "type",  MSG_AUTH);
    jb_str(&jb, "role",  "manager");
    jb_str(&jb, "token", cfg->token);
    jb_end(&jb);
    if (net_send_msg(s, buf) != 0) { closesocket(s); return INVALID_SOCKET; }

    /* Expect auth_ok */
    char *resp = net_recv_msg(s);
    if (!resp) { closesocket(s); return INVALID_SOCKET; }
    char type[32];
    json_find_str(resp, "type", type, sizeof(type));
    free(resp);
    if (strcmp(type, MSG_AUTH_OK) != 0) { closesocket(s); return INVALID_SOCKET; }

    return s;
}

/* Parse device_list JSON and merge into g_devices */
static void parse_and_merge_devices(const char *json, int via_lan) {
    /* Walk through the "devices":[...] array manually */
    const char *arr = strstr(json, "\"devices\":[");
    if (!arr) return;
    arr += strlen("\"devices\":[");

    EnterCriticalSection(&g_dev_cs);

    const char *p = arr;
    while (*p && *p != ']') {
        /* Find next object */
        const char *obj_start = strchr(p, '{');
        if (!obj_start) break;
        /* Find matching } (handles no nesting) */
        const char *obj_end = strchr(obj_start + 1, '}');
        if (!obj_end) break;

        /* Extract object as a temp null-terminated string */
        size_t obj_len = (size_t)(obj_end - obj_start + 1);
        if (obj_len >= MAX_MSG_SIZE) { p = obj_end + 1; continue; }
        char *obj = malloc(obj_len + 1);
        if (!obj) break;
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';

        char dev_id[MAX_DEVICE_ID_LEN] = "";
        json_find_str(obj, "id", dev_id, sizeof(dev_id));

        if (dev_id[0]) {
            /* Find or create slot */
            int slot = -1;
            for (int i = 0; i < g_device_count; i++) {
                if (strcmp(g_devices[i].device_id, dev_id) == 0) { slot = i; break; }
            }
            if (slot < 0 && g_device_count < MAX_DEVICES_UI) {
                slot = g_device_count++;
                memset(&g_devices[slot], 0, sizeof(ui_device_t));
                g_devices[slot].valid = 1;
            }
            if (slot >= 0) {
                ui_device_t *d = &g_devices[slot];
                strncpy(d->device_id, dev_id, MAX_DEVICE_ID_LEN-1);
                json_find_str(obj, "label",    d->label,    sizeof(d->label));
                json_find_str(obj, "hostname", d->hostname, sizeof(d->hostname));
                json_find_str(obj, "os",       d->os,       sizeof(d->os));
                json_find_str(obj, "ip",       d->ip,       sizeof(d->ip));
                d->ssh_port    = (uint16_t)json_find_int(obj, "ssh_port", 22);
                char on[8];
                json_find_str(obj, "online", on, sizeof(on));
                d->online      = strcmp(on, "true") == 0;
                d->last_seen   = (time_t)json_find_int(obj, "last_seen", 0);
                d->cpu_pct     = (float)atof(json_find_str(obj,"cpu_pct",  (char[32]){0},32) ?: "0");
                d->cpu_temp    = (float)atof(json_find_str(obj,"cpu_temp", (char[32]){0},32) ?: "-1");
                d->gpu_temp    = (float)atof(json_find_str(obj,"gpu_temp", (char[32]){0},32) ?: "-1");
                d->ram_used    = (uint64_t)json_find_int(obj,"ram_used",  0);
                d->ram_total   = (uint64_t)json_find_int(obj,"ram_total", 0);
                d->net_rx_bps  = (uint64_t)json_find_int(obj,"net_rx",    0);
                d->net_tx_bps  = (uint64_t)json_find_int(obj,"net_tx",    0);
                d->battery     = (float)atof(json_find_str(obj,"battery", (char[32]){0},32) ?: "-1");
                d->uptime      = (uint32_t)json_find_int(obj,"uptime",    0);
                if (via_lan) d->via_lan = 1;
            }
        }
        free(obj);
        p = obj_end + 1;
    }
    LeaveCriticalSection(&g_dev_cs);
}

/* ============================================================
 * BACKGROUND THREADS: relay poll + LAN discovery
 * ============================================================ */

static DWORD WINAPI relay_poll_thread(LPVOID arg) {
    relay_cfg_t *cfg = (relay_cfg_t*)arg;

    while (g_running) {
        SOCKET s = relay_connect_and_auth(cfg);
        if (s == INVALID_SOCKET) {
            /* back-off before retry */
            for (int i = 0; i < 20 && g_running; i++) Sleep(500);
            continue;
        }

        while (g_running) {
            /* Request device list */
            char req[128];
            json_builder_t jb; jb_init(&jb, req, sizeof(req));
            jb_begin(&jb); jb_str(&jb, "type", MSG_LIST); jb_end(&jb);
            if (net_send_msg(s, req) != 0) break;

            char *resp = net_recv_msg(s);
            if (!resp) break;

            char type[32];
            json_find_str(resp, "type", type, sizeof(type));
            if (strcmp(type, "device_list") == 0) {
                parse_and_merge_devices(resp, 0);
                SetEvent(g_refresh_event);   /* trigger redraw */
            }
            free(resp);

            /* Wait before next poll */
            for (int i = 0; i < (REFRESH_INTERVAL_MS / 200) && g_running; i++)
                Sleep(200);
        }

        closesocket(s);
    }
    return 0;
}

/* LAN discovery: send UDP broadcast, listen for agent beacons */
/* Agents broadcast a compact JSON on LAN_DISCOVERY_PORT */
static DWORD WINAPI lan_discovery_thread(LPVOID unused) {
    (void)unused;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;

    BOOL bcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char*)&bcast, sizeof(bcast));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(LAN_DISCOVERY_PORT);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind(s, (struct sockaddr*)&bind_addr, sizeof(bind_addr));

    /* Set receive timeout so we can check g_running */
    DWORD tv = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));

    while (g_running) {
        /* Send discovery probe broadcast */
        char probe[128];
        json_builder_t jb; jb_init(&jb, probe, sizeof(probe));
        jb_begin(&jb); jb_str(&jb, "type", "ms_probe"); jb_end(&jb);

        struct sockaddr_in bcast_addr;
        memset(&bcast_addr, 0, sizeof(bcast_addr));
        bcast_addr.sin_family      = AF_INET;
        bcast_addr.sin_port        = htons(LAN_DISCOVERY_PORT + 1);
        bcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(s, probe, (int)strlen(probe), 0,
               (struct sockaddr*)&bcast_addr, sizeof(bcast_addr));

        /* Listen for beacon responses for 2 seconds */
        char buf[4096];
        struct sockaddr_in from;
        int fromlen = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf)-1, 0,
                         (struct sockaddr*)&from, &fromlen);
        if (n > 0) {
            buf[n] = '\0';
            /* Build synthetic device_list JSON to reuse parse logic */
            char wrapper[8192];
            snprintf(wrapper, sizeof(wrapper), "{\"type\":\"device_list\",\"devices\":[%s]}", buf);
            /* Fix: agents send a single device object on LAN beacon */
            parse_and_merge_devices(wrapper, 1);
            SetEvent(g_refresh_event);
        }

        Sleep(LAN_BEACON_INTERVAL);
    }

    closesocket(s);
    return 0;
}

/* ============================================================
 * ACTIONS: SSH, SFTP, RDP, new window
 * ============================================================ */

/* Launch ssh.exe in a new console window — auto-picks key from ~/.ms/config.json */
static void action_ssh(ui_device_t *d) {
    if (!d->online && !d->ip[0]) {
        printf(COL_YELLOW "\n  Device is offline.\n" COL_RESET);
        Sleep(1500); return;
    }
    con_show_cursor();

    /* Get username — default Administrator, no prompt if key found */
    char uname[64] = "Administrator";
    const char *key_path = find_key_for_device(d->label);

    /* Build SSH command */
    char cmd[768];
    if (key_path) {
        snprintf(cmd, sizeof(cmd),
            "ssh -p %u -i \"%s\" -o StrictHostKeyChecking=accept-new -o IdentitiesOnly=yes %s@%s",
            d->ssh_port, key_path, uname, d->ip);
    } else {
        /* No key found — ask for username and fall back to interactive auth */
        printf("\n  SSH username (default: Administrator): ");
        fflush(stdout);
        DWORD mode;
        GetConsoleMode(g_console_in, &mode);
        SetConsoleMode(g_console_in, ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        fgets(uname, sizeof(uname), stdin);
        uname[strcspn(uname, "\r\n")] = '\0';
        if (uname[0] == '\0') strcpy(uname, "Administrator");
        SetConsoleMode(g_console_in, ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT);
        snprintf(cmd, sizeof(cmd),
            "ssh -p %u -o StrictHostKeyChecking=accept-new %s@%s",
            d->ssh_port, uname, d->ip);
    }

    /* Open in new console window */
    char full_cmd[840];
    snprintf(full_cmd, sizeof(full_cmd), "cmd.exe /K \"%s\"", cmd);
    STARTUPINFOA si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, full_cmd, NULL, NULL, FALSE,
                        CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
        system(cmd); /* fallback: same window */
    } else {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/* Launch sftp/WinSCP — auto-picks key */
static void action_sftp(ui_device_t *d) {
    if (!d->online) {
        printf(COL_YELLOW "\n  Device is offline.\n" COL_RESET);
        Sleep(1500); return;
    }
    const char *key_path = find_key_for_device(d->label);

    /* Try WinSCP GUI first */
    char winscp[MAX_PATH];
    ExpandEnvironmentStringsA("%ProgramFiles%\\WinSCP\\WinSCP.exe", winscp, MAX_PATH);
    if (GetFileAttributesA(winscp) != INVALID_FILE_ATTRIBUTES) {
        char args[512];
        if (key_path)
            snprintf(args, sizeof(args), "sftp://Administrator@%s:%u/ /privatekey=\"%s\"",
                     d->ip, d->ssh_port, key_path);
        else
            snprintf(args, sizeof(args), "sftp://Administrator@%s:%u/", d->ip, d->ssh_port);
        ShellExecuteA(NULL, "open", winscp, args, NULL, SW_SHOW);
        return;
    }

    /* CLI sftp fallback */
    char cmd[512];
    if (key_path)
        snprintf(cmd, sizeof(cmd), "sftp -P %u -i \"%s\" -o IdentitiesOnly=yes Administrator@%s",
                 d->ssh_port, key_path, d->ip);
    else
        snprintf(cmd, sizeof(cmd), "sftp -P %u Administrator@%s", d->ssh_port, d->ip);

    char full[600];
    snprintf(full, sizeof(full), "cmd.exe /K \"%s\"", cmd);
    STARTUPINFOA si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    CreateProcessA(NULL, full, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
}

/* Launch RDP (mstsc) */
static void action_rdp(ui_device_t *d) {
    if (!d->online) {
        printf(COL_YELLOW "\n  Device is offline.\n" COL_RESET);
        Sleep(1500); return;
    }
    char args[256];
    snprintf(args, sizeof(args), "/v:%s /f", d->ip);
    ShellExecuteA(NULL, "open", "mstsc.exe", args, NULL, SW_SHOW);
}

/* Open a new terminal window connected to same device */
static void action_new_window(ui_device_t *d) {
    action_ssh(d);
}

/* ============================================================
 * PUSH UPDATE — send push_update message to one or all devices
 * The device's MassUpdater service listens on the relay for this.
 * 'target_id' = specific device_id, or "" to broadcast to all.
 * ============================================================ */
static void action_push_update(const char *target_id) {
    if (g_relay_count == 0) {
        printf(COL_YELLOW "\n  No relay configured.\n" COL_RESET);
        Sleep(1500); return;
    }

    /* Connect to relay as manager */
    SOCKET s = relay_connect_and_auth(&g_relays[0]);
    if (s == INVALID_SOCKET) {
        printf(COL_RED "\n  Could not connect to relay.\n" COL_RESET);
        Sleep(1500); return;
    }

    /* Send push_update message */
    char buf[512];
    json_builder_t jb; jb_init(&jb, buf, sizeof(buf));
    jb_begin(&jb);
    jb_str(&jb, "type", "push_update");
    if (target_id && target_id[0])
        jb_str(&jb, "device_id", target_id);   /* targeted */
    else
        jb_str(&jb, "device_id", "*");          /* broadcast to all */
    jb_end(&jb);

    if (net_send_msg(s, buf) == 0)
        printf(COL_GREEN "\n  Update pushed to %s\n" COL_RESET,
               (target_id && target_id[0]) ? target_id : "ALL devices");
    else
        printf(COL_RED "\n  Failed to send update push.\n" COL_RESET);

    closesocket(s);
    Sleep(1200);
}

/* ============================================================
 * CONFIG: load from ~/.ms/config.json (written by setup-laptop.ps1)
 * Falls back to registry if JSON not found.
 * ============================================================ */

/* Global key map: device_id/label -> key path */
#define MAX_KEY_ENTRIES 32
typedef struct { char label[64]; char key_path[MAX_PATH]; } key_entry_t;
static key_entry_t g_key_map[MAX_KEY_ENTRIES];
static int         g_key_count = 0;
static char        g_key_dir[MAX_PATH] = "";

/* Look up the SSH key path for a given device label */
static const char *find_key_for_device(const char *label) {
    for (int i = 0; i < g_key_count; i++) {
        if (_stricmp(g_key_map[i].label, label) == 0)
            return g_key_map[i].key_path;
    }
    /* Fallback: look for any key file in key_dir */
    if (g_key_dir[0]) {
        static char found_path[MAX_PATH];
        WIN32_FIND_DATAA fd;
        char pattern[MAX_PATH];
        snprintf(pattern, sizeof(pattern), "%s\\id_*", g_key_dir);
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            snprintf(found_path, sizeof(found_path), "%s\\%s", g_key_dir, fd.cFileName);
            FindClose(h);
            return found_path;
        }
    }
    return NULL;
}

/* Parse a JSON string value from a flat JSON object */
static void json_cfg_str(const char *json, const char *key, char *out, size_t cap) {
    json_find_str(json, key, out, (int)cap);
}

static void config_load(void) {
    /* Try ~/.ms/config.json first */
    char cfg_path[MAX_PATH];
    ExpandEnvironmentStringsA("%USERPROFILE%\\.ms\\config.json", cfg_path, MAX_PATH);

    HANDLE fh = CreateFileA(cfg_path, GENERIC_READ, FILE_SHARE_READ,
                            NULL, OPEN_EXISTING, 0, NULL);
    if (fh != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(fh, NULL);
        if (sz > 0 && sz < 65536) {
            char *buf = (char*)malloc(sz + 1);
            if (buf) {
                DWORD rd;
                ReadFile(fh, buf, sz, &rd, NULL);
                buf[rd] = '\0';

                /* Parse relay */
                char host[256]="", port[16]="", token[MAX_TOKEN_LEN]="";
                json_cfg_str(buf, "relay_host",  host,  sizeof(host));
                json_cfg_str(buf, "relay_port",  port,  sizeof(port));
                json_cfg_str(buf, "relay_token", token, sizeof(token));
                json_cfg_str(buf, "key_dir",     g_key_dir, sizeof(g_key_dir));

                if (host[0] && g_relay_count < MAX_KNOWN_RELAYS) {
                    strncpy(g_relays[g_relay_count].host,  host,  255);
                    strncpy(g_relays[g_relay_count].port,  port[0] ? port : "10000", 15);
                    strncpy(g_relays[g_relay_count].token, token, MAX_TOKEN_LEN-1);
                    g_relay_count++;
                }

                /* Parse keys object: scan for "Label":"path" pairs */
                const char *keys_start = strstr(buf, "\"keys\"");
                if (keys_start) {
                    keys_start = strchr(keys_start, '{');
                    if (keys_start) {
                        const char *p = keys_start + 1;
                        while (*p && *p != '}' && g_key_count < MAX_KEY_ENTRIES) {
                            /* skip whitespace */
                            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
                            if (*p != '"') break;
                            p++;
                            /* read label */
                            int li = 0;
                            while (*p && *p != '"' && li < 63) g_key_map[g_key_count].label[li++] = *p++;
                            g_key_map[g_key_count].label[li] = '\0';
                            if (*p == '"') p++;
                            /* skip : */
                            while (*p == ' ' || *p == ':') p++;
                            if (*p != '"') break;
                            p++;
                            /* read path */
                            int pi2 = 0;
                            while (*p && *p != '"' && pi2 < MAX_PATH-1) {
                                if (*p == '\\' && *(p+1) == '\\') { g_key_map[g_key_count].key_path[pi2++] = '\\'; p += 2; }
                                else g_key_map[g_key_count].key_path[pi2++] = *p++;
                            }
                            g_key_map[g_key_count].key_path[pi2] = '\0';
                            if (*p == '"') p++;
                            if (g_key_map[g_key_count].label[0] && g_key_map[g_key_count].key_path[0])
                                g_key_count++;
                        }
                    }
                }
                free(buf);
                CloseHandle(fh);
                return; /* loaded from JSON — skip registry */
            }
        }
        CloseHandle(fh);
    }

    /* Fallback: registry */
    HKEY hkey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, CONFIG_REG_KEY, 0, KEY_READ, &hkey) != ERROR_SUCCESS)
        return;
    DWORD relay_count = 0, rsz = sizeof(relay_count);
    RegQueryValueExA(hkey, "RelayCount", NULL, NULL, (LPBYTE)&relay_count, &rsz);
    if (relay_count > MAX_KNOWN_RELAYS) relay_count = MAX_KNOWN_RELAYS;
    for (DWORD i = 0; i < relay_count; i++) {
        char key[32]; rsz = sizeof(g_relays[i].host);
        snprintf(key, sizeof(key), "RelayHost%lu", i);
        RegQueryValueExA(hkey, key, NULL, NULL, (LPBYTE)g_relays[i].host, &rsz);
        rsz = sizeof(g_relays[i].port);
        snprintf(key, sizeof(key), "RelayPort%lu", i);
        RegQueryValueExA(hkey, key, NULL, NULL, (LPBYTE)g_relays[i].port, &rsz);
        if (!g_relays[i].port[0]) strcpy(g_relays[i].port, RELAY_PORT_STR);
        rsz = sizeof(g_relays[i].token);
        snprintf(key, sizeof(key), "RelayToken%lu", i);
        RegQueryValueExA(hkey, key, NULL, NULL, (LPBYTE)g_relays[i].token, &rsz);
    }
    g_relay_count = (int)relay_count;
    RegCloseKey(hkey);
}

static void config_save(void) {
    HKEY hkey;
    RegCreateKeyExA(HKEY_CURRENT_USER, CONFIG_REG_KEY, 0, NULL,
                    REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hkey, NULL);
    DWORD rc = (DWORD)g_relay_count;
    RegSetValueExA(hkey, "RelayCount", 0, REG_DWORD, (LPBYTE)&rc, sizeof(rc));
    for (int i = 0; i < g_relay_count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "RelayHost%d", i);
        RegSetValueExA(hkey, key, 0, REG_SZ,
            (LPBYTE)g_relays[i].host, (DWORD)strlen(g_relays[i].host)+1);
        snprintf(key, sizeof(key), "RelayPort%d", i);
        RegSetValueExA(hkey, key, 0, REG_SZ,
            (LPBYTE)g_relays[i].port, (DWORD)strlen(g_relays[i].port)+1);
        snprintf(key, sizeof(key), "RelayToken%d", i);
        RegSetValueExA(hkey, key, 0, REG_SZ,
            (LPBYTE)g_relays[i].token, (DWORD)strlen(g_relays[i].token)+1);
    }
    RegCloseKey(hkey);
}

/* Interactive: add a relay server */
static void action_add_relay(void) {
    if (g_relay_count >= MAX_KNOWN_RELAYS) {
        printf("\n  Max relays reached.\n"); Sleep(1500); return;
    }
    con_show_cursor();
    DWORD mode;
    GetConsoleMode(g_console_in, &mode);
    SetConsoleMode(g_console_in, ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);

    relay_cfg_t *r = &g_relays[g_relay_count];
    con_move(g_height - 5, 1);
    printf("  Relay host: "); fflush(stdout);
    fgets(r->host, sizeof(r->host), stdin);
    r->host[strcspn(r->host, "\r\n")] = '\0';

    printf("  Relay port (default 443): "); fflush(stdout);
    fgets(r->port, sizeof(r->port), stdin);
    r->port[strcspn(r->port, "\r\n")] = '\0';
    if (!r->port[0]) strcpy(r->port, "443");

    printf("  Auth token: "); fflush(stdout);
    fgets(r->token, sizeof(r->token), stdin);
    r->token[strcspn(r->token, "\r\n")] = '\0';

    SetConsoleMode(g_console_in, ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT);
    con_hide_cursor();

    if (r->host[0]) {
        g_relay_count++;
        config_save();

        /* Spawn poll thread for this new relay */
        relay_cfg_t *cfg_copy = malloc(sizeof(relay_cfg_t));
        *cfg_copy = *r;
        HANDLE ht = CreateThread(NULL, 0, relay_poll_thread, cfg_copy, 0, NULL);
        if (ht) CloseHandle(ht);
    }
}

/* ============================================================
 * BACKGROUND REDRAW THREAD
 * ============================================================ */
static DWORD WINAPI redraw_thread(LPVOID unused) {
    (void)unused;
    while (g_running) {
        /* Wait for refresh signal or timeout (auto-refresh) */
        WaitForSingleObject(g_refresh_event, REFRESH_INTERVAL_MS);
        ResetEvent(g_refresh_event);
        if (g_running) ui_draw();
    }
    return 0;
}

/* ============================================================
 * INPUT LOOP (main thread)
 * ============================================================ */
static void input_loop(void) {
    while (g_running) {
        INPUT_RECORD ir;
        DWORD nr;
        if (!ReadConsoleInputA(g_console_in, &ir, 1, &nr)) break;
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        WORD vk    = ir.Event.KeyEvent.wVirtualKeyCode;
        char ch    = ir.Event.KeyEvent.uChar.AsciiChar;

        int prev_sel = g_selected;

        if (vk == VK_UP   || ch == 'k') {
            if (g_selected > 0) g_selected--;
            if (g_selected < g_scroll_offset) g_scroll_offset = g_selected;
        }
        else if (vk == VK_DOWN || ch == 'j') {
            EnterCriticalSection(&g_dev_cs);
            int max_sel = g_device_count - 1;
            LeaveCriticalSection(&g_dev_cs);
            if (g_selected < max_sel) g_selected++;
            int list_rows = g_height - 12;
            if (g_selected >= g_scroll_offset + list_rows)
                g_scroll_offset = g_selected - list_rows + 1;
        }
        else if (vk == VK_RETURN) {
            EnterCriticalSection(&g_dev_cs);
            if (g_selected >= 0 && g_selected < g_device_count) {
                ui_device_t d = g_devices[g_selected];
                LeaveCriticalSection(&g_dev_cs);
                action_ssh(&d);
                SetEvent(g_refresh_event);
            } else LeaveCriticalSection(&g_dev_cs);
        }
        else if (ch == 'f' || ch == 'F') {
            EnterCriticalSection(&g_dev_cs);
            if (g_selected >= 0 && g_selected < g_device_count) {
                ui_device_t d = g_devices[g_selected];
                LeaveCriticalSection(&g_dev_cs);
                action_sftp(&d);
            } else LeaveCriticalSection(&g_dev_cs);
        }
        else if (ch == 'r' || ch == 'R') {
            EnterCriticalSection(&g_dev_cs);
            if (g_selected >= 0 && g_selected < g_device_count) {
                ui_device_t d = g_devices[g_selected];
                LeaveCriticalSection(&g_dev_cs);
                action_rdp(&d);
            } else LeaveCriticalSection(&g_dev_cs);
        }
        else if (ch == 't' || ch == 'T') {
            EnterCriticalSection(&g_dev_cs);
            if (g_selected >= 0 && g_selected < g_device_count) {
                ui_device_t d = g_devices[g_selected];
                LeaveCriticalSection(&g_dev_cs);
                action_new_window(&d);
            } else LeaveCriticalSection(&g_dev_cs);
        }
        else if (ch == 'a' || ch == 'A') {
            action_add_relay();
            SetEvent(g_refresh_event);
        }
        else if (ch == 'u' || ch == 'U') {
            /* U = push update to selected device, Shift+U already = 'U'
             * Hold Ctrl+U to push to ALL devices */
            BOOL ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl) {
                /* Push to all */
                action_push_update("");
            } else {
                EnterCriticalSection(&g_dev_cs);
                if (g_selected >= 0 && g_selected < g_device_count) {
                    char dev_id[MAX_DEVICE_ID_LEN];
                    strncpy(dev_id, g_devices[g_selected].device_id,
                            MAX_DEVICE_ID_LEN-1);
                    LeaveCriticalSection(&g_dev_cs);
                    action_push_update(dev_id);
                } else {
                    LeaveCriticalSection(&g_dev_cs);
                }
            }
            SetEvent(g_refresh_event);
        }
        else if (ch == 'q' || ch == 'Q' || vk == VK_ESCAPE) {
            g_running = 0;
            SetEvent(g_refresh_event);
            break;
        }

        if (g_selected != prev_sel) SetEvent(g_refresh_event);
    }
}

/* ============================================================
 * MAIN
 * ============================================================ */
int main(int argc, char *argv[]) {
    /* Quick relay config from command line: msmgr.exe relay.host token */
    if (argc >= 3) {
        strncpy(g_relays[0].host,  argv[1], 255);
        strncpy(g_relays[0].port,  argc >= 4 ? argv[3] : "443", 15);
        strncpy(g_relays[0].token, argv[2], MAX_TOKEN_LEN-1);
        g_relay_count = 1;
        config_save();
    }

    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
    InitializeCriticalSection(&g_dev_cs);
    g_refresh_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    con_init();
    config_load();

    /* Auto-bootstrap from .temp/.env if no relay configured */
    if (g_relay_count == 0) {
        /* Try to fetch config from repo */
        const char *env_url = "https://raw.githubusercontent.com/ketw/sshra/master/.temp/.env";
        HINTERNET sess = WinHttpOpen(L"msmgr/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
        if (sess) {
            HINTERNET conn = WinHttpConnect(sess,
                L"raw.githubusercontent.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (conn) {
                HINTERNET req = WinHttpOpenRequest(conn, L"GET",
                    L"/ketw/sshra/master/.temp/.env",
                    NULL, WINHTTP_NO_REFERER,
                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
                if (req && WinHttpSendRequest(req, NULL, 0, NULL, 0, 0, 0)
                        && WinHttpReceiveResponse(req, NULL)) {
                    char body[4096] = {0}; DWORD got = 0, total = 0;
                    while (WinHttpReadData(req, body+total,
                                          (DWORD)(sizeof(body)-total-1), &got) && got)
                        total += got;
                    body[total] = '\0';
                    /* Parse KEY=VALUE lines */
                    char *line = strtok(body, "\n");
                    while (line) {
                        while (*line == ' ' || *line == '\r') line++;
                        if (*line && *line != '#') {
                            char *eq = strchr(line, '=');
                            if (eq) {
                                *eq = '\0'; char *k = line, *v = eq+1;
                                while (*v == ' ') v++;
                                int vlen = (int)strlen(v);
                                while (vlen > 0 && (v[vlen-1]=='\r'||v[vlen-1]=='\n'||v[vlen-1]==' '))
                                    v[--vlen] = '\0';
                                if (strcmp(k,"RELAY_HOST")==0)  strncpy(g_relays[0].host, v, 255);
                                if (strcmp(k,"RELAY_PORT")==0)  strncpy(g_relays[0].port, v, 15);
                                if (strcmp(k,"RELAY_TOKEN")==0) strncpy(g_relays[0].token,v, MAX_TOKEN_LEN-1);
                            }
                        }
                        line = strtok(NULL, "\n");
                    }
                    if (g_relays[0].host[0]) {
                        if (!g_relays[0].port[0]) strcpy(g_relays[0].port, "443");
                        g_relay_count = 1;
                        config_save();
                    }
                    WinHttpCloseHandle(req);
                }
                WinHttpCloseHandle(conn);
            }
            WinHttpCloseHandle(sess);
        }

        /* Still no config — prompt */
        if (g_relay_count == 0) {
            con_cls();
            printf(COL_BOLD COL_CYAN "  Mass Manager - Setup\n\n" COL_RESET);
            printf("  Could not auto-load config. Run setup-laptop.ps1 first, or:\n");
            printf("  Usage: ms <relay-host> <auth-token>\n\n");
            fflush(stdout);
            DWORD mode;
            GetConsoleMode(g_console_in, &mode);
            SetConsoleMode(g_console_in, ENABLE_PROCESSED_INPUT);
            INPUT_RECORD ir; DWORD nr;
            ReadConsoleInputA(g_console_in, &ir, 1, &nr);
            action_add_relay();
        }
    }

    /* Spawn relay poll threads for each configured relay */
    for (int i = 0; i < g_relay_count; i++) {
        relay_cfg_t *cfg = malloc(sizeof(relay_cfg_t));
        *cfg = g_relays[i];
        HANDLE ht = CreateThread(NULL, 0, relay_poll_thread, cfg, 0, NULL);
        if (ht) CloseHandle(ht);
    }

    /* LAN discovery thread */
    HANDLE lan_t = CreateThread(NULL, 0, lan_discovery_thread, NULL, 0, NULL);
    if (lan_t) CloseHandle(lan_t);

    /* Redraw thread */
    HANDLE draw_t = CreateThread(NULL, 0, redraw_thread, NULL, 0, NULL);
    if (draw_t) CloseHandle(draw_t);

    /* Initial draw */
    ui_draw();

    /* Block on input */
    input_loop();

    /* Cleanup */
    g_running = 0;
    SetEvent(g_refresh_event);
    Sleep(300);

    con_show_cursor();
    con_cls();
    printf("Goodbye.\n");

    WSACleanup();
    DeleteCriticalSection(&g_dev_cs);
    CloseHandle(g_refresh_event);
    return 0;
}
