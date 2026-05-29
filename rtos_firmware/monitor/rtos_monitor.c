/**
 * @file rtos_monitor.c
 * @brief P1 monitor 占位快照实现。
 * @author Yukikaze
 */
#include "rtos_monitor.h"

#include <string.h>

/**
 * @brief 读取 Linux heartbeat 占位快照。
 *
 * @param out_snapshot 输出 Linux heartbeat 快照。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_linux_heartbeat_get_snapshot(
    rtos_linux_heartbeat_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0) {
        return UNIFIED_ERR_NULL;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    return UNIFIED_OK;
}

/**
 * @brief 读取错误状态占位快照。
 *
 * @param out_snapshot 输出错误状态快照。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_error_state_get_snapshot(
    rtos_error_state_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0) {
        return UNIFIED_ERR_NULL;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    return UNIFIED_OK;
}

/**
 * @brief 读取 recovery 占位快照。
 *
 * @param out_snapshot 输出 recovery 快照。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_recovery_get_snapshot(
    rtos_recovery_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0) {
        return UNIFIED_ERR_NULL;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    return UNIFIED_OK;
}

/**
 * @brief 读取 monitor 统计占位快照。
 *
 * @param out_snapshot 输出 monitor 统计快照。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_monitor_statistics_get_snapshot(
    rtos_monitor_statistics_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0) {
        return UNIFIED_ERR_NULL;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    return UNIFIED_OK;
}
