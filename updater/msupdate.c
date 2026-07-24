/*
 * msupdate.c - Mass Auto-Updater
 *
 * Runs as a Windows service (MassUpdater).
 * Two update triggers:
 *   1. Polls .temp/update.json from GitHub every 5 minutes
 *   2. Listens on the relay for a push_update message from the manager
 *
 * On update detected:
 *   - Downloads new binary to temp path
 *   - SHA-256 verifies it
 *   - Stops old service, replaces binary, restarts service
 *   - Logs every step to C:\ProgramData\Mass\update.log
 *
 * Build: gcc msupdate.c -I../common -O2 -D_WIN32_WINNT=0x0601
 *        -municode -o msupdate.exe -lws2_32 -ladvapi32 -lwinhttp -lcrypt32
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <winsvc.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <winreg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>

#include "..\common\protocol.h"
#include "..\common\json_util.h"
#include "..\common\ws.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

/* ── Constants ────────────────────────────────────────────────────────────── */
#define SVC_NAME          L"MassUpdater"
#define SVC_DISPLAY       L"Mass Updater"
#define SVC_DESC          L"Mass remote access - auto updater"
#define DATA_DIR          "C:\\ProgramData\\Mass"
#define LOG_FILE          "C:\\ProgramData\\Mass\\update.log"
#define INSTALL_DIR       "C:\\Program Files\\Mass"
#define AGENT_EXE         "C:\\Program Files\\Mass\\msagent.exe"
#define UPDATE_URL_PATH   L"/.temp/update.json"
#define POLL_INTERVAL_MS  (5 * 60 * 1000)   /* 5 minutes */
#define CONFIG_REG_KEY    L"SOFTWARE\\Mass"

/* ── Globals ──────────────────────────────────────────────────────────────── */
static SERVICE_STATUS        g_status;
static SERVICE_STATUS_HANDLE g_svc_handle;
static HANDLE                g_stop_event;
static HANDLE                g_update_event;
static char                  g_relay_host[256] = "";
static char                  g_relay_token[MAX_TOKEN_LEN] = "";
static char                  g_current_version[32] = "0.0.0";
static char                  g_repo_raw[512] = "https://raw.githubusercontent.com/ketw/sshra/master";
static CRITICAL_SECTION      g_log_cs;

/* ── Forward declarations ────────────────────────────────────────────────── */
static char *http_get(const wchar_t *host, const wchar_t *path, DWORD *out_len);
static int   http_download(const wchar_t *host, const wchar_t *path, const char *dest);
static void  check_for_update(void);
static void  do_update(const char *version, const char *agent_url, const char *sha256);
static int   sha256_file(const char *path, char *out_hex64);
static void  config_load(void);
static void  version_save(const char *ver);
static int   version_newer(const char *remote, const char *local);

/* ── Logging ──────────────────────────────────────────────────────────────── */
static void ulog(const char *level, const char *fmt, ...) {
    EnterCriticalSection(&g_log_cs);
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
        fprintf(f, "[%s][%s] ", ts, level);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fprintf(f, "\n"); fflush(f); fclose(f);
    }
    LeaveCriticalSection(&g_log_cs);
}
#define ULOG(...)  ulog("INFO",  __VA_ARGS__)
#define UWARN(...) ulog("WARN",  __VA_ARGS__)
#define UERR(...)  ulog("ERROR", __VA_ARGS__)

/* ── HTTP GET via WinHTTP (handles HTTPS + redirects) ────────────────────── */
/* Returns malloc'd response body, sets *out_len. NULL on failure. */
static char *http_get(const wchar_t *host, const wchar_t *path,
                       DWORD *out_len) {
    HINTERNET sess = WinHttpOpen(L"MassUpdater/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return NULL;

    HINTERNET conn = WinHttpConnect(sess, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { WinHttpCloseHandle(sess); return NULL; }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, NULL,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(sess); return NULL; }

    /* Follow redirects */
    DWORD opt = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &opt, sizeof(opt));

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(sess);
        return NULL;
    }

    /* Read full body */
    char *body = NULL; DWORD total = 0;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
        char *tmp = realloc(body, total + avail + 1);
        if (!tmp) break;
        body = tmp;
        DWORD read = 0;
        WinHttpReadData(req, body + total, avail, &read);
        total += read;
        body[total] = '\0';
    }
    if (out_len) *out_len = total;

    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(sess);
    return body;
}

