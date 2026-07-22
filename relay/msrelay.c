/*
 * relay.c - Mass Relay Server
 *
 * Lightweight TCP relay / device registry.
 * Agents connect and register; managers connect and list/connect to devices.
 * Connection brokering: when manager requests a device, relay forwards the
 * request to the agent which then accepts a reverse SSH tunnel back.
 *
 * Deploy this on any VPS/cloud server with a public IP.
 *
 * Build (Linux): gcc relay.c -O2 -lpthread -o msrelay
 * Build (Windows): cl relay.c /Fe:msrelay.exe /link ws2_32.lib
 *
 * Usage: msrelay [--port 7744] [--token mysecret] [--cert cert.pem --key key.pem]
 */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define _WIN32_WINNT 0x0601
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET socket_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define CLOSE_SOCKET closesocket
  #define THREAD_RET DWORD WINAPI
  typedef HANDLE thread_t;
  #define MUTEX_T CRITICAL_SECTION
  #define MUTEX_INIT(m) InitializeCriticalSection(&(m))
  #define MUTEX_LOCK(m) EnterCriticalSection(&(m))
  #define MUTEX_UNLOCK(m) LeaveCriticalSection(&(m))
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <pthread.h>
  #include <netdb.h>
  typedef int socket_t;
  #define SOCK_INVALID (-1)
  #define CLOSE_SOCKET close
  #define THREAD_RET void*
  typedef pthread_t thread_t;
  #define MUTEX_T pthread_mutex_t
  #define MUTEX_INIT(m) pthread_mutex_init(&(m), NULL)
  #define MUTEX_LOCK(m) pthread_mutex_lock(&(m))
  #define MUTEX_UNLOCK(m) pthread_mutex_unlock(&(m))
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#ifndef _WIN32
#include <signal.h>
#endif

#include "../common/protocol.h"
#include "../common/json_util.h"

#define MAX_DEVICES     64
#define MAX_MANAGERS    32
#define DEVICE_TIMEOUT  90    /* seconds without heartbeat = offline */

/* ── Logging ───────────────────────────────────────────────────────────────── */
static void relay_log(const char *lvl, const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32]; strftime(ts, sizeof(ts), "%H:%M:%S", tm);
    printf("[%s][%s] ", ts, lvl);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n"); fflush(stdout);
}
#define RLOG(...)  relay_log("INFO",  __VA_ARGS__)
#define RWARN(...) relay_log("WARN",  __VA_ARGS__)
#define RERR(...)  relay_log("ERROR", __VA_ARGS__)

/* ── Device registry ───────────────────────────────────────────────────────── */
typedef struct {
    int      active;
    char     device_id[MAX_DEVICE_ID_LEN];
    char     hostname[MAX_HOSTNAME_LEN];
    char     label[64];
    char     os[128];
    char     remote_ip[48];    /* IP the agent connected from */
    uint16_t ssh_port;
    time_t   last_seen;
    socket_t sock;             /* agent's persistent connection */
    /* Latest stats from heartbeat */
    float    cpu_pct;
    float    cpu_temp;
    float    gpu_temp;
    uint64_t ram_used;
    uint64_t ram_total;
    uint64_t net_rx_bps;
    uint64_t net_tx_bps;
    float    battery;
    uint32_t uptime;
} device_t;

typedef struct {
    int      active;
    socket_t sock;
    char     remote_ip[48];
    time_t   connected_at;
} manager_t;

static device_t  g_devices[MAX_DEVICES];
static manager_t g_managers[MAX_MANAGERS];
static MUTEX_T   g_dev_mutex;
static char      g_auth_token[MAX_TOKEN_LEN] = "";
static int       g_port = RELAY_PORT;

