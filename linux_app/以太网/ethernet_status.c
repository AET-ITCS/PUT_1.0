/* 以太网 Web 状态快照实现：原子写 JSON，供 put-webd 只读展示。 */
#define _POSIX_C_SOURCE 200809L

#include "ethernet_status.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ETHERNET_STATUS_STALE_MS 10000ull
#define ETHERNET_STATUS_TMP_PATH_MAX 320u

typedef int (*status_writer_t)(FILE *fp, const ethernet_status_t *status, uint64_t now_ms);

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
    char tmp[ETHERNET_STATUS_PATH_MAX];
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

static const char *ethernet_module_state(const ethernet_status_t *status, uint64_t now_ms)
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
        ((now_ms - status->last_seen_ms) > ETHERNET_STATUS_STALE_MS)) {
        return "stale";
    }

    return "online";
}

static const char *ethernet_module_message(const ethernet_status_t *status, uint64_t now_ms)
{
    const char *state = ethernet_module_state(status, now_ms);

    if (strcmp(state, "offline") == 0) {
        return (status->last_error_message[0] == '\0') ? "UDP listener stopped" : status->last_error_message;
    }

    if (strcmp(state, "error") == 0) {
        return (status->last_error_message[0] == '\0') ? "last UDP pipeline step failed" : status->last_error_message;
    }

    if (strcmp(state, "unknown") == 0) {
        return (status->last_error_message[0] == '\0') ? "UDP listener is not ready" : status->last_error_message;
    }

    if (strcmp(state, "stale") == 0) {
        return "no UDP packet received for more than 10 seconds";
    }

    if (status->rx_count == 0u) {
        return "UDP socket listening; no packet received yet";
    }

    return "UDP forwarding active";
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

static int write_modules_json(FILE *fp, const ethernet_status_t *status, uint64_t now_ms)
{
    const char *state = ethernet_module_state(status, now_ms);
    const char *message = ethernet_module_message(status, now_ms);

    fprintf(fp,
            "{\n"
            "  \"updated_at_ms\":%" PRIu64 ",\n"
            "  \"state\":\"ok\",\n"
            "  \"modules\":[\n",
            now_ms);

    fprint_unknown_module(fp, "4g", "4G protocol module is not implemented in linux_app first version");
    fputs(",\n", fp);
    fprint_unknown_module(fp, "wifi", "WiFi protocol module is not implemented in linux_app first version");
    fputs(",\n", fp);
    fprint_unknown_module(fp, "bluetooth", "Bluetooth protocol module is not implemented in linux_app first version");
    fputs(",\n", fp);

    fprintf(fp,
            "    {\"name\":\"ethernet\",\"protocol\":\"udp\",\"status\":\"%s\","
            "\"rx_count\":%" PRIu64 ",\"tx_count\":%" PRIu64 ",\"rx_bytes\":%" PRIu64 ","
            "\"error_count\":%" PRIu64 ",\"parse_error_count\":%" PRIu64 ","
            "\"pack_error_count\":%" PRIu64 ",\"ipc_error_count\":%" PRIu64 ","
            "\"last_seen_ms\":%" PRIu64 ",\"last_tx_ms\":%" PRIu64 ","
            "\"last_error_ms\":%" PRIu64 ",\"listen_port\":%u,\"message\":",
            state,
            status->rx_count,
            status->tx_count,
            status->rx_bytes,
            status->error_count,
            status->parse_error_count,
            status->pack_error_count,
            status->ipc_error_count,
            status->last_seen_ms,
            status->last_tx_ms,
            status->last_error_ms,
            (unsigned)status->listen_port);
    fprint_json_string(fp, message);
    fputs("},\n", fp);

    fprint_unknown_module(fp, "rs485", "RS485 protocol module is not implemented in linux_app first version");
    fputs("\n  ]\n}\n", fp);

    return ferror(fp) ? -1 : 0;
}

static const char *ipc_state(const ethernet_status_t *status)
{
    if (status->stopped) {
        return "offline";
    }

    if ((status->last_ipc_error_ms != 0u) && (status->last_ipc_error_ms > status->last_tx_ms)) {
        return "error";
    }

    if (status->tx_count == 0u) {
        return "unknown";
    }

    return "ok";
}

static int write_ipc_status_json(FILE *fp, const ethernet_status_t *status, uint64_t now_ms)
{
    const char *message;

    if (status->last_ipc_error_message[0] != '\0') {
        message = status->last_ipc_error_message;
    } else if (status->tx_count == 0u) {
        message = "no frame has been sent to ipc_to_rtos yet";
    } else {
        message = "ipc_to_rtos send path is healthy";
    }

    fprintf(fp,
            "{\n"
            "  \"updated_at_ms\":%" PRIu64 ",\n"
            "  \"state\":\"%s\",\n"
            "  \"tx_count\":%" PRIu64 ",\n"
            "  \"error_count\":%" PRIu64 ",\n"
            "  \"last_tx_ms\":%" PRIu64 ",\n"
            "  \"last_error_ms\":%" PRIu64 ",\n"
            "  \"last_error_code\":%d,\n"
            "  \"message\":",
            now_ms,
            ipc_state(status),
            status->tx_count,
            status->ipc_error_count,
            status->last_tx_ms,
            status->last_ipc_error_ms,
            (int)status->last_ipc_error_code);
    fprint_json_string(fp, message);
    fputs("\n}\n", fp);

    return ferror(fp) ? -1 : 0;
}

static int write_can_status_json(FILE *fp, const ethernet_status_t *status, uint64_t now_ms)
{
    (void)status;

    fprintf(fp,
            "{\n"
            "  \"updated_at_ms\":%" PRIu64 ",\n"
            "  \"state\":\"unknown\",\n"
            "  \"bus_state\":\"unknown\",\n"
            "  \"tx_fail_count\":0,\n"
            "  \"bus_error_count\":0,\n"
            "  \"message\":\"RTOS CAN status feedback is not connected in linux_app first version\"\n"
            "}\n",
            now_ms);

    return ferror(fp) ? -1 : 0;
}

static int write_atomic_json(const ethernet_status_t *status,
                             const char *filename,
                             status_writer_t writer,
                             uint64_t now_ms)
{
    char final_path[ETHERNET_STATUS_TMP_PATH_MAX];
    char tmp_path[ETHERNET_STATUS_TMP_PATH_MAX];
    FILE *fp;
    int rc = 0;

    if (mkdir_p(status->status_dir) != 0) {
        return -1;
    }

    if ((snprintf(final_path, sizeof(final_path), "%s/%s", status->status_dir, filename) >=
         (int)sizeof(final_path)) ||
        (snprintf(tmp_path,
                  sizeof(tmp_path),
                  "%s/%s.tmp.%ld",
                  status->status_dir,
                  filename,
                  (long)getpid()) >= (int)sizeof(tmp_path))) {
        return -1;
    }

    fp = fopen(tmp_path, "w");
    if (fp == NULL) {
        return -1;
    }

    if (writer(fp, status, now_ms) != 0) {
        rc = -1;
    }

    if ((fflush(fp) != 0) || (fsync(fileno(fp)) != 0)) {
        rc = -1;
    }

    if (fclose(fp) != 0) {
        rc = -1;
    }

    if ((rc == 0) && (rename(tmp_path, final_path) != 0)) {
        rc = -1;
    }

    if (rc != 0) {
        (void)remove(tmp_path);
    }

    return rc;
}

static void append_event_jsonl(const ethernet_status_t *status,
                               const char *stage,
                               unified_error_t err,
                               uint64_t now_ms)
{
    char path[ETHERNET_STATUS_TMP_PATH_MAX];
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
            "\"module\":\"ethernet\",\"event\":\"pipeline_error\",\"stage\":",
            now_ms);
    fprint_json_string(fp, stage);
    fprintf(fp, ",\"error_code\":%d,\"message\":", (int)err);
    fprint_json_string(fp, status->last_error_message);
    fputs("}\n", fp);
    (void)fclose(fp);
}