/* Download a base64-encoded file, decode it, and write binary to dest.
 * Returns 1 on success. GitHub serves binary files as .b64 text. */
static int http_download(const wchar_t *host, const wchar_t *path,
                          const char *dest) {
    DWORD len = 0;
    char *b64 = http_get(host, path, &len);
    if (!b64 || len == 0) { free(b64); return 0; }

    /* Strip whitespace/newlines from base64 */
    char *clean = (char*)malloc(len + 1);
    if (!clean) { free(b64); return 0; }
    DWORD ci = 0;
    for (DWORD i = 0; i < len; i++) {
        char c = b64[i];
        if (c != '\r' && c != '\n' && c != ' ') clean[ci++] = c;
    }
    clean[ci] = '\0';
    free(b64);

    /* Base64 decode using CryptStringToBinaryA */
    DWORD bin_len = 0;
    if (!CryptStringToBinaryA(clean, ci, CRYPT_STRING_BASE64, NULL, &bin_len, NULL, NULL)) {
        free(clean); return 0;
    }
    BYTE *bin = (BYTE*)malloc(bin_len);
    if (!bin) { free(clean); return 0; }
    if (!CryptStringToBinaryA(clean, ci, CRYPT_STRING_BASE64, bin, &bin_len, NULL, NULL)) {
        free(clean); free(bin); return 0;
    }
    free(clean);

    /* Write to temp file then atomically replace dest */
    char tmp_path[MAX_PATH];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dest);
    HANDLE fh = CreateFileA(tmp_path, GENERIC_WRITE, 0, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) { free(bin); return 0; }
    DWORD written = 0;
    WriteFile(fh, bin, bin_len, &written, NULL);
    CloseHandle(fh);
    free(bin);

    if (written != bin_len) { DeleteFileA(tmp_path); return 0; }

    /* Atomic replace */
    char bak[MAX_PATH];
    snprintf(bak, sizeof(bak), "%s.bak", dest);
    DeleteFileA(bak);
    MoveFileA(dest, bak);
    if (!MoveFileA(tmp_path, dest)) {
        MoveFileA(bak, dest);
        return 0;
    }
    return 1;
}

/* ── SHA-256 of a file using Windows CNG ─────────────────────────────────── */
static int sha256_file(const char *path, char *out_hex64) {
    HCRYPTPROV prov = 0; HCRYPTHASH hash = 0;
    BYTE digest[32]; DWORD dlen = 32;
    int ok = 0;

    HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING, 0, NULL);
    if (fh == INVALID_HANDLE_VALUE) return 0;

    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES,
                               CRYPT_VERIFYCONTEXT)) goto done;
    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) goto done;

    char buf[65536]; DWORD rd;
    while (ReadFile(fh, buf, sizeof(buf), &rd, NULL) && rd > 0)
        CryptHashData(hash, (BYTE*)buf, rd, 0);

    if (CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0)) {
        for (int i = 0; i < 32; i++)
            sprintf(out_hex64 + i*2, "%02x", digest[i]);
        out_hex64[64] = '\0'; ok = 1;
    }
done:
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(fh);
    return ok;
}

