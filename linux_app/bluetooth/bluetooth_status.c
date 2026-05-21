/* 蓝牙 Web 状态快照实现：多线程互斥写 JSON 快照，供 put-webd 只读展示。 */
#define _POSIX_C_SOURCE 200809L

#include "bluetooth_status.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "ethernet_status.h"

#define BLUETOOTH_STATUS_STALE_MS 10000ull
#define BLUETOOTH_STATUS_TMP_PATH_MAX 320u

/* 全局互斥量，保障以太网线程和蓝牙线程写 modules.json 文件的线程安全，避免写冲突 */
pthread_mutex_t g_status_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 外部以太网状态指针，由 protocol_manager 或以太网模块赋初值 */
extern ethernet_status_t *g_ethernet_status;
/* 本地全局蓝牙状态指针 */
bluetooth_status_t *g_bluetooth_status = NULL;

static uint64_t now_monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }

    return ((uint64_t)ts.tv_sec * 1000ull) + ((uint64_t)ts.tv_nsec / 1000000ull);
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if ((dst == NULL) || (dst_size == 0u)) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    (void)snprintf(dst, dst_size, "%s", src);
}

static int mkdir_p(const char *path)
{
    char tmp[BLUETOOTH_STATUS_PATH_MAX];
    size_t len;

    if ((path == NULL) || (path[0] == '\0')) {
        return -1;
    }

    len = strlen(path);
    if (len >= sizeof(tmp)) {
        return -1;
    }

    (void)snprintf(tmp, sizeof(tmp), "%s", path);
    if ((len > 1u) && (tmp[len - 1u] == '/')) {
        tmp[len - 1u] = '\0';
    }

    for (char *p = tmp + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if ((mkdir(tmp, 0755) != 0) && (errno != EEXIST)) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }

    if ((mkdir(tmp, 0755) != 0) && (errno != EEXIST)) {
        return -1;
    }

    return 0;
}

static void fprint_json_string(FILE *fp, const char *text)
{
    const unsigned char *p = (const unsigned char *)((text == NULL) ? "" : text);

    fputc('"', fp);
    while (*p != '\0') {
        switch (*p) {
        case '"':
            fputs("\\\"", fp);
            break;
        case '\\':
            fputs("\\\\", fp);
            break;
        case '\b':
            fputs("\\b", fp);
            break;
        case '\f':
            fputs("\\f", fp);
            break;
        case '\n':
            fputs("\\n", fp);
            break;
        case '\r':
            fputs("\\r", fp);
            break;
        case '\t':
            fputs("\\t", fp);
            break;
        default:
            if (*p < 0x20u) {
                fprintf(fp, "\\u%04X", (unsigned)*p);
            } else {
                fputc((int)*p, fp);
            }
            break;
        }
        ++p;
    }
    fputc('"', fp);
}

static const char *bluetooth_module_state(const bluetooth_status_t *status, uint64_t now_ms)
{
    if (status->stopped) {
        return "offline";
    }

    if ((status->last_error_ms != 0u) && (status->last_error_ms > status->last_tx_ms)) {
        return "error";
    }

    if (status->connected) {
        return "online";
    }

    if (status->listening) {
        return "listening";
    }

    return "unknown";
}

static const char *ethernet_module_state_fallback(const ethernet_status_t *status, uint64_t now_ms)
{
    if (status->stopped) {
        return "offline";
    }

    if ((status->last_error_ms != 0u) && (status->last_error_ms > status->last_tx_ms)) {
        return "error";
    }

    if (!status->listening) {
        return "unknown";
    }

    if ((status->last_seen_ms != 0u) &&
        (now_ms > status->last_seen_ms) &&
        ((now_ms - status->last_seen_ms) > BLUETOOTH_STATUS_STALE_MS)) {
        return "stale";
    }

    return "online";
}

static void fprint_unknown_module(FILE *fp, const char *name, const char *message)
{
    fputs("    {\"name\":", fp);
    fprint_json_string(fp, name);
    fputs(",\"status\":\"unknown\",\"rx_count\":0,\"tx_count\":0,\"error_count\":0,"
          "\"last_seen_ms\":0,\"message\":",
          fp);
    fprint_json_string(fp, message);
    fputs("}", fp);
}

