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

static const char *interface_name_from_id(uint32_t interface_id)
{
    switch (interface_id) {
    case PUT_SHM_INTERFACE_CAN:
        return "can";
    case PUT_SHM_INTERFACE_ETHERNET:
        return "ethernet";
    case PUT_SHM_INTERFACE_WIFI:
        return "wifi";
    case PUT_SHM_INTERFACE_BLUETOOTH:
        return "bluetooth";
    case PUT_SHM_INTERFACE_4G:
        return "4g";
    case PUT_SHM_INTERFACE_RS485:
        return "rs485";
    default:
        return "unknown";
    }
}

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
    if ((dst == 0) || (dst_size == 0u)) {
        return;
    }

    (void)snprintf(dst, dst_size, "%s", (src == 0) ? "" : src);
}

const char *status_module_name(status_module_id_t module_id)
{
    switch (module_id) {
    case STATUS_MODULE_CAN:
        return "can";
    case STATUS_MODULE_ETHERNET:
        return "ethernet";
    case STATUS_MODULE_WIFI:
        return "wifi";
    case STATUS_MODULE_BLUETOOTH:
        return "bluetooth";
    case STATUS_MODULE_4G:
        return "4g";
    case STATUS_MODULE_RS485:
        return "rs485";
    default:
        return "unknown";
    }
}

const char *status_module_display_protocol(status_module_id_t module_id)
{
    switch (module_id) {
    case STATUS_MODULE_CAN:
        return "CAN";
    case STATUS_MODULE_ETHERNET:
        return "Ethernet UDP/TCP raw";
    case STATUS_MODULE_WIFI:
        return "Wi-Fi UDP/TCP raw";
    case STATUS_MODULE_BLUETOOTH:
        return "Bluetooth";
    case STATUS_MODULE_4G:
        return "4G";
    case STATUS_MODULE_RS485:
        return "RS485";
    default:
        return "unknown";
    }
}