/* ── Read config from registry ────────────────────────────────────────────── */
static void config_load(void) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CONFIG_REG_KEY, 0,
                      KEY_READ, &hk) != ERROR_SUCCESS) return;
    DWORD sz;
    sz = sizeof(g_relay_host);
    RegQueryValueExA(hk, "RelayHost",  NULL, NULL, (LPBYTE)g_relay_host,  &sz);
    sz = sizeof(g_relay_token);
    RegQueryValueExA(hk, "AuthToken",  NULL, NULL, (LPBYTE)g_relay_token, &sz);
    sz = sizeof(g_repo_raw);
    RegQueryValueExA(hk, "RepoRaw",    NULL, NULL, (LPBYTE)g_repo_raw,    &sz);
    sz = sizeof(g_current_version);
    RegQueryValueExA(hk, "Version",    NULL, NULL, (LPBYTE)g_current_version, &sz);
    RegCloseKey(hk);
}

static void version_save(const char *ver) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CONFIG_REG_KEY, 0,
                      KEY_WRITE, &hk) != ERROR_SUCCESS) return;
    RegSetValueExA(hk, "Version", 0, REG_SZ, (LPBYTE)ver, (DWORD)strlen(ver)+1);
    RegCloseKey(hk);
    strncpy(g_current_version, ver, 31);
}

/* ── Core update logic ────────────────────────────────────────────────────── */
static int version_newer(const char *remote, const char *local) {
    /* Simple semver compare: split on '.' and compare numerically */
    int ra,rb,rc, la,lb,lc;
    if (sscanf(remote, "%d.%d.%d", &ra,&rb,&rc) != 3) return 0;
    if (sscanf(local,  "%d.%d.%d", &la,&lb,&lc) != 3) return 0;
    if (ra != la) return ra > la;
    if (rb != lb) return rb > lb;
    return rc > lc;
}

static void do_update(const char *version, const char *agent_url,
                      const char *agent_sha256) {
    ULOG("=== Update triggered: %s -> %s ===", g_current_version, version);

    /* Build wide strings for WinHTTP */
    wchar_t whost[256], wpath[512];
    /* Parse host and path from the URL */
    /* URL format: https://raw.githubusercontent.com/ketw/sshra/master/.temp/msagent.exe */
    const char *p = agent_url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    const char *slash = strchr(p, '/');
    if (!slash) { UERR("Bad agent URL: %s", agent_url); return; }

    char host_a[256]; int hlen = (int)(slash - p);
    if (hlen >= 256) { UERR("Host too long"); return; }
    memcpy(host_a, p, hlen); host_a[hlen] = '\0';
    mbstowcs(whost, host_a, 256);
    mbstowcs(wpath, slash, 512);

    /* Download to temp location */
    char tmp_agent[MAX_PATH];
    snprintf(tmp_agent, sizeof(tmp_agent), "%s\\msagent_new.exe", DATA_DIR);

    ULOG("Downloading new msagent.exe from %s...", agent_url);
    if (!http_download(whost, wpath, tmp_agent)) {
        UERR("Download failed"); return;
    }
    ULOG("Download complete: %s", tmp_agent);

    /* SHA-256 verify if hash provided */
    if (agent_sha256 && agent_sha256[0]) {
        char actual[65] = "";
        sha256_file(tmp_agent, actual);
        if (strcmp(actual, agent_sha256) != 0) {
            UERR("SHA-256 mismatch! expected=%s actual=%s", agent_sha256, actual);
            DeleteFileA(tmp_agent);
            return;
        }
        ULOG("SHA-256 verified OK");
    } else {
        ULOG("No SHA-256 provided, skipping verification");
    }

    /* Stop MassAgent service */
    ULOG("Stopping MassAgent service...");
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, L"MassAgent",
                                      SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (svc) {
            SERVICE_STATUS ss;
            ControlService(svc, SERVICE_CONTROL_STOP, &ss);
            /* Wait up to 10s for it to stop */
            for (int i = 0; i < 20; i++) {
                Sleep(500);
                QueryServiceStatus(svc, &ss);
                if (ss.dwCurrentState == SERVICE_STOPPED) break;
            }
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
    ULOG("MassAgent stopped");

    /* Replace binary */
    char bak[MAX_PATH];
    snprintf(bak, sizeof(bak), "%s\\msagent.bak", INSTALL_DIR);
    DeleteFileA(bak);
    MoveFileA(AGENT_EXE, bak);
    if (!MoveFileA(tmp_agent, AGENT_EXE)) {
        UERR("Failed to replace msagent.exe — restoring backup");
        MoveFileA(bak, AGENT_EXE);
        return;
    }
    ULOG("msagent.exe replaced successfully");

    /* Restart MassAgent */
    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, L"MassAgent", SERVICE_START);
        if (svc) { StartServiceW(svc, 0, NULL); CloseServiceHandle(svc); }
        CloseServiceHandle(scm);
    }
    ULOG("MassAgent restarted");

    /* Save new version */
    version_save(version);
    ULOG("=== Update complete: now at %s ===", version);
}