void ethernet_status_init(ethernet_status_t *status,
                          const char *status_dir,
                          uint16_t listen_port,
                          bool enabled)
{
    uint64_t now_ms;

    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->enabled = enabled;
    status->listen_port = listen_port;
    copy_text(status->status_dir,
              sizeof(status->status_dir),
              (status_dir == NULL || status_dir[0] == '\0') ? ETHERNET_STATUS_DEFAULT_DIR : status_dir);

    now_ms = now_monotonic_ms();
    status->started_at_ms = now_ms;
    status->updated_at_ms = now_ms;
    status->last_error_code = UNIFIED_OK;
    status->last_ipc_error_code = UNIFIED_OK;
}

void ethernet_status_mark_listening(ethernet_status_t *status)
{
    if (status == NULL) {
        return;
    }

    status->listening = true;
    status->stopped = false;
    status->updated_at_ms = now_monotonic_ms();
}

void ethernet_status_mark_stopped(ethernet_status_t *status, const char *reason)
{
    if (status == NULL) {
        return;
    }

    status->stopped = true;
    status->listening = false;
    status->updated_at_ms = now_monotonic_ms();
    copy_text(status->last_error_message,
              sizeof(status->last_error_message),
              (reason == NULL || reason[0] == '\0') ? "UDP listener stopped" : reason);
}

void ethernet_status_record_rx(ethernet_status_t *status, size_t bytes)
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

void ethernet_status_record_tx_ok(ethernet_status_t *status)
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

void ethernet_status_record_error(ethernet_status_t *status,
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

    if ((stage != NULL) && (strcmp(stage, "ethernet_udp_parse_frame") == 0)) {
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

int ethernet_status_write_all(ethernet_status_t *status)
{
    uint64_t now_ms;
    int rc = 0;

    if ((status == NULL) || !status->enabled) {
        return 0;
    }

    now_ms = now_monotonic_ms();
    status->updated_at_ms = now_ms;

    // 联合状态写出：在以太网侧使用相同的全局互斥锁保护 modules.json 写入，避免写冲突
    extern pthread_mutex_t g_status_mutex;
    extern void *g_bluetooth_status; // 声明为 void* 以规避头文件依赖
    extern int gateway_status_write_modules_json(const char *status_dir,
                                                 bool status_enabled,
                                                 const void *eth_status_raw,
                                                 const void *bt_status_raw);

    (void)pthread_mutex_lock(&g_status_mutex);
    if (gateway_status_write_modules_json(status->status_dir, status->enabled, status, g_bluetooth_status) != 0) {
        rc = -1;
    }
    (void)pthread_mutex_unlock(&g_status_mutex);

    if (write_atomic_json(status, "ipc_status.json", write_ipc_status_json, now_ms) != 0) {
        rc = -1;
    }
    if (write_atomic_json(status, "can_status.json", write_can_status_json, now_ms) != 0) {
        rc = -1;
    }

    return rc;
}
