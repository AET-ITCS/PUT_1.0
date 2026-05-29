/**
 * @file rtos_monitor.h
 * @brief P3 heartbeat、错误监控、Recovery 和统计快照接口。
 * @author Yukikaze
 */
#ifndef RTOS_MONITOR_H
#define RTOS_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_firmware_config.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Linux heartbeat 状态。
 */
typedef enum {
    RTOS_LINUX_HEARTBEAT_STATE_NORMAL = 0,
    RTOS_LINUX_HEARTBEAT_STATE_WARNING = 1,
    RTOS_LINUX_HEARTBEAT_STATE_SUSPECTED_ABNORMAL = 2,
    RTOS_LINUX_HEARTBEAT_STATE_GLOBAL_DEGRADED = 3,
} rtos_linux_heartbeat_state_t;

/**
 * @brief P3 监控错误原因 bit。
 */
typedef enum {
    RTOS_MONITOR_ERROR_LINUX_HEARTBEAT = (1u << 0),
    RTOS_MONITOR_ERROR_ENDPOINT_HEARTBEAT = (1u << 1),
    RTOS_MONITOR_ERROR_RX_BACKLOG = (1u << 2),
    RTOS_MONITOR_ERROR_TX_RING_FULL = (1u << 3),
    RTOS_MONITOR_ERROR_PENDING_STUCK = (1u << 4),
    RTOS_MONITOR_ERROR_FRAME_POOL = (1u << 5),
    RTOS_MONITOR_ERROR_ROUTE_DROPS = (1u << 6),
    RTOS_MONITOR_ERROR_MAILBOX_NOTIFY = (1u << 7),
    RTOS_MONITOR_ERROR_SHM_FORMAT = (1u << 8),
    RTOS_MONITOR_ERROR_RECLAIM_BLOCKED = (1u << 9),
} rtos_monitor_error_reason_t;

/**
 * @brief Recovery 触发原因 bit。
 */
typedef enum {
    RTOS_RECOVERY_TRIGGER_LINUX_HEARTBEAT = (1u << 0),
    RTOS_RECOVERY_TRIGGER_LINUX_EPOCH = (1u << 1),
    RTOS_RECOVERY_TRIGGER_SHM_REBUILT = (1u << 2),
    RTOS_RECOVERY_TRIGGER_RING_DESCRIPTOR = (1u << 3),
    RTOS_RECOVERY_TRIGGER_ROUTE_TABLE = (1u << 4),
    RTOS_RECOVERY_TRIGGER_MAILBOX = (1u << 5),
    RTOS_RECOVERY_TRIGGER_RECLAIM_READY = (1u << 6),
} rtos_recovery_trigger_t;

/**
 * @brief Linux heartbeat 快照。
 */
typedef struct {
    bool initialized;
    uint32_t last_sequence;
    uint32_t last_update_time_ms;
    uint32_t current_time_ms;
    uint32_t unchanged_time_ms;
    uint32_t timeout_count;
    uint32_t recover_count;
    rtos_linux_heartbeat_state_t state;
} rtos_linux_heartbeat_snapshot_t;

/**
 * @brief 错误状态快照。
 */
typedef struct {
    bool degraded;
    uint32_t reason_code;
    uint32_t error_bits;
    uint32_t last_error_time_ms;
} rtos_error_state_snapshot_t;

/**
 * @brief recovery 快照。
 */
typedef struct {
    bool recovery_pending;
    bool recovery_active;
    uint32_t recovery_epoch;
    uint32_t recovery_count;
    uint32_t trigger_bits;
} rtos_recovery_snapshot_t;

/**
 * @brief latency 统计快照。
 */
typedef struct {
    uint32_t count;
    uint32_t max_ms;
    uint32_t p95_ms;
    uint32_t p99_ms;
} rtos_latency_statistics_t;

/**
 * @brief monitor 聚合统计快照。
 */
