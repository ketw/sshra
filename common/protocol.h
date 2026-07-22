/*
 * protocol.h - Shared protocol definitions for ms (mass) remote access system
 * Wire format: all messages are length-prefixed JSON over TCP
 * Port 7744 - relay server default
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define RELAY_PORT          7744
#define RELAY_PORT_STR      "7744"
#define AGENT_BEACON_INTERVAL_S  30
#define MAX_DEVICE_ID_LEN   64
#ifndef MAX_HOSTNAME_LEN
#define MAX_HOSTNAME_LEN    256
#endif
#define MAX_TOKEN_LEN       128
#define PROTOCOL_VERSION    1

/* Message types */
#define MSG_REGISTER        "register"
#define MSG_HEARTBEAT       "heartbeat"
#define MSG_LIST            "list"
#define MSG_CONNECT         "connect"
#define MSG_TUNNEL_READY    "tunnel_ready"
#define MSG_DEVICE_LIST     "device_list"
#define MSG_ERROR           "error"
#define MSG_AUTH            "auth"
#define MSG_AUTH_OK         "auth_ok"
#define MSG_DISCONNECT      "disconnect"

/*
 * Wire format:
 * [4 bytes big-endian uint32 length][length bytes UTF-8 JSON]
 */
#define MAX_MSG_SIZE        (1024 * 1024)

typedef struct {
    float    cpu_usage_pct;
    float    cpu_temp_c;
    float    gpu_temp_c;
    uint64_t ram_used_bytes;
    uint64_t ram_total_bytes;
    uint64_t disk_read_bps;
    uint64_t disk_write_bps;
    uint64_t net_rx_bps;
    uint64_t net_tx_bps;
    float    battery_pct;
    uint32_t uptime_seconds;
} hw_stats_t;

typedef struct {
    char     device_id[MAX_DEVICE_ID_LEN];
    char     hostname[MAX_HOSTNAME_LEN];
    char     os_version[128];
    char     label[64];
    uint16_t ssh_port;
    char     auth_token[MAX_TOKEN_LEN];
} device_info_t;

#endif /* PROTOCOL_H */