/* ── Check for update from GitHub ────────────────────────────────────────── */
static void check_for_update(void) {
    ULOG("Checking for updates at %s/.temp/update.json ...", g_repo_raw);

    /* Parse repo host + path from g_repo_raw */
    const char *p = g_repo_raw;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    const char *slash = strchr(p, '/');
    if (!slash) { UERR("Bad repo URL"); return; }

    char host_a[256]; int hlen = (int)(slash - p);
    memcpy(host_a, p, hlen); host_a[hlen] = '\0';
    wchar_t whost[256]; mbstowcs(whost, host_a, 256);

    char path_a[512]; snprintf(path_a, sizeof(path_a), "%s/.temp/update.json", slash);
    wchar_t wpath[512]; mbstowcs(wpath, path_a, 512);

    DWORD len = 0;
    char *json = http_get(whost, wpath, &len);
    if (!json) { UWARN("Could not fetch update.json (relay may be sleeping)"); return; }

    /* Parse fields */
    char remote_ver[32]  = "";
    char agent_url[512]  = "";
    char agent_sha[65]   = "";
    char push_flag[8]    = "false";

    json_find_str(json, "version",      remote_ver,  sizeof(remote_ver));
    json_find_str(json, "push_update",  push_flag,   sizeof(push_flag));

    /* msagent sub-object: find "url" and "sha256" inside it */
    const char *agent_obj = strstr(json, "\"msagent\"");
    if (agent_obj) {
        /* Find the { after "msagent": */
        const char *obj = strchr(agent_obj, '{');
        if (obj) {
            char tmp[1024];
            const char *end = strchr(obj, '}');
            if (end) {
                int olen = (int)(end - obj + 1);
                if (olen < (int)sizeof(tmp)) {
                    memcpy(tmp, obj, olen); tmp[olen] = '\0';
                    json_find_str(tmp, "url",    agent_url, sizeof(agent_url));
                    json_find_str(tmp, "sha256", agent_sha, sizeof(agent_sha));
                }
            }
        }
    }
    free(json);

    int is_push = (strcmp(push_flag, "true") == 0);
    int is_newer = version_newer(remote_ver, g_current_version);

    ULOG("Remote version: %s  Local: %s  Newer: %s  Push: %s",
         remote_ver, g_current_version,
         is_newer ? "YES" : "NO",
         is_push  ? "YES" : "NO");

    if ((is_newer || is_push) && remote_ver[0] && agent_url[0]) {
        do_update(remote_ver, agent_url, agent_sha);
    } else {
        ULOG("No update needed");
    }
}

