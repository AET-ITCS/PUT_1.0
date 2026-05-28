/**
 * @file rtos_monitor.c
 * @brief P1 monitor placeholder snapshots.
 * @author Yukikaze
 */
#include "rtos_monitor.h"

#include <string.h>

unified_error_t rtos_linux_heartbeat_get_snapshot(
    rtos_linux_heartbeat_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0) {
        return UNIFIED_ERR_NULL;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    return UNIFIED_OK;
}

unified_error_t rtos_error_state_get_snapshot(
    rtos_error_state_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0) {
        return UNIFIED_ERR_NULL;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    return UNIFIED_OK;
}

unified_error_t rtos_recovery_get_snapshot(
    rtos_recovery_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0) {
        return UNIFIED_ERR_NULL;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    return UNIFIED_OK;
}

unified_error_t rtos_monitor_statistics_get_snapshot(
    rtos_monitor_statistics_snapshot_t *out_snapshot)
{
    if (out_snapshot == 0) {
        return UNIFIED_ERR_NULL;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    return UNIFIED_OK;
}
