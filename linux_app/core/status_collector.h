/* 通用 Web 状态快照：维护多协议模块统计并写入 /run/put/status。 */
#ifndef STATUS_COLLECTOR_H
#define STATUS_COLLECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pthread.h>

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STATUS_COLLECTOR_DEFAULT_DIR "/run/put/status"
#define STATUS_COLLECTOR_PATH_MAX 256u
#define STATUS_COLLECTOR_TEXT_MAX 128u

typedef enum {
    STATUS_MODULE_4G = 0,
    STATUS_MODULE_WIFI,
    STATUS_MODULE_BLUETOOTH,
    STATUS_MODULE_ETHERNET,
    STATUS_MODULE_RS485,
    STATUS_MODULE_COUNT
} status_module_id_t;

typedef struct {
    bool implemented;
    bool enabled;
    bool running;
    bool stopped;
    char name[32];
    char protocol[32];
    char detail[STATUS_COLLECTOR_TEXT_MAX];

    uint64_t rx_count;
    uint64_t tx_count;
    uint64_t rx_bytes;
    uint64_t error_count;
    uint64_t parse_error_count;
    uint64_t pack_error_count;
    uint64_t ipc_error_count;

    uint64_t last_seen_ms;
    uint64_t last_tx_ms;
    uint64_t last_error_ms;
    uint64_t last_ipc_error_ms;

    unified_error_t last_error_code;
    unified_error_t last_ipc_error_code;
    char last_error_stage[STATUS_COLLECTOR_TEXT_MAX];
    char last_error_message[STATUS_COLLECTOR_TEXT_MAX];
    char last_ipc_error_message[STATUS_COLLECTOR_TEXT_MAX];
} status_module_snapshot_t;

typedef struct {
    bool enabled;
    char status_dir[STATUS_COLLECTOR_PATH_MAX];
    uint64_t updated_at_ms;
    pthread_mutex_t mutex;
    status_module_snapshot_t modules[STATUS_MODULE_COUNT];
} status_collector_t;

void status_collector_init(status_collector_t *collector, const char *status_dir, bool enabled);
void status_collector_destroy(status_collector_t *collector);
void status_collector_configure_module(status_collector_t *collector,
                                       status_module_id_t module_id,
                                       bool implemented,
                                       bool enabled,
                                       const char *protocol,
                                       const char *detail);
void status_collector_mark_running(status_collector_t *collector, status_module_id_t module_id);
void status_collector_mark_stopped(status_collector_t *collector,
                                   status_module_id_t module_id,
                                   const char *reason);
void status_collector_record_rx(status_collector_t *collector, status_module_id_t module_id, size_t bytes);
void status_collector_record_tx_ok(status_collector_t *collector, status_module_id_t module_id);
void status_collector_record_error(status_collector_t *collector,
                                   status_module_id_t module_id,
                                   const char *stage,
                                   unified_error_t err);
int status_collector_write_all(status_collector_t *collector);

const char *status_module_name(status_module_id_t module_id);

#ifdef __cplusplus
}
#endif

#endif /* STATUS_COLLECTOR_H */