/* 联合状态写出核心逻辑：合并以太网和蓝牙的统计数据，安全地覆盖 modules.json */
int gateway_status_write_modules_json(const char *status_dir,
                                     bool status_enabled,
                                     const void *eth_status_raw,
                                     const bluetooth_status_t *bt_status)
{
    char final_path[BLUETOOTH_STATUS_TMP_PATH_MAX];
    char tmp_path[BLUETOOTH_STATUS_TMP_PATH_MAX];
    FILE *fp;
    uint64_t now_ms = now_monotonic_ms();
    int rc = 0;

    if (!status_enabled) {
        return 0;
    }

    if (mkdir_p(status_dir) != 0) {
        return -1;
    }

    if ((snprintf(final_path, sizeof(final_path), "%s/modules.json", status_dir) >= (int)sizeof(final_path)) ||
        (snprintf(tmp_path,
                  sizeof(tmp_path),
                  "%s/modules.json.tmp.%ld",
                  status_dir,
                  (long)getpid()) >= (int)sizeof(tmp_path))) {
        return -1;
    }

    fp = fopen(tmp_path, "w");
    if (fp == NULL) {
        return -1;
    }

    // 写入 JSON 头部
    fprintf(fp,
            "{\n"
            "  \"updated_at_ms\":%" PRIu64 ",\n"
            "  \"state\":\"ok\",\n"
            "  \"modules\":[\n",
            now_ms);

    // 1. 写入 4G (填 unknown 占位)
    fprint_unknown_module(fp, "4g", "4G protocol module is not implemented in linux_app first version");
    fputs(",\n", fp);

    // 2. 写入 WiFi (填 unknown 占位)
    fprint_unknown_module(fp, "wifi", "WiFi protocol module is not implemented in linux_app first version");
    fputs(",\n", fp);

    // 3. 写入 蓝牙 (根据实际统计输出)
    if (bt_status != NULL) {
        const char *bt_state = bluetooth_module_state(bt_status, now_ms);
        char bt_message[128];

        if (strcmp(bt_state, "offline") == 0) {
            snprintf(bt_message, sizeof(bt_message), "%s",
                     (bt_status->last_error_message[0] == '\0') ? "Bluetooth service stopped" : bt_status->last_error_message);
        } else if (strcmp(bt_state, "error") == 0) {
            snprintf(bt_message, sizeof(bt_message), "%s",
                     (bt_status->last_error_message[0] == '\0') ? "Bluetooth pipeline failed" : bt_status->last_error_message);
        } else if (strcmp(bt_state, "listening") == 0) {
            snprintf(bt_message, sizeof(bt_message), "Bluetooth RFCOMM listening on Channel %u", bt_status->rfcomm_channel);
        } else if (strcmp(bt_state, "online") == 0) {
            snprintf(bt_message, sizeof(bt_message), "Connected to client %s", bt_status->connected_client_addr);
        } else {
            snprintf(bt_message, sizeof(bt_message), "Bluetooth not ready");
        }

        fprintf(fp,
                "    {\"name\":\"bluetooth\",\"protocol\":\"rfcomm\",\"status\":\"%s\","
                "\"rx_count\":%" PRIu64 ",\"tx_count\":%" PRIu64 ",\"rx_bytes\":%" PRIu64 ","
                "\"error_count\":%" PRIu64 ",\"parse_error_count\":%" PRIu64 ","
                "\"pack_error_count\":%" PRIu64 ",\"ipc_error_count\":%" PRIu64 ","
                "\"last_seen_ms\":%" PRIu64 ",\"last_tx_ms\":%" PRIu64 ","
                "\"last_error_ms\":%" PRIu64 ",\"listen_channel\":%u,\"message\":",
                bt_state,
                bt_status->rx_count,
                bt_status->tx_count,
                bt_status->rx_bytes,
                bt_status->error_count,
                bt_status->parse_error_count,
                bt_status->pack_error_count,
                bt_status->ipc_error_count,
                bt_status->last_seen_ms,
                bt_status->last_tx_ms,
                bt_status->last_error_ms,
                (unsigned)bt_status->rfcomm_channel);
        fprint_json_string(fp, bt_message);
        fputs("},\n", fp);
    } else {
        fprint_unknown_module(fp, "bluetooth", "Bluetooth protocol module is not initialized");
        fputs(",\n", fp);
    }

    // 4. 写入 以太网 (根据实际统计输出)
    const ethernet_status_t *eth = (const ethernet_status_t *)eth_status_raw;
    if (eth != NULL) {
        const char *eth_state = ethernet_module_state_fallback(eth, now_ms);
        char eth_message[128];

        if (strcmp(eth_state, "offline") == 0) {
            snprintf(eth_message, sizeof(eth_message), "%s",
                     (eth->last_error_message[0] == '\0') ? "UDP listener stopped" : eth->last_error_message);
        } else if (strcmp(eth_state, "error") == 0) {
            snprintf(eth_message, sizeof(eth_message), "%s",
                     (eth->last_error_message[0] == '\0') ? "last UDP pipeline step failed" : eth->last_error_message);
        } else if (strcmp(eth_state, "unknown") == 0) {
            snprintf(eth_message, sizeof(eth_message), "%s",
                     (eth->last_error_message[0] == '\0') ? "UDP listener is not ready" : eth->last_error_message);
        } else if (strcmp(eth_state, "stale") == 0) {
            snprintf(eth_message, sizeof(eth_message), "no UDP packet received for more than 10 seconds");
        } else if (eth->rx_count == 0u) {
            snprintf(eth_message, sizeof(eth_message), "UDP socket listening; no packet received yet");
        } else {
            snprintf(eth_message, sizeof(eth_message), "UDP forwarding active");
        }

        fprintf(fp,
                "    {\"name\":\"ethernet\",\"protocol\":\"udp\",\"status\":\"%s\","
                "\"rx_count\":%" PRIu64 ",\"tx_count\":%" PRIu64 ",\"rx_bytes\":%" PRIu64 ","
                "\"error_count\":%" PRIu64 ",\"parse_error_count\":%" PRIu64 ","
                "\"pack_error_count\":%" PRIu64 ",\"ipc_error_count\":%" PRIu64 ","
                "\"last_seen_ms\":%" PRIu64 ",\"last_tx_ms\":%" PRIu64 ","
                "\"last_error_ms\":%" PRIu64 ",\"listen_port\":%u,\"message\":",
                eth_state,
                eth->rx_count,
                eth->tx_count,
                eth->rx_bytes,
                eth->error_count,
                eth->parse_error_count,
                eth->pack_error_count,
                eth->ipc_error_count,
                eth->last_seen_ms,
                eth->last_tx_ms,
                eth->last_error_ms,
                (unsigned)eth->listen_port);
        fprint_json_string(fp, eth_message);
        fputs("},\n", fp);
    } else {
        fprint_unknown_module(fp, "ethernet", "Ethernet protocol module is not initialized");
        fputs(",\n", fp);
    }

    // 5. 写入 RS485 (填 unknown 占位)
    fprint_unknown_module(fp, "rs485", "RS485 protocol module is not implemented in linux_app first version");
    fputs("\n  ]\n}\n", fp);

    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0 || fclose(fp) != 0) {
        (void)remove(tmp_path);
        return -1;
    }

    if (rename(tmp_path, final_path) != 0) {
        (void)remove(tmp_path);
        return -1;
    }

    return 0;
}

