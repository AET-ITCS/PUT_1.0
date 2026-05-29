/**
 * @file rtos_monitor.c
 * @brief P3 heartbeat、错误监控、Recovery 和统计快照实现。
 * @author Yukikaze
 */
#include "rtos_monitor.h"

#include <string.h>

/**
 * @brief 刷新 latency 窗口的 p95/p99。
 *
 * @param samples latency 样本窗口。
 * @param sample_count 当前有效样本数量。
 * @param out_latency 待更新的 latency 统计。
 */
static void refresh_latency_percentiles(
    const uint32_t samples[RTOS_FIRMWARE_LATENCY_WINDOW_CAPACITY],
    uint32_t sample_count,
    rtos_latency_statistics_t *out_latency)
{
    uint32_t sorted[RTOS_FIRMWARE_LATENCY_WINDOW_CAPACITY]; /**< 排序副本。 */
    uint32_t i;                                             /**< 外层下标。 */
    uint32_t j;                                             /**< 内层下标。 */
    uint32_t value;                                         /**< 插入值。 */
    uint32_t p95_index;                                     /**< p95 下标。 */
    uint32_t p99_index;                                     /**< p99 下标。 */

    if ((samples == 0) || (out_latency == 0) || (sample_count == 0u)) {
        return;
    }

    if (sample_count > RTOS_FIRMWARE_LATENCY_WINDOW_CAPACITY) {
        sample_count = RTOS_FIRMWARE_LATENCY_WINDOW_CAPACITY;
    }

    for (i = 0u; i < sample_count; ++i) {
        sorted[i] = samples[i];
    }

    for (i = 1u; i < sample_count; ++i) {
        value = sorted[i];
        j = i;
        while ((j > 0u) && (sorted[j - 1u] > value)) {
            sorted[j] = sorted[j - 1u];
            j = j - 1u;
        }
        sorted[j] = value;
    }

    p95_index = ((sample_count * 95u) + 99u) / 100u;
    p99_index = ((sample_count * 99u) + 99u) / 100u;
    if (p95_index > 0u) {
        p95_index = p95_index - 1u;
    }
    if (p99_index > 0u) {
        p99_index = p99_index - 1u;
    }
    if (p95_index >= sample_count) {
        p95_index = sample_count - 1u;
    }
    if (p99_index >= sample_count) {
        p99_index = sample_count - 1u;
    }

    out_latency->p95_ms = sorted[p95_index];
    out_latency->p99_ms = sorted[p99_index];
}

/**
 * @brief 根据 Linux heartbeat 未变化时间计算状态。
 *
 * @param unchanged_ms 未变化时间。
 * @return Linux heartbeat 状态。
 */
static rtos_linux_heartbeat_state_t heartbeat_state_from_age(uint32_t unchanged_ms)
{
    if (unchanged_ms >= RTOS_FIRMWARE_LINUX_HEARTBEAT_DEGRADED_MS) {
        return RTOS_LINUX_HEARTBEAT_STATE_GLOBAL_DEGRADED;
    }

    if (unchanged_ms >= RTOS_FIRMWARE_LINUX_HEARTBEAT_SUSPECT_MS) {
        return RTOS_LINUX_HEARTBEAT_STATE_SUSPECTED_ABNORMAL;
    }

    if (unchanged_ms >= RTOS_FIRMWARE_LINUX_HEARTBEAT_WARNING_MS) {
        return RTOS_LINUX_HEARTBEAT_STATE_WARNING;
    }

    return RTOS_LINUX_HEARTBEAT_STATE_NORMAL;
}

/**
 * @brief 初始化 monitor 上下文。
 *
 * @param monitor monitor 上下文。
 */
void rtos_monitor_context_init(rtos_monitor_context_t *monitor)
{
    if (monitor != 0) {
        (void)memset(monitor, 0, sizeof(*monitor));
        monitor->initialized = true;
        monitor->linux_heartbeat.state = RTOS_LINUX_HEARTBEAT_STATE_NORMAL;
    }
}