static int mkdir_p(const char *path)
{
    char tmp[STATUS_COLLECTOR_PATH_MAX];
    size_t len;

    if ((path == 0) || (path[0] == '\0')) {
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
    const unsigned char *p = (const unsigned char *)((text == 0) ? "" : text);

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
        return ((module->last_error_ms != 0u) && (module->last_error_ms > module->last_tx_ms)) ?
            "error" : "offline";
    }

    if ((module->last_error_ms != 0u) &&
        (module->last_error_ms > module->last_rx_ms) &&
        (module->last_error_ms > module->last_tx_ms)) {
        return "error";
    }

    if (!module->running) {
        return "unknown";
    }

    if ((module->last_rx_ms != 0u) &&
        (now_ms > module->last_rx_ms) &&
        ((now_ms - module->last_rx_ms) > STATUS_STALE_MS)) {
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

    if (module->rx_frames == 0u) {
        return "module running; no packet received yet";
    }

    return "interface active";
}

static int write_modules_json(FILE *fp, const status_collector_t *collector, uint64_t now_ms)
{
    const char *top_state = "ok";

    for (int i = 0; i < (int)STATUS_MODULE_COUNT; ++i) {
        const char *state = module_state(&collector->modules[i], now_ms);
        if (strcmp(state, "error") == 0) {
            top_state = "error";
            break;
        }
        if ((strcmp(state, "stale") == 0) || (strcmp(state, "offline") == 0)) {
            top_state = "warn";
        }
    }

    fprintf(fp,
            "{\n"
            "  \"updated_at_ms\":%" PRIu64 ",\n"
            "  \"state\":\"%s\",\n"
            "  \"modules\":[\n",
            now_ms,
            top_state);

    for (int i = 0; i < (int)STATUS_MODULE_COUNT; ++i) {
        const status_module_snapshot_t *m = &collector->modules[i];
        const char *state = module_state(m, now_ms);
        const char *message = module_message(m, now_ms);

        fputs("    {\"name\":", fp);
        fprint_json_string(fp, m->name);
        fprintf(fp,
                ",\"status\":\"%s\",\"rx_bytes\":%" PRIu64
                ",\"tx_bytes\":%" PRIu64 ",\"rx_frames\":%" PRIu64
                ",\"tx_frames\":%" PRIu64 ",\"decode_error_count\":%" PRIu64
                ",\"fragment_drop_count\":%" PRIu64
                ",\"reassemble_timeout_count\":%" PRIu64
                ",\"crc_error_count\":%" PRIu64
                ",\"send_fail_count\":%" PRIu64
                ",\"interface_offline_count\":%" PRIu64
                ",\"last_rx_ms\":%" PRIu64 ",\"last_tx_ms\":%" PRIu64
                ",\"last_error\":",
                state,
                m->rx_bytes,
                m->tx_bytes,
                m->rx_frames,
                m->tx_frames,
                m->decode_error_count,
                m->fragment_drop_count,
                m->reassemble_timeout_count,
                m->crc_error_count,
                m->send_fail_count,
                m->interface_offline_count,
                m->last_rx_ms,
                m->last_tx_ms);
        fprint_json_string(fp, (m->last_error_message[0] == '\0') ? "none" : m->last_error_message);
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

static void write_ring_array(FILE *fp, const char *name, const linux_shm_ring_stats_t rings[PUT_SHM_INTERFACE_COUNT])
{
    fprintf(fp, "  \"%s\":[\n", name);
    for (uint32_t i = 0u; i < PUT_SHM_INTERFACE_COUNT; ++i) {
        fprintf(fp,
                "    {\"interface\":\"%s\",\"capacity\":%" PRIu64
                ",\"used\":%" PRIu64 ",\"high_watermark\":%" PRIu64
                ",\"full_count\":%" PRIu64 "}%s\n",
                interface_name_from_id(i),
                rings[i].capacity,
                rings[i].used,
                rings[i].high_watermark,
                rings[i].full_count,
                (i + 1u < PUT_SHM_INTERFACE_COUNT) ? "," : "");
    }
    fputs("  ]", fp);
}

static int write_ipc_status_json(FILE *fp, const status_collector_t *collector, uint64_t now_ms)
{
    const linux_shm_ipc_stats_t *stats = &collector->ipc_stats;

    if (!collector->ipc_stats_valid) {
        fprintf(fp,
                "{\n"
                "  \"updated_at_ms\":%" PRIu64 ",\n"
                "  \"state\":\"unknown\",\n"
                "  \"rtos_online\":false,\n"
                "  \"frame_pool\":{\"capacity\":0,\"used\":0,\"pending_reclaim\":0,\"leaked_suspect\":0},\n"
                "  \"rx_rings\":[],\n"
                "  \"tx_rings\":[]\n"
                "}\n",
                now_ms);
        return ferror(fp) ? -1 : 0;
    }

    fprintf(fp,
            "{\n"
            "  \"updated_at_ms\":%" PRIu64 ",\n"
            "  \"state\":\"ok\",\n"
            "  \"rtos_online\":%s,\n"
            "  \"heartbeat_ms\":%" PRIu64 ",\n"
            "  \"frame_pool\":{\"capacity\":%" PRIu64 ",\"used\":%" PRIu64
            ",\"high_watermark\":%" PRIu64 ",\"full_count\":%" PRIu64
            ",\"allocated\":%" PRIu64 ",\"released\":%" PRIu64
            ",\"pending_reclaim\":%" PRIu64 ",\"leaked_suspect\":%" PRIu64 "},\n",
            now_ms,
            collector->rtos_online ? "true" : "false",
            collector->heartbeat_ms,
            stats->frame_pool.capacity,
            stats->frame_pool.used,
            stats->frame_pool.high_watermark,
            stats->frame_pool.full_count,
            stats->frame_pool.allocated,
            stats->frame_pool.released,
            stats->frame_pool.pending_reclaim,
            stats->frame_pool.leaked_suspect);

    write_ring_array(fp, "rx_rings", stats->rx_rings);
    fputs(",\n", fp);
    write_ring_array(fp, "tx_rings", stats->tx_rings);
    fputs(",\n", fp);

    fprintf(fp,
            "  \"pending_bitmap\":{\"rx\":\"0x%02X\",\"tx\":\"0x%02X\"},\n"
            "  \"mailbox\":{\"rx_doorbell_count\":%" PRIu64
            ",\"tx_doorbell_count\":%" PRIu64
            ",\"notify_fail_count\":%" PRIu64
            ",\"periodic_drain_count\":%" PRIu64 "},\n"
            "  \"integrity\":{\"descriptor_crc_error_count\":%" PRIu64
            ",\"epoch_mismatch_count\":0,\"cache_sync_error_count\":%" PRIu64 "},\n"
            "  \"reclaim\":{\"heartbeat_consumed\":%" PRIu64
            ",\"invalid_frame_reclaimed\":%" PRIu64
            ",\"no_route_reclaimed\":%" PRIu64
            ",\"ttl_expired_reclaimed\":%" PRIu64
            ",\"epoch_mismatch_reclaimed\":%" PRIu64
            ",\"reclaim_ring_used\":%" PRIu64
            ",\"reclaim_ack_count\":%" PRIu64 "}\n"
            "}\n",
            stats->rx_pending_bits,
            stats->tx_pending_bits,
            stats->mailbox.rx_doorbell_count,
            stats->mailbox.tx_doorbell_count,
            stats->mailbox.notify_fail_count,
            stats->mailbox.periodic_drain_count,
            stats->descriptor_crc_error_count,
            stats->cache_sync_error_count,
            stats->reclaim_reason_count[PUT_SHM_RECLAIM_REASON_HEARTBEAT_CONSUMED],
            stats->reclaim_reason_count[PUT_SHM_RECLAIM_REASON_INVALID_FRAME],
            stats->reclaim_reason_count[PUT_SHM_RECLAIM_REASON_NO_ROUTE],
            stats->reclaim_reason_count[PUT_SHM_RECLAIM_REASON_TTL_EXPIRED],
            stats->reclaim_reason_count[PUT_SHM_RECLAIM_REASON_EPOCH_MISMATCH],
            stats->reclaim_ring_used,
            stats->reclaim_ack_count);

    return ferror(fp) ? -1 : 0;
}

static int write_route_status_json(FILE *fp, const status_collector_t *collector, uint64_t now_ms)
{
    (void)collector;
    fprintf(fp,
            "{\n"
            "  \"updated_at_ms\":%" PRIu64 ",\n"
            "  \"state\":\"unknown\",\n"
            "  \"route_table\":{\"version\":0,\"epoch\":0,\"source\":\"unknown\",\"active_entries\":0},\n"
            "  \"priority_queues\":[],\n"
            "  \"cid_stats\":{\"routed_frames\":0,\"heartbeat_consumed\":0,\"no_route\":0,\"invalid_cid\":0,\"reserved_cid\":0,\"broadcast_frames\":0},\n"
            "  \"drop_reasons\":{\"invalid_length\":0,\"invalid_type\":0,\"ttl_expired\":0,\"frame_pool_full\":0,\"rx_ring_full\":0,\"tx_ring_full\":0,\"target_interface_offline\":0,\"auth_failed\":0,\"integrity_failed\":0,\"replay_dropped\":0},\n"
            "  \"latency\":{\"rx_ring_to_tx_ring_max_ms\":0,\"rx_ring_to_tx_ring_avg_ms\":0,\"linux_egress_max_ms\":0,\"end_to_end_max_ms\":0}\n"
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
    if (fp == 0) {
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

    if ((collector == 0) || !collector->enabled || (module_id >= STATUS_MODULE_COUNT)) {
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
    if (fp == 0) {
        return;
    }

    fprintf(fp,
            "{\"timestamp_ms\":%" PRIu64 ",\"level\":\"warn\",\"source\":\"adapter\",\"message\":",
            now_ms);
    fprint_json_string(fp, "pipeline error");
    fputs(",\"detail\":", fp);
    char detail[STATUS_COLLECTOR_TEXT_MAX * 2u];
    (void)snprintf(detail,
                   sizeof(detail),
                   "module=%s stage=%s error=%d",
                   module->name,
                   (stage == 0) ? "unknown" : stage,
                   (int)err);
    fprint_json_string(fp, detail);
    fputs("}\n", fp);
    (void)fclose(fp);
}

void status_collector_init(status_collector_t *collector, const char *status_dir, bool enabled)
{
    if (collector == 0) {
        return;
    }

    memset(collector, 0, sizeof(*collector));
    collector->enabled = enabled;
    copy_text(collector->status_dir,
              sizeof(collector->status_dir),
              ((status_dir == 0) || (status_dir[0] == '\0')) ? STATUS_COLLECTOR_DEFAULT_DIR : status_dir);
    collector->updated_at_ms = now_monotonic_ms();
    (void)pthread_mutex_init(&collector->mutex, 0);

    for (int i = 0; i < (int)STATUS_MODULE_COUNT; ++i) {
        status_module_snapshot_t *m = &collector->modules[i];
        copy_text(m->name, sizeof(m->name), status_module_name((status_module_id_t)i));
        copy_text(m->protocol, sizeof(m->protocol), status_module_display_protocol((status_module_id_t)i));
        m->implemented = false;
        m->enabled = false;
        m->last_error_code = UNIFIED_OK;
        m->last_ipc_error_code = UNIFIED_OK;
    }
}

void status_collector_destroy(status_collector_t *collector)
{
    if (collector == 0) {
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

    if ((collector == 0) || (module_id >= STATUS_MODULE_COUNT)) {
        return;
    }

    (void)pthread_mutex_lock(&collector->mutex);
    m = &collector->modules[module_id];
    m->implemented = implemented;
    m->enabled = enabled;
    copy_text(m->protocol, sizeof(m->protocol),
              (protocol == 0) ? status_module_display_protocol(module_id) : protocol);
    copy_text(m->detail, sizeof(m->detail), detail);
    (void)pthread_mutex_unlock(&collector->mutex);
}

void status_collector_mark_running(status_collector_t *collector, status_module_id_t module_id)
{
    if ((collector == 0) || (module_id >= STATUS_MODULE_COUNT)) {
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
    if ((collector == 0) || (module_id >= STATUS_MODULE_COUNT)) {
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

    if ((collector == 0) || (module_id >= STATUS_MODULE_COUNT)) {
        return;
    }

    now_ms = now_monotonic_ms();
    (void)pthread_mutex_lock(&collector->mutex);
    m = &collector->modules[module_id];
    m->rx_frames++;
    m->rx_bytes += (uint64_t)bytes;
    m->last_rx_ms = now_ms;
    m->last_error_message[0] = '\0';
    collector->updated_at_ms = now_ms;
    (void)pthread_mutex_unlock(&collector->mutex);
}

void status_collector_record_tx_ok(status_collector_t *collector, status_module_id_t module_id, size_t bytes)
{
    uint64_t now_ms;
    status_module_snapshot_t *m;

    if ((collector == 0) || (module_id >= STATUS_MODULE_COUNT)) {
        return;
    }

    now_ms = now_monotonic_ms();
    (void)pthread_mutex_lock(&collector->mutex);
    m = &collector->modules[module_id];
    m->tx_frames++;
    m->tx_bytes += (uint64_t)bytes;
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

    if ((collector == 0) || (module_id >= STATUS_MODULE_COUNT)) {
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
                   ((stage == 0) || (stage[0] == '\0')) ? "unknown_stage" : stage,
                   (int)err);

    if ((stage != 0) && ((strstr(stage, "decode") != 0) || (strstr(stage, "parse") != 0))) {
        m->decode_error_count++;
    } else if ((stage != 0) && (strstr(stage, "fragment") != 0)) {
        m->fragment_drop_count++;
    } else if ((stage != 0) && (strstr(stage, "reassemble") != 0)) {
        m->reassemble_timeout_count++;
    } else if ((stage != 0) && (strstr(stage, "crc") != 0)) {
        m->crc_error_count++;
    } else if ((stage != 0) && (strstr(stage, "send") != 0)) {
        m->send_fail_count++;
    } else if ((stage != 0) && ((strstr(stage, "bind") != 0) || (strstr(stage, "socket") != 0))) {
        m->interface_offline_count++;
    }

    if ((stage != 0) && (strstr(stage, "ipc") != 0)) {
        m->ipc_error_count++;
        m->last_ipc_error_ms = now_ms;
        m->last_ipc_error_code = err;
        copy_text(m->last_ipc_error_message, sizeof(m->last_ipc_error_message), m->last_error_message);
    }

    collector->updated_at_ms = now_ms;
    append_event_jsonl_locked(collector, module_id, stage, err, now_ms);
    (void)pthread_mutex_unlock(&collector->mutex);
}

void status_collector_update_ipc_stats(status_collector_t *collector,
                                       const linux_shm_ipc_stats_t *stats,
                                       bool rtos_online,
                                       uint64_t heartbeat_ms)
{
    if ((collector == 0) || (stats == 0)) {
        return;
    }

    (void)pthread_mutex_lock(&collector->mutex);
    collector->ipc_stats = *stats;
    collector->ipc_stats_valid = true;
    collector->rtos_online = rtos_online;
    collector->heartbeat_ms = heartbeat_ms;
    collector->updated_at_ms = now_monotonic_ms();
    (void)pthread_mutex_unlock(&collector->mutex);
}

int status_collector_write_all(status_collector_t *collector)
{
    uint64_t now_ms;
    int rc = 0;

    if ((collector == 0) || !collector->enabled) {
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
    if (write_atomic_json(collector, "route_status.json", write_route_status_json, now_ms) != 0) {
        rc = -1;
    }
    (void)pthread_mutex_unlock(&collector->mutex);

    return rc;
}
