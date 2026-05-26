/* 通用 Web 状态快照实现：原子写 JSON/JSONL，供 put-webd 只读展示。 */
#define _POSIX_C_SOURCE 200809L

#include "status_collector.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STATUS_STALE_MS 10000ull
#define STATUS_TMP_PATH_MAX 320u

typedef int (*status_writer_t)(FILE *fp, const status_collector_t *collector, uint64_t now_ms);

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

    (void)snprintf(dst, dst_size, "%s", (src == NULL) ? "" : src);
}

const char *status_module_name(status_module_id_t module_id)
{
    switch (module_id) {
    case STATUS_MODULE_4G:
        return "4g";
    case STATUS_MODULE_WIFI:
        return "wifi";
    case STATUS_MODULE_BLUETOOTH:
        return "bluetooth";
    default:
        return "unknown";
    }
}

static int mkdir_p(const char *path)
{
    char tmp[STATUS_COLLECTOR_PATH_MAX];
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

static const char *module_state(const status_module_snapshot_t *module, uint64_t now_ms)
{
    if (!module->implemented) {
        return "unknown";
    }

    if (!module->enabled) {
        return "disabled";
    }

    if (module->stopped) {
        return ((module->last_error_ms != 0u) && (module->last_error_ms > module->last_tx_ms)) ? "error" : "offline";
    }

    if ((module->last_error_ms != 0u) && (module->last_error_ms > module->last_tx_ms)) {
        return "error";
    }

    if (!module->running) {
        return "unknown";
    }

    if ((module->last_seen_ms != 0u) &&
        (now_ms > module->last_seen_ms) &&
        ((now_ms - module->last_seen_ms) > STATUS_STALE_MS)) {
        return "stale";
    }

    return "online";
}

static const char *module_message(const status_module_snapshot_t *module, uint64_t now_ms)
{
    const char *state = module_state(module, now_ms);

    if (strcmp(state, "disabled") == 0) {
        return "module disabled by config";
    }

    if (strcmp(state, "unknown") == 0) {
        if (!module->implemented) {
            return "module is not implemented in linux_app yet";
        }
        return (module->last_error_message[0] == '\0') ? "module not ready" : module->last_error_message;
    }

    if (strcmp(state, "error") == 0) {
        return (module->last_error_message[0] == '\0') ? "last pipeline step failed" : module->last_error_message;
    }

    if (strcmp(state, "offline") == 0) {
        return (module->last_error_message[0] == '\0') ? "module stopped" : module->last_error_message;
    }

    if (strcmp(state, "stale") == 0) {
        return "no packet received for more than 10 seconds";
    }

    if (module->rx_count == 0u) {
        return "module running; no packet received yet";
    }

    return "protocol forwarding active";
}

static int write_modules_json(FILE *fp, const status_collector_t *collector, uint64_t now_ms)
{
    fprintf(fp,
            "{\n"
            "  \"updated_at_ms\":%" PRIu64 ",\n"
            "  \"state\":\"ok\",\n"
            "  \"modules\":[\n",
            now_ms);

    for (int i = 0; i < (int)STATUS_MODULE_COUNT; ++i) {
        const status_module_snapshot_t *m = &collector->modules[i];
        const char *state = module_state(m, now_ms);
        const char *message = module_message(m, now_ms);

        fputs("    {\"name\":", fp);
        fprint_json_string(fp, m->name);
        fputs(",\"protocol\":", fp);
        fprint_json_string(fp, m->protocol);
        fprintf(fp,
                ",\"status\":\"%s\",\"enabled\":%s,\"rx_count\":%" PRIu64
                ",\"tx_count\":%" PRIu64 ",\"rx_bytes\":%" PRIu64
                ",\"error_count\":%" PRIu64 ",\"parse_error_count\":%" PRIu64
                ",\"pack_error_count\":%" PRIu64 ",\"ipc_error_count\":%" PRIu64
                ",\"last_seen_ms\":%" PRIu64 ",\"last_tx_ms\":%" PRIu64
                ",\"last_error_ms\":%" PRIu64 ",\"detail\":",
                state,
                m->enabled ? "true" : "false",
                m->rx_count,
                m->tx_count,
                m->rx_bytes,
                m->error_count,
                m->parse_error_count,
                m->pack_error_count,
                m->ipc_error_count,
                m->last_seen_ms,
                m->last_tx_ms,
                m->last_error_ms);
        fprint_json_string(fp, m->detail);
        fputs(",\"message\":", fp);
        fprint_json_string(fp, message);
        fputs("}", fp);

        if (i + 1 < (int)STATUS_MODULE_COUNT) {
            fputs(",", fp);
        }
        fputs("\n", fp);
    }

    fputs("  ]\n}\n", fp);
    return ferror(fp) ? -1 : 0;
}

static void aggregate_ipc(const status_collector_t *collector,
                          uint64_t *tx_count,
                          uint64_t *error_count,
                          uint64_t *last_tx_ms,
                          uint64_t *last_error_ms,
                          unified_error_t *last_error_code,
                          const char **last_message)
{
    *tx_count = 0u;
    *error_count = 0u;
    *last_tx_ms = 0u;
    *last_error_ms = 0u;
    *last_error_code = UNIFIED_OK;
    *last_message = "";

    for (int i = 0; i < (int)STATUS_MODULE_COUNT; ++i) {
        const status_module_snapshot_t *m = &collector->modules[i];
        *tx_count += m->tx_count;
        *error_count += m->ipc_error_count;
        if (m->last_tx_ms > *last_tx_ms) {
            *last_tx_ms = m->last_tx_ms;
        }
        if (m->last_ipc_error_ms > *last_error_ms) {
            *last_error_ms = m->last_ipc_error_ms;
            *last_error_code = m->last_ipc_error_code;
            *last_message = m->last_ipc_error_message;
        }
    }
}

static int write_ipc_status_json(FILE *fp, const status_collector_t *collector, uint64_t now_ms)
{
    uint64_t tx_count;
    uint64_t error_count;
    uint64_t last_tx_ms;
    uint64_t last_error_ms;
    unified_error_t last_error_code;
    const char *last_message;
    const char *state;
    const char *message;

    aggregate_ipc(collector, &tx_count, &error_count, &last_tx_ms, &last_error_ms, &last_error_code, &last_message);

    if ((last_error_ms != 0u) && (last_error_ms > last_tx_ms)) {
        state = "error";
        message = (last_message[0] == '\0') ? "ipc_to_rtos send failed" : last_message;
    } else if (tx_count == 0u) {
        state = "unknown";
        message = "no frame has been sent to ipc_to_rtos yet";
    } else {
        state = "ok";
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
            state,
            tx_count,
            error_count,
            last_tx_ms,
            last_error_ms,
            (int)last_error_code);
    fprint_json_string(fp, message);
    fputs("\n}\n", fp);

    return ferror(fp) ? -1 : 0;
}

static int write_can_status_json(FILE *fp, const status_collector_t *collector, uint64_t now_ms)
{
    (void)collector;

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

static int write_atomic_json(const status_collector_t *collector,
                             const char *filename,
                             status_writer_t writer,
                             uint64_t now_ms)
{
    char final_path[STATUS_TMP_PATH_MAX];
    char tmp_path[STATUS_TMP_PATH_MAX];
    FILE *fp;
    int rc = 0;

    if (mkdir_p(collector->status_dir) != 0) {
        return -1;
    }

    if ((snprintf(final_path, sizeof(final_path), "%s/%s", collector->status_dir, filename) >=
         (int)sizeof(final_path)) ||
        (snprintf(tmp_path,
                  sizeof(tmp_path),
                  "%s/%s.tmp.%ld",
                  collector->status_dir,
                  filename,
                  (long)getpid()) >= (int)sizeof(tmp_path))) {
        return -1;
    }

    fp = fopen(tmp_path, "w");
    if (fp == NULL) {
        return -1;
    }

    if (writer(fp, collector, now_ms) != 0) {
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

static void append_event_jsonl_locked(status_collector_t *collector,
                                      status_module_id_t module_id,
                                      const char *stage,
                                      unified_error_t err,
                                      uint64_t now_ms)
{
    char path[STATUS_TMP_PATH_MAX];
    FILE *fp;
    const status_module_snapshot_t *module;

    if ((collector == NULL) || !collector->enabled || (module_id >= STATUS_MODULE_COUNT)) {
        return;
    }

    module = &collector->modules[module_id];

    if (mkdir_p(collector->status_dir) != 0) {
        return;
    }

    if (snprintf(path, sizeof(path), "%s/events.jsonl", collector->status_dir) >= (int)sizeof(path)) {
        return;
    }

    fp = fopen(path, "a");
    if (fp == NULL) {
        return;
    }

    fprintf(fp,
            "{\"time_ms\":%" PRIu64 ",\"level\":\"warn\",\"source\":\"linux_app\","
            "\"module\":",
            now_ms);
    fprint_json_string(fp, module->name);
    fputs(",\"event\":\"pipeline_error\",\"stage\":", fp);
    fprint_json_string(fp, stage);
    fprintf(fp, ",\"error_code\":%d,\"message\":", (int)err);
    fprint_json_string(fp, module->last_error_message);
    fputs("}\n", fp);
    (void)fclose(fp);
}

void status_collector_init(status_collector_t *collector, const char *status_dir, bool enabled)
{
    if (collector == NULL) {
        return;
    }

    memset(collector, 0, sizeof(*collector));
    collector->enabled = enabled;
    copy_text(collector->status_dir,
              sizeof(collector->status_dir),
              ((status_dir == NULL) || (status_dir[0] == '\0')) ? STATUS_COLLECTOR_DEFAULT_DIR : status_dir);
    collector->updated_at_ms = now_monotonic_ms();
    (void)pthread_mutex_init(&collector->mutex, NULL);

    for (int i = 0; i < (int)STATUS_MODULE_COUNT; ++i) {
        status_module_snapshot_t *m = &collector->modules[i];
        copy_text(m->name, sizeof(m->name), status_module_name((status_module_id_t)i));
        copy_text(m->protocol, sizeof(m->protocol), status_module_name((status_module_id_t)i));
        m->implemented = false;
        m->enabled = false;
        m->last_error_code = UNIFIED_OK;
        m->last_ipc_error_code = UNIFIED_OK;
    }
}

void status_collector_destroy(status_collector_t *collector)
{
    if (collector == NULL) {
        return;
    }

    (void)pthread_mutex_destroy(&collector->mutex);
}

void status_collector_configure_module(status_collector_t *collector,
                                       status_module_id_t module_id,
                                       bool implemented,
                                       bool enabled,
                                       const char *protocol,
                                       const char *detail)
{
    status_module_snapshot_t *m;

    if ((collector == NULL) || (module_id >= STATUS_MODULE_COUNT)) {
        return;
    }

    (void)pthread_mutex_lock(&collector->mutex);
    m = &collector->modules[module_id];
    m->implemented = implemented;
    m->enabled = enabled;
    copy_text(m->protocol, sizeof(m->protocol), protocol);
    copy_text(m->detail, sizeof(m->detail), detail);
    (void)pthread_mutex_unlock(&collector->mutex);
}

void status_collector_mark_running(status_collector_t *collector, status_module_id_t module_id)
{
    if ((collector == NULL) || (module_id >= STATUS_MODULE_COUNT)) {
        return;
    }

    (void)pthread_mutex_lock(&collector->mutex);
    collector->modules[module_id].running = true;
    collector->modules[module_id].stopped = false;
    collector->updated_at_ms = now_monotonic_ms();
    (void)pthread_mutex_unlock(&collector->mutex);
}

void status_collector_mark_stopped(status_collector_t *collector,
                                   status_module_id_t module_id,
                                   const char *reason)
{
    if ((collector == NULL) || (module_id >= STATUS_MODULE_COUNT)) {
        return;
    }

    (void)pthread_mutex_lock(&collector->mutex);
    collector->modules[module_id].running = false;
    collector->modules[module_id].stopped = true;
    collector->updated_at_ms = now_monotonic_ms();
    copy_text(collector->modules[module_id].last_error_message,
              sizeof(collector->modules[module_id].last_error_message),
              reason);
    (void)pthread_mutex_unlock(&collector->mutex);
}

void status_collector_record_rx(status_collector_t *collector, status_module_id_t module_id, size_t bytes)
{
    uint64_t now_ms;
    status_module_snapshot_t *m;

    if ((collector == NULL) || (module_id >= STATUS_MODULE_COUNT)) {
        return;
    }

    now_ms = now_monotonic_ms();
    (void)pthread_mutex_lock(&collector->mutex);
    m = &collector->modules[module_id];
    m->rx_count++;
    m->rx_bytes += (uint64_t)bytes;
    m->last_seen_ms = now_ms;
    collector->updated_at_ms = now_ms;
    (void)pthread_mutex_unlock(&collector->mutex);
}

void status_collector_record_tx_ok(status_collector_t *collector, status_module_id_t module_id)
{
    uint64_t now_ms;
    status_module_snapshot_t *m;

    if ((collector == NULL) || (module_id >= STATUS_MODULE_COUNT)) {
        return;
    }

    now_ms = now_monotonic_ms();
    (void)pthread_mutex_lock(&collector->mutex);
    m = &collector->modules[module_id];
    m->tx_count++;
    m->last_tx_ms = now_ms;
    m->last_ipc_error_message[0] = '\0';
    collector->updated_at_ms = now_ms;
    (void)pthread_mutex_unlock(&collector->mutex);
}

void status_collector_record_error(status_collector_t *collector,
                                   status_module_id_t module_id,
                                   const char *stage,
                                   unified_error_t err)
{
    uint64_t now_ms;
    status_module_snapshot_t *m;

    if ((collector == NULL) || (module_id >= STATUS_MODULE_COUNT)) {
        return;
    }

    now_ms = now_monotonic_ms();
    (void)pthread_mutex_lock(&collector->mutex);
    m = &collector->modules[module_id];
    m->error_count++;
    m->last_error_ms = now_ms;
    m->last_error_code = err;
    copy_text(m->last_error_stage, sizeof(m->last_error_stage), stage);
    (void)snprintf(m->last_error_message,
                   sizeof(m->last_error_message),
                   "%s failed with error=%d",
                   ((stage == NULL) || (stage[0] == '\0')) ? "unknown_stage" : stage,
                   (int)err);

    if ((stage != NULL) && (strstr(stage, "parse") != NULL)) {
        m->parse_error_count++;
    } else if ((stage != NULL) && (strcmp(stage, "frame_packer_pack") == 0)) {
        m->pack_error_count++;
    } else if ((stage != NULL) && (strcmp(stage, "ipc_to_rtos_send") == 0)) {
        m->ipc_error_count++;
        m->last_ipc_error_ms = now_ms;
        m->last_ipc_error_code = err;
        copy_text(m->last_ipc_error_message, sizeof(m->last_ipc_error_message), m->last_error_message);
    }

    collector->updated_at_ms = now_ms;
    append_event_jsonl_locked(collector, module_id, stage, err, now_ms);
    (void)pthread_mutex_unlock(&collector->mutex);
}

int status_collector_write_all(status_collector_t *collector)
{
    uint64_t now_ms;
    int rc = 0;

    if ((collector == NULL) || !collector->enabled) {
        return 0;
    }

    now_ms = now_monotonic_ms();
    (void)pthread_mutex_lock(&collector->mutex);
    collector->updated_at_ms = now_ms;
    if (write_atomic_json(collector, "modules.json", write_modules_json, now_ms) != 0) {
        rc = -1;
    }
    if (write_atomic_json(collector, "ipc_status.json", write_ipc_status_json, now_ms) != 0) {
        rc = -1;
    }
    if (write_atomic_json(collector, "can_status.json", write_can_status_json, now_ms) != 0) {
        rc = -1;
    }
    (void)pthread_mutex_unlock(&collector->mutex);

    return rc;
}