/* ── Relay listener thread: watches for push_update messages ─────────────── */
static DWORD WINAPI relay_listener_thread(LPVOID unused) {
    (void)unused;
    ULOG("Relay listener started (host=%s)", g_relay_host);

    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        /* TCP connect to relay port 443 */
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(g_relay_host, "443", &hints, &res) != 0) {
            WaitForSingleObject(g_stop_event, 30000);
            continue;
        }

        SOCKET s = INVALID_SOCKET;
        for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
            s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (s == INVALID_SOCKET) continue;
            DWORD tv = 15000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
            if (connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
            closesocket(s); s = INVALID_SOCKET;
        }
        freeaddrinfo(res);

        if (s == INVALID_SOCKET) {
            WaitForSingleObject(g_stop_event, 30000);
            continue;
        }

        /* WebSocket handshake */
        if (ws_client_handshake((ws_sock_t)s, g_relay_host, "/ms") != 0) {
            closesocket(s);
            WaitForSingleObject(g_stop_event, 30000);
            continue;
        }

        /* Auth as updater agent */
        char auth_buf[512];
        json_builder_t jb; jb_init(&jb, auth_buf, sizeof(auth_buf));
        jb_begin(&jb);
        jb_str(&jb, "type",  MSG_AUTH);
        jb_str(&jb, "role",  "updater");
        jb_str(&jb, "token", g_relay_token);
        jb_end(&jb);
        ws_send_msg((ws_sock_t)s, auth_buf, 1);

        char *resp = ws_recv_msg((ws_sock_t)s);
        if (!resp) { closesocket(s); WaitForSingleObject(g_stop_event, 30000); continue; }
        char rtype[32]; json_find_str(resp, "type", rtype, sizeof(rtype)); free(resp);
        if (strcmp(rtype, MSG_AUTH_OK) != 0) {
            UWARN("Relay auth failed for updater");
            closesocket(s); WaitForSingleObject(g_stop_event, 60000); continue;
        }
        ULOG("Relay listener connected, waiting for push_update...");

        /* Listen loop */
        while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
            char *msg = ws_recv_msg((ws_sock_t)s);
            if (!msg) break;
            char mtype[32]; json_find_str(msg, "type", mtype, sizeof(mtype));
            if (strcmp(mtype, "push_update") == 0) {
                ULOG("push_update received from manager — triggering update check");
                SetEvent(g_update_event);
            }
            free(msg);
        }
        closesocket(s);
        ULOG("Relay listener disconnected, reconnecting...");
        WaitForSingleObject(g_stop_event, 10000);
    }
    return 0;
}

/* ── Poll thread: checks for updates every 5 minutes ─────────────────────── */
static DWORD WINAPI poll_thread(LPVOID unused) {
    (void)unused;
    ULOG("Poll thread started, interval=%d ms", POLL_INTERVAL_MS);

    /* Run first check shortly after start */
    WaitForSingleObject(g_stop_event, 10000);

    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        check_for_update();

        /* Wait for poll interval OR a push_update event, whichever comes first */
        HANDLE events[2] = { g_stop_event, g_update_event };
        DWORD w = WaitForMultipleObjects(2, events, FALSE, POLL_INTERVAL_MS);
        if (w == WAIT_OBJECT_0) break;          /* stop */
        if (w == WAIT_OBJECT_0 + 1) {           /* push event */
            ResetEvent(g_update_event);
            ULOG("Push event received — checking immediately");
            check_for_update();
        }
        /* timeout = next scheduled poll */
    }
    return 0;
}

/* ── Windows service boilerplate ─────────────────────────────────────────── */
static void svc_report(DWORD state, DWORD wait_hint) {
    static DWORD cp = 1;
    g_status.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState     = state;
    g_status.dwWin32ExitCode    = NO_ERROR;
    g_status.dwWaitHint         = wait_hint;
    g_status.dwControlsAccepted = (state == SERVICE_START_PENDING) ? 0
                                  : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_status.dwCheckPoint = (state==SERVICE_RUNNING||state==SERVICE_STOPPED) ? 0 : cp++;
    SetServiceStatus(g_svc_handle, &g_status);
}

static VOID WINAPI svc_ctrl(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        svc_report(SERVICE_STOP_PENDING, 3000);
        SetEvent(g_stop_event);
    }
}

