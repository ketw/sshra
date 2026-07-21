/*
 * protocol.h - Shared protocol definitions for ssh-access system
 * Wire format: all messages are length-prefixed JSON over TCP/TLS
 * Port 7744 - relay server default
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define RELAY_PORT          7744
#define RELAY_PORT_STR      "7744"
#define AGENT_BEACON_INTERVAL_S  30   /* heartbeat every 30s */
#define MAX_DEVICE_ID_LEN   64
#ifndef MAX_HOSTNAME_LEN
#define MAX_HOSTNAME_LEN    256
#endif
#define MAX_TOKEN_LEN       128
#define PROTOCOL_VERSION    1

/* Message types (JSON "type" field) */
#define MSG_REGISTER        "register"      /* agent -> relay: announce presence */
#define MSG_HEARTBEAT       "heartbeat"     /* agent -> relay: keepalive + stats */
#define MSG_LIST            "list"          /* manager -> relay: get device list */
#define MSG_CONNECT         "connect"       /* manager -> relay: request tunnel */
#define MSG_TUNNEL_READY    "tunnel_ready"  /* relay -> both: tunnel established */
#define MSG_DEVICE_LIST     "device_list"   /* relay -> manager: list response */
#define MSG_ERROR           "error"         /* any direction */
#define MSG_AUTH            "auth"          /* manager -> relay: authenticate */
#define MSG_AUTH_OK         "auth_ok"
#define MSG_DISCONNECT      "disconnect"

/*
 * Wire format:
 * [4 bytes big-endian uint32 length][length bytes UTF-8 JSON]
 * Max message size: 1MB
 */
#define MAX_MSG_SIZE        (1024 * 1024)

/* Hardware stats included in heartbeat */
typedef struct {
    float   cpu_usage_pct;
    float   cpu_temp_c;          /* from kernel, -1 if unavailable */
    float   gpu_temp_c;          /* -1 if no GPU or unavailable */
    uint64_t ram_used_bytes;
    uint64_t ram_total_bytes;
    uint64_t disk_read_bps;
    uint64_t disk_write_bps;
    uint64_t net_rx_bps;
    uint64_t net_tx_bps;
    float   battery_pct;         /* -1 if desktop/no battery */
    uint32_t uptime_seconds;
} hw_stats_t;

/* Device info sent during registration */
typedef struct {
    char    device_id[MAX_DEVICE_ID_LEN];   /* stable UUID derived from HW */
    char    hostname[MAX_HOSTNAME_LEN];
    char    os_version[128];
    char    label[64];                       /* user-assigned e.g. "Gaming PC" */
    uint16_t ssh_port;                       /* local SSH port */
    char    auth_token[MAX_TOKEN_LEN];       /* shared secret from install */
} device_info_t;

#endif /* PROTOCOL_H */