/**
 * @brief 更新小核 heartbeat seq。
 *
 * @param monitor monitor 上下文。
 * @param now_ms 当前时间。
 */
void rtos_monitor_tick_rtos_heartbeat(rtos_monitor_context_t *monitor,
                                      uint32_t now_ms)
{
    if (monitor == 0) {
        return;
    }

    (void)now_ms;
    monitor->rtos_heartbeat_seq = monitor->rtos_heartbeat_seq + 1u;
    monitor->statistics.rtos_heartbeat_seq = monitor->rtos_heartbeat_seq;
}

/**
 * @brief 注入/观察 Linux heartbeat seq。
 *
 * @param monitor monitor 上下文。
 * @param linux_heartbeat_seq Linux heartbeat seq。
 * @param now_ms 当前时间。
 */
void rtos_monitor_observe_linux_heartbeat(rtos_monitor_context_t *monitor,
                                          uint32_t linux_heartbeat_seq,
                                          uint32_t now_ms)
{
    bool recovered; /**< 本次观察是否从异常恢复。 */

    if (monitor == 0) {
        return;
    }

    if (!monitor->linux_heartbeat.initialized) {
        monitor->linux_heartbeat.initialized = true;
        monitor->linux_heartbeat.last_sequence = linux_heartbeat_seq;
        monitor->linux_heartbeat.last_update_time_ms = now_ms;
        monitor->linux_heartbeat.current_time_ms = now_ms;
        monitor->linux_heartbeat.unchanged_time_ms = 0u;
        monitor->linux_heartbeat.state = RTOS_LINUX_HEARTBEAT_STATE_NORMAL;
        rtos_monitor_clear_error(monitor, RTOS_MONITOR_ERROR_LINUX_HEARTBEAT);
        return;
    }

    if (linux_heartbeat_seq == monitor->linux_heartbeat.last_sequence) {
        return;
    }

    recovered = monitor->linux_heartbeat.state != RTOS_LINUX_HEARTBEAT_STATE_NORMAL;
    monitor->linux_heartbeat.last_sequence = linux_heartbeat_seq;
    monitor->linux_heartbeat.last_update_time_ms = now_ms;
    monitor->linux_heartbeat.current_time_ms = now_ms;
    monitor->linux_heartbeat.unchanged_time_ms = 0u;
    monitor->linux_heartbeat.state = RTOS_LINUX_HEARTBEAT_STATE_NORMAL;
    rtos_monitor_clear_error(monitor, RTOS_MONITOR_ERROR_LINUX_HEARTBEAT);

    if (recovered) {
        monitor->linux_heartbeat.recover_count =
            monitor->linux_heartbeat.recover_count + 1u;
        monitor->statistics.linux_heartbeat_recover_count =
            monitor->statistics.linux_heartbeat_recover_count + 1u;
        rtos_monitor_mark_recovery_pending(monitor,
                                           RTOS_RECOVERY_TRIGGER_LINUX_HEARTBEAT);
    }
}

/**
 * @brief 轮询 Linux heartbeat 是否超时。
 *
 * @param monitor monitor 上下文。
 * @param now_ms 当前时间。
 * @return 当前 Linux heartbeat 状态。
 */
