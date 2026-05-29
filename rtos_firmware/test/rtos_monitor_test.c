/**
 * @file rtos_monitor_test.c
 * @brief P3 monitor heartbeat 和 latency host 单测。
 * @author Yukikaze
 */
#include "rtos_monitor.h"

#include <stdio.h>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                          #condition);                                              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

/**
 * @brief 验证 Linux heartbeat 状态转换和恢复触发 Recovery。
 *
 * @return 0 表示通过。
 */
static int test_linux_heartbeat_transitions(void)
{
    rtos_monitor_context_t monitor;              /**< monitor 上下文。 */
    rtos_linux_heartbeat_snapshot_t heartbeat;   /**< heartbeat 快照。 */
    rtos_error_state_snapshot_t error_state;     /**< 错误快照。 */
    rtos_recovery_snapshot_t recovery;           /**< recovery 快照。 */

    rtos_monitor_context_init(&monitor);
    rtos_monitor_observe_linux_heartbeat(&monitor, 1u, 0u);
    CHECK(rtos_monitor_poll_linux_heartbeat(&monitor, 299u) ==
          RTOS_LINUX_HEARTBEAT_STATE_NORMAL);
    CHECK(rtos_monitor_poll_linux_heartbeat(&monitor, 300u) ==
          RTOS_LINUX_HEARTBEAT_STATE_WARNING);
    CHECK(rtos_monitor_poll_linux_heartbeat(&monitor, 500u) ==
          RTOS_LINUX_HEARTBEAT_STATE_SUSPECTED_ABNORMAL);
    CHECK(rtos_monitor_poll_linux_heartbeat(&monitor, 1000u) ==
          RTOS_LINUX_HEARTBEAT_STATE_GLOBAL_DEGRADED);

    rtos_monitor_get_linux_heartbeat_snapshot(&monitor, &heartbeat);
    CHECK(heartbeat.timeout_count == 1u);
    rtos_monitor_get_error_state_snapshot(&monitor, &error_state);
    CHECK((error_state.error_bits & RTOS_MONITOR_ERROR_LINUX_HEARTBEAT) != 0u);

    rtos_monitor_observe_linux_heartbeat(&monitor, 2u, 1001u);
    rtos_monitor_get_linux_heartbeat_snapshot(&monitor, &heartbeat);
    CHECK(heartbeat.state == RTOS_LINUX_HEARTBEAT_STATE_NORMAL);
    CHECK(heartbeat.recover_count == 1u);
    rtos_monitor_get_recovery_snapshot(&monitor, &recovery);
    CHECK(recovery.recovery_pending);
    CHECK((recovery.trigger_bits & RTOS_RECOVERY_TRIGGER_LINUX_HEARTBEAT) != 0u);
    CHECK(monitor.statistics.recovery_count == 1u);
    CHECK(monitor.statistics.linux_heartbeat_recover_count == 1u);

    return 0;
}

/**
 * @brief 验证 latency max / p95 / p99 统计。
 *
 * @return 0 表示通过。
 */
static int test_latency_statistics(void)
{
    rtos_monitor_context_t monitor;                  /**< monitor 上下文。 */
    rtos_monitor_statistics_snapshot_t statistics;   /**< 统计快照。 */

    rtos_monitor_context_init(&monitor);
    rtos_monitor_record_latency(&monitor, 1u, 10u);
    rtos_monitor_record_latency(&monitor, 1u, 20u);
    rtos_monitor_record_latency(&monitor, 1u, 30u);
    rtos_monitor_get_statistics_snapshot(&monitor, &statistics);
    CHECK(statistics.latency_total.count == 3u);
    CHECK(statistics.latency_total.max_ms == 30u);
    CHECK(statistics.latency_total.p95_ms == 30u);
    CHECK(statistics.latency_total.p99_ms == 30u);
    CHECK(statistics.latency_by_priority[1].count == 3u);
    CHECK(statistics.latency_by_priority[1].max_ms == 30u);

    return 0;
}

int main(void)
{
    CHECK(test_linux_heartbeat_transitions() == 0);
    CHECK(test_latency_statistics() == 0);
    return 0;
}
