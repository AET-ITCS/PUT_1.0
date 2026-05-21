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

void rtos_status_inc_drop_null(void)
{
    ++g_status.drop_null;
}

void rtos_status_inc_drop_flag(void)
{
    ++g_status.drop_flag;
}

void rtos_status_inc_drop_can_id(void)
{
    ++g_status.drop_can_id;
}

void rtos_status_inc_drop_dlc(void)
{
    ++g_status.drop_dlc;
}

void rtos_status_inc_rx_from_can(void)
{
    ++g_status.rx_from_can;
}

void rtos_status_inc_tx_to_linux(void)
{
    ++g_status.tx_to_linux;
}

void rtos_status_inc_drop_ring_full(void)
{
    ++g_status.drop_ring_full;
}

void rtos_status_inc_ipc_payload_drop(void)
{
    ++g_status.ipc_payload_drop;
}

void rtos_status_inc_rx_overrun(void)
{
    ++g_status.rx_overrun;
}

void rtos_status_inc_xl2515_rx_overflow(void)
{
    ++g_status.xl2515_rx_overflow;
}

void rtos_status_inc_spi_error(void)
{
    ++g_status.spi_error;
}

void rtos_status_inc_can_bus_off(void)
{
    ++g_status.can_bus_off;
}

void rtos_status_inc_can_error_passive(void)
{
    ++g_status.can_error_passive;
}

void rtos_status_inc_linux_heartbeat_timeout(void)
{
    ++g_status.linux_heartbeat_timeout;
}

void rtos_status_inc_linux_offline_enter(void)
{
    ++g_status.linux_offline_enter;
}

void rtos_status_add_tx_queue_purged(uint32_t count)
{
    g_status.tx_queue_purged += count;
}

void rtos_status_inc_xl2515_tx_aborted(void)
{
    ++g_status.xl2515_tx_aborted;
}

void rtos_status_inc_listen_only_enter(void)
{
    ++g_status.listen_only_enter;
}

void rtos_status_inc_linux_rehandshake_ok(void)
{
    ++g_status.linux_rehandshake_ok;
}