typedef struct {
    uint32_t snapshot_sequence;
    uint32_t rtos_heartbeat_seq;
    uint32_t doorbell_rx_count;
    uint32_t doorbell_tx_count;
    uint32_t rx_ring_drain_count;
    uint32_t tx_ring_write_count;
    uint32_t route_success_count;
    uint32_t route_miss_count;
    uint32_t drop_reason_count[PUT_SHM_RECLAIM_REASON_QUEUE_FULL + 1u];
    uint32_t routed_by_interface[PUT_SHM_INTERFACE_COUNT];
    uint32_t routed_by_priority[RTOS_FIRMWARE_PRIORITY_COUNT];
    uint32_t ttl_drop_count;
    uint32_t epoch_drop_count;
    uint32_t tx_ring_full_count;
    uint32_t reclaim_ring_full_count;
    uint32_t reclaim_blocked_count;
    uint32_t pending_reclaim_retry_count;
    uint32_t auth_failed_drop_count;
    uint32_t integrity_failed_drop_count;
    uint32_t replay_drop_count;
    uint32_t invalid_descriptor_count;
    uint32_t invalid_descriptor_no_reclaim_count;
    uint32_t invalid_anymsg_count;
    uint32_t mailbox_fail_count;
    uint32_t linux_heartbeat_timeout_count;
    uint32_t linux_heartbeat_recover_count;
    uint32_t endpoint_hb_rx_count;
    uint32_t endpoint_hb_invalid_count;
    uint32_t endpoint_hb_timeout_count;
    uint32_t endpoint_hb_recover_count;
    uint32_t endpoint_hb_table_full_count;
    uint32_t recovery_count;
    rtos_latency_statistics_t latency_total;
    rtos_latency_statistics_t latency_by_priority[RTOS_FIRMWARE_PRIORITY_COUNT];
} rtos_monitor_statistics_snapshot_t;

/**
 * @brief P3 monitor 上下文。
 */
typedef struct {
    bool initialized;
    uint32_t rtos_heartbeat_seq;
    rtos_linux_heartbeat_snapshot_t linux_heartbeat;
    rtos_error_state_snapshot_t error_state;
    rtos_recovery_snapshot_t recovery;
    rtos_monitor_statistics_snapshot_t statistics;
    uint32_t latency_samples_total[RTOS_FIRMWARE_LATENCY_WINDOW_CAPACITY];
    uint32_t latency_sample_count_total;
    uint32_t latency_sample_index_total;
    uint32_t latency_samples_by_priority[RTOS_FIRMWARE_PRIORITY_COUNT]
                                            [RTOS_FIRMWARE_LATENCY_WINDOW_CAPACITY];
    uint32_t latency_sample_count_by_priority[RTOS_FIRMWARE_PRIORITY_COUNT];
    uint32_t latency_sample_index_by_priority[RTOS_FIRMWARE_PRIORITY_COUNT];
} rtos_monitor_context_t;

void rtos_monitor_context_init(rtos_monitor_context_t *monitor);

void rtos_monitor_tick_rtos_heartbeat(rtos_monitor_context_t *monitor,
                                      uint32_t now_ms);

void rtos_monitor_observe_linux_heartbeat(rtos_monitor_context_t *monitor,
                                          uint32_t linux_heartbeat_seq,
                                          uint32_t now_ms);

rtos_linux_heartbeat_state_t rtos_monitor_poll_linux_heartbeat(
    rtos_monitor_context_t *monitor,
    uint32_t now_ms);

void rtos_monitor_record_error(rtos_monitor_context_t *monitor,
                               rtos_monitor_error_reason_t reason,
                               uint32_t now_ms);

void rtos_monitor_clear_error(rtos_monitor_context_t *monitor,
                              rtos_monitor_error_reason_t reason);

void rtos_monitor_mark_recovery_pending(rtos_monitor_context_t *monitor,
                                        rtos_recovery_trigger_t trigger);

void rtos_monitor_mark_recovery_started(rtos_monitor_context_t *monitor);

void rtos_monitor_mark_recovery_done(rtos_monitor_context_t *monitor);

void rtos_monitor_record_latency(rtos_monitor_context_t *monitor,
                                 uint8_t priority,
                                 uint32_t latency_ms);

void rtos_monitor_get_linux_heartbeat_snapshot(
    const rtos_monitor_context_t *monitor,
    rtos_linux_heartbeat_snapshot_t *out_snapshot);

void rtos_monitor_get_error_state_snapshot(
    const rtos_monitor_context_t *monitor,
    rtos_error_state_snapshot_t *out_snapshot);

void rtos_monitor_get_recovery_snapshot(
    const rtos_monitor_context_t *monitor,
    rtos_recovery_snapshot_t *out_snapshot);

void rtos_monitor_get_statistics_snapshot(
    const rtos_monitor_context_t *monitor,
    rtos_monitor_statistics_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_MONITOR_H */