rtos_linux_heartbeat_state_t rtos_monitor_poll_linux_heartbeat(
    rtos_monitor_context_t *monitor,
    uint32_t now_ms)
{
    rtos_linux_heartbeat_state_t previous_state; /**< 轮询前状态。 */
    rtos_linux_heartbeat_state_t next_state;     /**< 轮询后状态。 */

    if (monitor == 0) {
        return RTOS_LINUX_HEARTBEAT_STATE_GLOBAL_DEGRADED;
    }

    if (!monitor->linux_heartbeat.initialized) {
        monitor->linux_heartbeat.current_time_ms = now_ms;
        return RTOS_LINUX_HEARTBEAT_STATE_NORMAL;
    }

    previous_state = monitor->linux_heartbeat.state;
    monitor->linux_heartbeat.current_time_ms = now_ms;
    monitor->linux_heartbeat.unchanged_time_ms =
        now_ms - monitor->linux_heartbeat.last_update_time_ms;
    next_state = heartbeat_state_from_age(monitor->linux_heartbeat.unchanged_time_ms);
    monitor->linux_heartbeat.state = next_state;

    if (next_state == RTOS_LINUX_HEARTBEAT_STATE_NORMAL) {
        rtos_monitor_clear_error(monitor, RTOS_MONITOR_ERROR_LINUX_HEARTBEAT);
    } else {
        rtos_monitor_record_error(monitor, RTOS_MONITOR_ERROR_LINUX_HEARTBEAT, now_ms);
    }

    if ((previous_state != RTOS_LINUX_HEARTBEAT_STATE_GLOBAL_DEGRADED) &&
        (next_state == RTOS_LINUX_HEARTBEAT_STATE_GLOBAL_DEGRADED)) {
        monitor->linux_heartbeat.timeout_count =
            monitor->linux_heartbeat.timeout_count + 1u;
        monitor->statistics.linux_heartbeat_timeout_count =
            monitor->statistics.linux_heartbeat_timeout_count + 1u;
    }

    return next_state;
}

/**
 * @brief 记录 monitor 错误。
 *
 * @param monitor monitor 上下文。
 * @param reason 错误 reason bit。
 * @param now_ms 当前时间。
 */
void rtos_monitor_record_error(rtos_monitor_context_t *monitor,
                               rtos_monitor_error_reason_t reason,
                               uint32_t now_ms)
{
    if (monitor == 0) {
        return;
    }

    monitor->error_state.error_bits = monitor->error_state.error_bits | (uint32_t)reason;
    monitor->error_state.reason_code = (uint32_t)reason;
    monitor->error_state.last_error_time_ms = now_ms;
    monitor->error_state.degraded = monitor->error_state.error_bits != 0u;
}

/**
 * @brief 清除 monitor 错误。
 *
 * @param monitor monitor 上下文。
 * @param reason 错误 reason bit。
 */
void rtos_monitor_clear_error(rtos_monitor_context_t *monitor,
                              rtos_monitor_error_reason_t reason)
{
    if (monitor == 0) {
        return;
    }

    monitor->error_state.error_bits =
        monitor->error_state.error_bits & ~((uint32_t)reason);
    monitor->error_state.degraded = monitor->error_state.error_bits != 0u;
    if (monitor->error_state.error_bits == 0u) {
        monitor->error_state.reason_code = 0u;
    }
}

/**
 * @brief 标记 Recovery 待执行。
 *
 * @param monitor monitor 上下文。
 * @param trigger 触发原因。
 */
void rtos_monitor_mark_recovery_pending(rtos_monitor_context_t *monitor,
                                        rtos_recovery_trigger_t trigger)
{
    if (monitor == 0) {
        return;
    }

    if (!monitor->recovery.recovery_pending && !monitor->recovery.recovery_active) {
        monitor->recovery.recovery_count = monitor->recovery.recovery_count + 1u;
        monitor->statistics.recovery_count = monitor->statistics.recovery_count + 1u;
    }

    monitor->recovery.recovery_pending = true;
    monitor->recovery.trigger_bits = monitor->recovery.trigger_bits | (uint32_t)trigger;
}

/**
 * @brief 标记 Recovery 开始执行。
 *
 * @param monitor monitor 上下文。
 */
void rtos_monitor_mark_recovery_started(rtos_monitor_context_t *monitor)
{
    if (monitor != 0) {
        monitor->recovery.recovery_pending = false;
        monitor->recovery.recovery_active = true;
    }
}

/**
 * @brief 标记 Recovery 完成。
 *
 * @param monitor monitor 上下文。
 */
