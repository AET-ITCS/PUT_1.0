/* FreeRTOS comm 状态统计实现：mock 阶段使用单实例计数器。 */
#include "rtos_status.h"

#include <string.h>

static rtos_status_snapshot_t g_status;

void rtos_status_init(void)
{
    rtos_status_reset();
}

void rtos_status_reset(void)
{
    memset(&g_status, 0, sizeof(g_status));
    g_status.linux_online = true;
}

void rtos_status_get_snapshot(rtos_status_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return;
    }

    *out_snapshot = g_status;
}

void rtos_status_set_can_ready(bool ready)
{
    g_status.can_ready = ready;
}

void rtos_status_set_linux_online(bool online)
{
    g_status.linux_online = online;
}

void rtos_status_inc_rx_from_linux(void)
{
    ++g_status.rx_from_linux;
}

void rtos_status_inc_tx_to_can_ok(void)
{
    ++g_status.tx_to_can_ok;
}

void rtos_status_inc_tx_to_can_fail(void)
{
    ++g_status.tx_to_can_fail;
}

void rtos_status_inc_drop_queue_full(void)
{
    ++g_status.drop_queue_full;
}

void rtos_status_record_validation_error(rtos_frame_validate_error_t error)
{
    switch (error) {
    case RTOS_FRAME_VALIDATE_NULL:
        ++g_status.drop_null;
        break;
    case RTOS_FRAME_VALIDATE_MAGIC:
        ++g_status.drop_magic;
        break;
    case RTOS_FRAME_VALIDATE_VERSION:
        ++g_status.drop_version;
        break;
    case RTOS_FRAME_VALIDATE_TYPE:
        ++g_status.drop_type;
        break;
    case RTOS_FRAME_VALIDATE_SOURCE_PROTOCOL:
        ++g_status.drop_source_protocol;
        break;
    case RTOS_FRAME_VALIDATE_VEHICLE_TYPE:
        ++g_status.drop_vehicle_type;
        break;
    case RTOS_FRAME_VALIDATE_FLAG:
        ++g_status.drop_flag;
        break;
    case RTOS_FRAME_VALIDATE_CAN_ID:
        ++g_status.drop_can_id;
        break;
    case RTOS_FRAME_VALIDATE_DLC:
        ++g_status.drop_dlc;
        break;
    case RTOS_FRAME_VALIDATE_CRC:
        ++g_status.drop_crc;
        break;
    case RTOS_FRAME_VALIDATE_OK:
    default:
        break;
    }
}