/* ── Wire helpers ──────────────────────────────────────────────────────────── */
static int send_msg(socket_t s, const char *payload) {
    uint32_t len = (uint32_t)strlen(payload);
    uint32_t wlen = htonl(len);
    char hdr[4]; memcpy(hdr, &wlen, 4);
    if (send(s, hdr, 4, 0) != 4) return -1;
    int sent = 0;
    while (sent < (int)len) {
        int n = send(s, payload + sent, (int)len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static char *recv_msg(socket_t s) {
    char hdr[4]; int got = 0;
    while (got < 4) {
        int n = recv(s, hdr + got, 4 - got, 0);
        if (n <= 0) return NULL;
        got += n;
    }
    uint32_t len; memcpy(&len, hdr, 4); len = ntohl(len);
    if (len == 0 || len > MAX_MSG_SIZE) return NULL;
    char *buf = malloc(len + 1);
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

static void send_error(socket_t s, const char *reason) {
    char buf[512];
    json_builder_t jb; jb_init(&jb, buf, sizeof(buf));
    jb_begin(&jb);
    jb_str(&jb, "type", MSG_ERROR);
    jb_str(&jb, "reason", reason);
    jb_end(&jb);
    send_msg(s, buf);
}

/* ── Device list as JSON ───────────────────────────────────────────────────── */
static void build_device_list_json(char *buf, size_t cap) {
    json_builder_t jb; jb_init(&jb, buf, cap);
    /* We build manually since we have array of objects */
    size_t pos = 0;
    pos += snprintf(buf+pos, cap-pos, "{\"type\":\"device_list\",\"devices\":[");
    int first = 1;
    time_t now = time(NULL);
    MUTEX_LOCK(g_dev_mutex);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!g_devices[i].active) continue;
        int online = (now - g_devices[i].last_seen) < DEVICE_TIMEOUT;
        if (!first) pos += snprintf(buf+pos, cap-pos, ",");
        first = 0;
        pos += snprintf(buf+pos, cap-pos,
            "{\"id\":\"%s\",\"label\":\"%s\",\"hostname\":\"%s\","
            "\"os\":\"%s\",\"ip\":\"%s\",\"ssh_port\":%u,"
            "\"online\":%s,\"last_seen\":%lld,"
            "\"cpu_pct\":%.1f,\"cpu_temp\":%.1f,\"gpu_temp\":%.1f,"
            "\"ram_used\":%llu,\"ram_total\":%llu,"
            "\"net_rx\":%llu,\"net_tx\":%llu,"
            "\"battery\":%.1f,\"uptime\":%u}",
            g_devices[i].device_id, g_devices[i].label, g_devices[i].hostname,
            g_devices[i].os, g_devices[i].remote_ip, g_devices[i].ssh_port,
            online ? "true" : "false", (long long)g_devices[i].last_seen,
            g_devices[i].cpu_pct, g_devices[i].cpu_temp, g_devices[i].gpu_temp,
            (unsigned long long)g_devices[i].ram_used,
            (unsigned long long)g_devices[i].ram_total,
            (unsigned long long)g_devices[i].net_rx_bps,
            (unsigned long long)g_devices[i].net_tx_bps,
            g_devices[i].battery, g_devices[i].uptime);
    }
    MUTEX_UNLOCK(g_dev_mutex);
    pos += snprintf(buf+pos, cap-pos, "]}");
}

/* ============================================================
 * SINGLE-PORT DISPATCH
 * Render.com only exposes one port ($PORT env var).
 * Both agents and managers connect to the same port.
 * The auth message includes a "role" field: "agent" or "manager".
 * We read the full auth message here, dispatch accordingly,
 * then call the handler with pre-read auth data passed in.
 * ============================================================ */

/* Extended conn_arg that also carries the already-read auth message */
typedef struct {
    socket_t sock;
    char     remote_ip[48];
    char    *auth_msg;     /* malloc'd, NULL if not pre-read */
} client_arg_t;

/* check_auth_preread: like check_auth but uses a message we already read */
static int check_auth_preread(socket_t s, const char *msg) {
    char type[32]  = "", token[MAX_TOKEN_LEN] = "";
    json_find_str(msg, "type",  type,  sizeof(type));
    json_find_str(msg, "token", token, sizeof(token));
    if (strcmp(type, MSG_AUTH) != 0) return 0;
    if (g_auth_token[0] && strcmp(token, g_auth_token) != 0) return 0;
    char ok[64];
    json_builder_t jb; jb_init(&jb, ok, sizeof(ok));
    jb_begin(&jb); jb_str(&jb, "type", MSG_AUTH_OK); jb_end(&jb);
    return send_msg(s, ok) == 0 ? 1 : 0;
}

/* handle_agent_preauth: auth already done + auth msg already read */
static THREAD_RET handle_agent_preauth(void *arg) {
    client_arg_t *ca = (client_arg_t*)arg;
    socket_t s = ca->sock;
    char remote_ip[48]; memcpy(remote_ip, ca->remote_ip, 48);
    free(ca->auth_msg);
    free(ca);

    /* Expect register message */
    char *msg = recv_msg(s);
    if (!msg) { CLOSE_SOCKET(s); return 0; }

    char type[32], device_id[MAX_DEVICE_ID_LEN], hostname[MAX_HOSTNAME_LEN];
    char label[64], os[128];
    json_find_str(msg, "type",      type,      sizeof(type));
    json_find_str(msg, "device_id", device_id, sizeof(device_id));
    json_find_str(msg, "hostname",  hostname,  sizeof(hostname));
    json_find_str(msg, "label",     label,     sizeof(label));
    json_find_str(msg, "os",        os,        sizeof(os));
    long long ssh_port = json_find_int(msg, "ssh_port", 22);
    free(msg);

    if (strcmp(type, MSG_REGISTER) != 0 || device_id[0] == '\0') {
        send_error(s, "expected register"); CLOSE_SOCKET(s); return 0;
    }

    MUTEX_LOCK(g_dev_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (g_devices[i].active && strcmp(g_devices[i].device_id, device_id) == 0) {
            /* Kick old connection if same device reconnects */
            if (g_devices[i].sock != SOCK_INVALID)
                CLOSE_SOCKET(g_devices[i].sock);
            slot = i; break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_DEVICES; i++)
            if (!g_devices[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        MUTEX_UNLOCK(g_dev_mutex);
        send_error(s, "max devices reached"); CLOSE_SOCKET(s); return 0;
    }
    device_t *dev = &g_devices[slot];
    dev->active = 1;
    strncpy(dev->device_id, device_id, MAX_DEVICE_ID_LEN-1);
    strncpy(dev->hostname,  hostname,  MAX_HOSTNAME_LEN-1);
    strncpy(dev->label,     label,     63);
    strncpy(dev->os,        os,        127);
    strncpy(dev->remote_ip, remote_ip, 47);
    dev->ssh_port  = (uint16_t)ssh_port;
    dev->last_seen = time(NULL);
    dev->sock      = s;
    MUTEX_UNLOCK(g_dev_mutex);

    RLOG("Agent registered: '%s' (%s) from %s", label, device_id, remote_ip);

    /* Set a long receive timeout — heartbeats every 30s, we allow 90s */
#ifdef _WIN32
    DWORD rtv = 95000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&rtv, sizeof(rtv));
#else
    struct timeval rtv = {95, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
#endif

    while (1) {
        char *hb = recv_msg(s);
        if (!hb) break;
        char htype[32];
        json_find_str(hb, "type", htype, sizeof(htype));
        if (strcmp(htype, MSG_HEARTBEAT) == 0) {
            MUTEX_LOCK(g_dev_mutex);
            dev->last_seen  = time(NULL);
            char tmp[32];
            dev->cpu_pct    = (float)atof(json_find_str(hb,"cpu_pct",  tmp,32) ? tmp : "0");
            dev->cpu_temp   = (float)atof(json_find_str(hb,"cpu_temp", tmp,32) ? tmp : "-1");
            dev->gpu_temp   = (float)atof(json_find_str(hb,"gpu_temp", tmp,32) ? tmp : "-1");
            dev->ram_used   = (uint64_t)json_find_int(hb,"ram_used",  0);
            dev->ram_total  = (uint64_t)json_find_int(hb,"ram_total", 0);
            dev->net_rx_bps = (uint64_t)json_find_int(hb,"net_rx_bps",0);
            dev->net_tx_bps = (uint64_t)json_find_int(hb,"net_tx_bps",0);
            dev->battery    = (float)atof(json_find_str(hb,"battery", tmp,32) ? tmp : "-1");
            dev->uptime     = (uint32_t)json_find_int(hb,"uptime",    0);
            MUTEX_UNLOCK(g_dev_mutex);
        }
        free(hb);
    }

    MUTEX_LOCK(g_dev_mutex);
    dev->active = 0; dev->sock = SOCK_INVALID;
    MUTEX_UNLOCK(g_dev_mutex);
    RLOG("Agent disconnected: '%s' (%s)", label, device_id);
    CLOSE_SOCKET(s);
    return 0;
}

/* handle_manager_preauth: auth already done */
static THREAD_RET handle_manager_preauth(void *arg) {
    client_arg_t *ca = (client_arg_t*)arg;
    socket_t s = ca->sock;
    char remote_ip[48]; memcpy(remote_ip, ca->remote_ip, 48);
    free(ca->auth_msg);
    free(ca);

    RLOG("Manager connected from %s", remote_ip);

    while (1) {
        char *msg = recv_msg(s);
        if (!msg) break;
        char type[32];
        json_find_str(msg, "type", type, sizeof(type));

        if (strcmp(type, MSG_LIST) == 0) {
            char *list_buf = (char*)malloc(MAX_MSG_SIZE);
            if (list_buf) {
                build_device_list_json(list_buf, MAX_MSG_SIZE);
                send_msg(s, list_buf);
                free(list_buf);
            }
        }
        else if (strcmp(type, MSG_CONNECT) == 0) {
            char target_id[MAX_DEVICE_ID_LEN];
            json_find_str(msg, "device_id", target_id, sizeof(target_id));
            MUTEX_LOCK(g_dev_mutex);
            device_t *found = NULL;
            for (int i = 0; i < MAX_DEVICES; i++) {
                if (g_devices[i].active &&
                    strcmp(g_devices[i].device_id, target_id) == 0) {
                    found = &g_devices[i]; break;
                }
            }
            char resp[1024];
            if (found && (time(NULL) - found->last_seen) < DEVICE_TIMEOUT) {
                json_builder_t jb; jb_init(&jb, resp, sizeof(resp));
                jb_begin(&jb);
                jb_str(&jb, "type",      MSG_TUNNEL_READY);
                jb_str(&jb, "device_id", found->device_id);
                jb_str(&jb, "ip",        found->remote_ip);
                jb_int(&jb, "ssh_port",  found->ssh_port);
                jb_str(&jb, "label",     found->label);
                jb_end(&jb);
            } else {
                json_builder_t jb; jb_init(&jb, resp, sizeof(resp));
                jb_begin(&jb);
                jb_str(&jb, "type",   MSG_ERROR);
                jb_str(&jb, "reason", "device offline or not found");
                jb_end(&jb);
            }
            MUTEX_UNLOCK(g_dev_mutex);
            send_msg(s, resp);
        }
        else if (strcmp(type, MSG_DISCONNECT) == 0) {
            free(msg); break;
        }
        free(msg);
    }

    RLOG("Manager disconnected from %s", remote_ip);
    CLOSE_SOCKET(s); return 0;
}

/* ── HTTP health check handler (for Render.com) ────────────────────────────── */
/* Render pings GET /health expecting HTTP 200 to confirm the service is up.   */
static void handle_http_health(socket_t s) {
    const char *resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n"
        "\r\nOK";
    send(s, resp, (int)strlen(resp), 0);
    CLOSE_SOCKET(s);
}

/* ── Single-port dispatcher ────────────────────────────────────────────────── */
static THREAD_RET dispatch_client(void *arg) {
    client_arg_t *ca = (client_arg_t*)arg;
    socket_t s       = ca->sock;
    char remote_ip[48]; memcpy(remote_ip, ca->remote_ip, 48);

    /* Read the framed auth message */
    char hdr[4]; int got = 0;
    while (got < 4) {
        int n = recv(s, hdr + got, 4 - got, 0);
        if (n <= 0) { free(ca); CLOSE_SOCKET(s); return 0; }
        got += n;
    }
    uint32_t len; memcpy(&len, hdr, 4); len = ntohl(len);

    /* If this looks like HTTP (no length prefix, starts with "GET " or "POST "):
     * Handle Render's health check probe. HTTP won't have a valid 4-byte length
     * that matches a reasonable message size — we detect by checking if the
     * raw 4 bytes spell "GET " or "POST" etc. */
    if (hdr[0] == 'G' || hdr[0] == 'P' || hdr[0] == 'H') {
        /* Consume the rest of the HTTP request, then respond */
        char http_buf[1024]; int hgot = 4;
        memcpy(http_buf, hdr, 4);
        while (hgot < (int)sizeof(http_buf) - 1) {
            int n = recv(s, http_buf + hgot, 1, 0);
            if (n <= 0) break;
            hgot += n;
            http_buf[hgot] = '\0';
            if (strstr(http_buf, "\r\n\r\n")) break;
        }
        handle_http_health(s);
        free(ca); return 0;
    }

    if (len == 0 || len > 8192) { free(ca); CLOSE_SOCKET(s); return 0; }

    char *auth_msg = malloc(len + 1);
    if (!auth_msg) { free(ca); CLOSE_SOCKET(s); return 0; }
    got = 0;
    while (got < (int)len) {
        int n = recv(s, auth_msg + got, (int)len - got, 0);
        if (n <= 0) { free(auth_msg); free(ca); CLOSE_SOCKET(s); return 0; }
        got += n;
    }
    auth_msg[len] = '\0';

    /* Parse role before auth (role is not secret) */
    char role[16] = "agent";
    json_find_str(auth_msg, "role", role, sizeof(role));

    /* Auth check + send auth_ok */
    if (!check_auth_preread(s, auth_msg)) {
        RWARN("Auth failed from %s (role=%s)", remote_ip, role);
        free(auth_msg); free(ca); CLOSE_SOCKET(s); return 0;
    }

    ca->auth_msg = auth_msg;

    if (strcmp(role, "manager") == 0)
        return handle_manager_preauth(ca);
    else
        return handle_agent_preauth(ca);
}

/* ── Listener + accept loop ────────────────────────────────────────────────── */

static socket_t make_listener(int port) {
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SOCK_INVALID) return SOCK_INVALID;
    int opt = 1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        RERR("bind() failed on port %d: errno=%d", port, errno);
        CLOSE_SOCKET(s); return SOCK_INVALID;
    }
    if (listen(s, 64) != 0) { CLOSE_SOCKET(s); return SOCK_INVALID; }
    RLOG("Listening on 0.0.0.0:%d", port);
    return s;
}

int main(int argc, char *argv[]) {
    /* Read port from env first (Render sets $PORT), then --port arg overrides */
    const char *env_port = getenv("PORT");
    if (env_port && atoi(env_port) > 0) g_port = atoi(env_port);

    const char *env_token = getenv("RELAY_TOKEN");
    if (env_token && env_token[0])
        strncpy(g_auth_token, env_token, MAX_TOKEN_LEN-1);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port")  == 0 && i+1 < argc) g_port = atoi(argv[++i]);
        if (strcmp(argv[i], "--token") == 0 && i+1 < argc)
            strncpy(g_auth_token, argv[++i], MAX_TOKEN_LEN-1);
    }

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#else
    /* Ignore SIGPIPE — broken socket writes return EPIPE instead of killing process */
    signal(SIGPIPE, SIG_IGN);
#endif

    MUTEX_INIT(g_dev_mutex);
    memset(g_devices,  0, sizeof(g_devices));
    memset(g_managers, 0, sizeof(g_managers));

    RLOG("=== Mass Relay Server ===");
    RLOG("Single-port mode (Render.com compatible)");
    RLOG("Port: %d", g_port);
    if (g_auth_token[0]) RLOG("Auth token: SET (%zu chars)", strlen(g_auth_token));
    else RLOG("WARNING: No auth token set! Set RELAY_TOKEN env var.");

    socket_t listener = make_listener(g_port);
    if (listener == SOCK_INVALID) {
        RERR("Failed to start listener on port %d", g_port); return 1;
    }

    RLOG("Ready. Accepting agents and managers on port %d", g_port);

    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t addrlen = sizeof(client_addr);
        socket_t client = accept(listener, (struct sockaddr*)&client_addr, &addrlen);
        if (client == SOCK_INVALID) continue;

        client_arg_t *ca = malloc(sizeof(client_arg_t));
        if (!ca) { CLOSE_SOCKET(client); continue; }
        ca->sock     = client;
        ca->auth_msg = NULL;
        if (client_addr.ss_family == AF_INET6)
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)&client_addr)->sin6_addr,
                      ca->remote_ip, sizeof(ca->remote_ip));
        else
            inet_ntop(AF_INET, &((struct sockaddr_in*)&client_addr)->sin_addr,
                      ca->remote_ip, sizeof(ca->remote_ip));

#ifdef _WIN32
        HANDLE t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)dispatch_client, ca, 0, NULL);
        if (t) CloseHandle(t); else { free(ca); CLOSE_SOCKET(client); }
#else
        pthread_t t;
        pthread_attr_t attr; pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &attr, dispatch_client, ca);
        pthread_attr_destroy(&attr);
#endif
    }
    return 0;
}