static void append_event_jsonl(const bluetooth_status_t *status,
                               const char *stage,
                               unified_error_t err,
                               uint64_t now_ms)
{
    char path[BLUETOOTH_STATUS_TMP_PATH_MAX];
    FILE *fp;

    if (!status->enabled) {
        return;
    }

    if (mkdir_p(status->status_dir) != 0) {
        return;
    }

    if (snprintf(path, sizeof(path), "%s/events.jsonl", status->status_dir) >= (int)sizeof(path)) {
        return;
    }

    fp = fopen(path, "a");
    if (fp == NULL) {
        return;
    }

    fprintf(fp,
            "{\"time_ms\":%" PRIu64 ",\"level\":\"warn\",\"source\":\"linux_app\","
            "\"module\":\"bluetooth\",\"event\":\"pipeline_error\",\"stage\":",
            now_ms);
    fprint_json_string(fp, stage);
    fprintf(fp, ",\"error_code\":%d,\"message\":", (int)err);
    fprint_json_string(fp, status->last_error_message);
    fputs("}\n", fp);
    (void)fclose(fp);
}

void bluetooth_status_init(bluetooth_status_t *status,
                           const char *status_dir,
                           uint8_t rfcomm_channel,
                           bool enabled)
{
    uint64_t now_ms;

    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->enabled = enabled;
    status->rfcomm_channel = rfcomm_channel;
    copy_text(status->status_dir,
              sizeof(status->status_dir),
              (status_dir == NULL || status_dir[0] == '\0') ? BLUETOOTH_STATUS_DEFAULT_DIR : status_dir);

    now_ms = now_monotonic_ms();
    status->started_at_ms = now_ms;
    status->updated_at_ms = now_ms;
    status->last_error_code = UNIFIED_OK;
    status->last_ipc_error_code = UNIFIED_OK;
}

