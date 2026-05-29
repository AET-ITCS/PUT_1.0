/**
 * @file rtos_monitor.h
 * @brief P1 monitor 占位快照，供后续心跳、错误和统计模块扩展。
 * @author Yukikaze
 */
#ifndef RTOS_MONITOR_H
#define RTOS_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief P1 Linux heartbeat 占位快照。
 */
typedef struct {
    bool initialized;
    uint32_t last_sequence;
    uint32_t last_update_time_ms;
} rtos_linux_heartbeat_snapshot_t;

/**
 * @brief P1 错误状态占位快照。
 */
typedef struct {
    bool degraded;
    uint32_t reason_code;
} rtos_error_state_snapshot_t;

/**
 * @brief P1 recovery 占位快照。
 */
typedef struct {
    bool recovery_pending;
    uint32_t recovery_epoch;
} rtos_recovery_snapshot_t;

/**
 * @brief P1 monitor 统计占位快照。
 */
typedef struct {
    uint32_t snapshot_sequence;
} rtos_monitor_statistics_snapshot_t;

unified_error_t rtos_linux_heartbeat_get_snapshot(
    rtos_linux_heartbeat_snapshot_t *out_snapshot);
unified_error_t rtos_error_state_get_snapshot(
    rtos_error_state_snapshot_t *out_snapshot);
unified_error_t rtos_recovery_get_snapshot(
    rtos_recovery_snapshot_t *out_snapshot);
unified_error_t rtos_monitor_statistics_get_snapshot(
    rtos_monitor_statistics_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_MONITOR_H */