static VOID WINAPI svc_main(DWORD argc, LPWSTR *argv) {
    (void)argc; (void)argv;
    g_svc_handle = RegisterServiceCtrlHandlerW(SVC_NAME, svc_ctrl);
    if (!g_svc_handle) return;
    svc_report(SERVICE_START_PENDING, 3000);

    CreateDirectoryA(DATA_DIR, NULL);
    InitializeCriticalSection(&g_log_cs);
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);

    g_stop_event   = CreateEventW(NULL, TRUE,  FALSE, NULL);
    g_update_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    config_load();
    ULOG("=== MassUpdater service starting (v%s) ===", g_current_version);

    svc_report(SERVICE_RUNNING, 0);

    /* Start threads */
    HANDLE threads[2];
    threads[0] = CreateThread(NULL, 0, poll_thread,            NULL, 0, NULL);
    threads[1] = CreateThread(NULL, 0, relay_listener_thread,  NULL, 0, NULL);

    WaitForSingleObject(g_stop_event, INFINITE);

    ULOG("Stopping...");
    WaitForMultipleObjects(2, threads, TRUE, 10000);
    CloseHandle(threads[0]); CloseHandle(threads[1]);

    WSACleanup();
    DeleteCriticalSection(&g_log_cs);
    ULOG("=== MassUpdater stopped ===");
    svc_report(SERVICE_STOPPED, 0);
}

/* ── Install / uninstall ──────────────────────────────────────────────────── */
static void svc_install(void) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { wprintf(L"OpenSCManager failed: %lu\n", GetLastError()); return; }
    SC_HANDLE svc = CreateServiceW(scm, SVC_NAME, SVC_DISPLAY,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        path, NULL, NULL, NULL, NULL, NULL);
    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_EXISTS) wprintf(L"Already installed\n");
        else wprintf(L"CreateService failed: %lu\n", GetLastError());
    } else {
        SERVICE_DESCRIPTIONW d = { (LPWSTR)SVC_DESC };
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &d);
        SC_ACTION acts[3]={{SC_ACTION_RESTART,5000},{SC_ACTION_RESTART,15000},{SC_ACTION_RESTART,30000}};
        SERVICE_FAILURE_ACTIONSW fa={0}; fa.cActions=3; fa.lpsaActions=acts; fa.dwResetPeriod=86400;
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
        wprintf(L"MassUpdater installed\n");
        StartServiceW(svc, 0, NULL);
        wprintf(L"MassUpdater started\n");
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

static void svc_uninstall(void) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME, SERVICE_STOP | DELETE);
    if (svc) {
        SERVICE_STATUS ss; ControlService(svc, SERVICE_CONTROL_STOP, &ss);
        Sleep(2000); DeleteService(svc); CloseServiceHandle(svc);
        wprintf(L"MassUpdater uninstalled\n");
    }
    CloseServiceHandle(scm);
}

/* ── Entry point ──────────────────────────────────────────────────────────── */
int wmain(int argc, wchar_t *argv[]) {
    if (argc > 1) {
        if (wcscmp(argv[1], L"--install")   == 0) { svc_install();   return 0; }
        if (wcscmp(argv[1], L"--uninstall") == 0) { svc_uninstall(); return 0; }
        if (wcscmp(argv[1], L"--run") == 0) {
            CreateDirectoryA(DATA_DIR, NULL);
            InitializeCriticalSection(&g_log_cs);
            WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
            g_stop_event   = CreateEventW(NULL, TRUE,  FALSE, NULL);
            g_update_event = CreateEventW(NULL, FALSE, FALSE, NULL);
            config_load();
            printf("MassUpdater running in console mode. Ctrl+C to stop.\n");
            HANDLE t = CreateThread(NULL, 0, poll_thread, NULL, 0, NULL);
            HANDLE t2 = CreateThread(NULL, 0, relay_listener_thread, NULL, 0, NULL);
            WaitForSingleObject(t, INFINITE);
            (void)t2; return 0;
        }
    }
    SERVICE_TABLE_ENTRYW tbl[] = { {(LPWSTR)SVC_NAME, svc_main}, {NULL,NULL} };
    StartServiceCtrlDispatcherW(tbl);
    return 0;
}