void bluetooth_status_mark_listening(bluetooth_status_t *status)
{
    if (status == NULL) {
        return;
    }

    status->listening = true;
    status->connected = false;
    status->stopped = false;
    status->updated_at_ms = now_monotonic_ms();
}

void bluetooth_status_mark_connected(bluetooth_status_t *status, const char *client_addr)
{
    if (status == NULL) {
        return;
    }

    status->listening = false;
    status->connected = true;
    status->stopped = false;
    status->updated_at_ms = now_monotonic_ms();
    copy_text(status->connected_client_addr, sizeof(status->connected_client_addr), client_addr);
}

void bluetooth_status_mark_disconnected(bluetooth_status_t *status)
{
    if (status == NULL) {
        return;
    }

    status->listening = true;
    status->connected = false;
    status->stopped = false;
    status->updated_at_ms = now_monotonic_ms();
    status->connected_client_addr[0] = '\0';
}

void bluetooth_status_mark_stopped(bluetooth_status_t *status, const char *reason)
{
    if (status == NULL) {
        return;
    }

    status->stopped = true;
    status->listening = false;
    status->connected = false;
    status->updated_at_ms = now_monotonic_ms();
    status->connected_client_addr[0] = '\0';
    copy_text(status->last_error_message,
              sizeof(status->last_error_message),
              (reason == NULL || reason[0] == '\0') ? "Bluetooth server stopped" : reason);
}

void bluetooth_status_record_rx(bluetooth_status_t *status, size_t bytes)
{
    uint64_t now_ms;

    if (status == NULL) {
        return;
    }

    now_ms = now_monotonic_ms();
    status->rx_count++;
    status->rx_bytes += (uint64_t)bytes;
    status->last_seen_ms = now_ms;
    status->updated_at_ms = now_ms;
}

void bluetooth_status_record_tx_ok(bluetooth_status_t *status)
{
    uint64_t now_ms;

    if (status == NULL) {
        return;
    }

    now_ms = now_monotonic_ms();
    status->tx_count++;
    status->last_tx_ms = now_ms;
    status->updated_at_ms = now_ms;
    status->consecutive_error_count = 0u;
    status->last_ipc_error_message[0] = '\0';
}

void bluetooth_status_record_error(bluetooth_status_t *status,
                                   const char *stage,
                                   unified_error_t err)
{
    uint64_t now_ms;

    if (status == NULL) {
        return;
    }

    now_ms = now_monotonic_ms();
    status->error_count++;
    status->consecutive_error_count++;
    status->last_error_ms = now_ms;
    status->last_error_code = err;
    status->updated_at_ms = now_ms;
    copy_text(status->last_error_stage, sizeof(status->last_error_stage), stage);
    (void)snprintf(status->last_error_message,
                   sizeof(status->last_error_message),
                   "%s failed with error=%d",
                   (stage == NULL || stage[0] == '\0') ? "unknown_stage" : stage,
                   (int)err);

    if ((stage != NULL) && (strcmp(stage, "bluetooth_parse_frame") == 0)) {
        status->parse_error_count++;
    } else if ((stage != NULL) && (strcmp(stage, "frame_packer_pack") == 0)) {
        status->pack_error_count++;
    } else if ((stage != NULL) && (strcmp(stage, "ipc_to_rtos_send") == 0)) {
        status->ipc_error_count++;
        status->last_ipc_error_ms = now_ms;
        status->last_ipc_error_code = err;
        copy_text(status->last_ipc_error_message,
                  sizeof(status->last_ipc_error_message),
                  status->last_error_message);
    }

    append_event_jsonl(status, stage, err, now_ms);
}

int bluetooth_status_write_all(bluetooth_status_t *status)
{
    int rc;

    if ((status == NULL) || !status->enabled) {
        return 0;
    }

    /* 利用全局互斥量确保多线程下写快照文件是安全的，以防以太网线程和蓝牙线程写冲突 */
    (void)pthread_mutex_lock(&g_status_mutex);
    g_bluetooth_status = status;
    rc = gateway_status_write_modules_json(status->status_dir,
                                           status->enabled,
                                           g_ethernet_status,
                                           g_bluetooth_status);
    (void)pthread_mutex_unlock(&g_status_mutex);

    return rc;
}