void rtos_monitor_mark_recovery_done(rtos_monitor_context_t *monitor)
{
    if (monitor != 0) {
        monitor->recovery.recovery_pending = false;
        monitor->recovery.recovery_active = false;
        monitor->recovery.trigger_bits = 0u;
    }
}

/**
 * @brief 记录小核 TX latency。
 *
 * @param monitor monitor 上下文。
 * @param priority priority。
 * @param latency_ms latency，单位毫秒。
 */
void rtos_monitor_record_latency(rtos_monitor_context_t *monitor,
                                 uint8_t priority,
                                 uint32_t latency_ms)
{
    uint32_t sample_index; /**< latency 样本下标。 */

    if (monitor == 0) {
        return;
    }

    sample_index = monitor->latency_sample_index_total %
                   RTOS_FIRMWARE_LATENCY_WINDOW_CAPACITY;
    monitor->latency_samples_total[sample_index] = latency_ms;
    monitor->latency_sample_index_total = monitor->latency_sample_index_total + 1u;
    if (monitor->latency_sample_count_total < RTOS_FIRMWARE_LATENCY_WINDOW_CAPACITY) {
        monitor->latency_sample_count_total =
            monitor->latency_sample_count_total + 1u;
    }
    monitor->statistics.latency_total.count =
        monitor->statistics.latency_total.count + 1u;
    if (latency_ms > monitor->statistics.latency_total.max_ms) {
        monitor->statistics.latency_total.max_ms = latency_ms;
    }
    refresh_latency_percentiles(monitor->latency_samples_total,
                                monitor->latency_sample_count_total,
                                &monitor->statistics.latency_total);

    if (priority >= (uint8_t)RTOS_FIRMWARE_PRIORITY_COUNT) {
        return;
    }

    sample_index = monitor->latency_sample_index_by_priority[priority] %
                   RTOS_FIRMWARE_LATENCY_WINDOW_CAPACITY;
    monitor->latency_samples_by_priority[priority][sample_index] = latency_ms;
    monitor->latency_sample_index_by_priority[priority] =
        monitor->latency_sample_index_by_priority[priority] + 1u;
    if (monitor->latency_sample_count_by_priority[priority] <
        RTOS_FIRMWARE_LATENCY_WINDOW_CAPACITY) {
        monitor->latency_sample_count_by_priority[priority] =
            monitor->latency_sample_count_by_priority[priority] + 1u;
    }
    monitor->statistics.latency_by_priority[priority].count =
        monitor->statistics.latency_by_priority[priority].count + 1u;
    if (latency_ms > monitor->statistics.latency_by_priority[priority].max_ms) {
        monitor->statistics.latency_by_priority[priority].max_ms = latency_ms;
    }
    refresh_latency_percentiles(monitor->latency_samples_by_priority[priority],
                                monitor->latency_sample_count_by_priority[priority],
                                &monitor->statistics.latency_by_priority[priority]);
}

void rtos_monitor_get_linux_heartbeat_snapshot(
    const rtos_monitor_context_t *monitor,
    rtos_linux_heartbeat_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0) {
        return;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    if (monitor != 0) {
        *out_snapshot = monitor->linux_heartbeat;
    }
}

void rtos_monitor_get_error_state_snapshot(
    const rtos_monitor_context_t *monitor,
    rtos_error_state_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0) {
        return;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    if (monitor != 0) {
        *out_snapshot = monitor->error_state;
    }
}

void rtos_monitor_get_recovery_snapshot(
    const rtos_monitor_context_t *monitor,
    rtos_recovery_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0) {
        return;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    if (monitor != 0) {
        *out_snapshot = monitor->recovery;
    }
}

void rtos_monitor_get_statistics_snapshot(
    const rtos_monitor_context_t *monitor,
    rtos_monitor_statistics_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0) {
        return;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    if (monitor != 0) {
        *out_snapshot = monitor->statistics;
    }
}
