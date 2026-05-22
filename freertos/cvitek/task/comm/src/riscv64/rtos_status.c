/**
 * @file rtos_status.c
 * @brief FreeRTOS comm 状态统计实现。
 *
 * 当前实现使用单实例计数器，适合小核 comm 任务和 host mock 测试共享。
 */
#include "rtos_status.h"

#include <string.h>

static rtos_status_snapshot_t g_status;

/** @brief 初始化状态统计模块。 */
void rtos_status_init(void)
{
    rtos_status_reset();
}

/** @brief 清零状态统计并设置默认在线状态。 */
void rtos_status_reset(void)
{
    memset(&g_status, 0, sizeof(g_status));
    g_status.linux_online = true;
}

/**
 * @brief 获取状态快照。
 * @param[out] out_snapshot 输出快照；NULL 时忽略。
 */
void rtos_status_get_snapshot(rtos_status_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return;
    }

    *out_snapshot = g_status;
}

/** @brief 设置 CAN ready 状态。 */
void rtos_status_set_can_ready(bool ready)
{
    g_status.can_ready = ready;
}

/** @brief 设置 Linux online 状态。 */
void rtos_status_set_linux_online(bool online)
{
    g_status.linux_online = online;
}

/** @brief 递增 rx_from_linux。 */
void rtos_status_inc_rx_from_linux(void)
{
    ++g_status.rx_from_linux;
}

/** @brief 递增 tx_to_can_ok。 */
void rtos_status_inc_tx_to_can_ok(void)
{
    ++g_status.tx_to_can_ok;
}

/** @brief 递增 tx_to_can_fail。 */
void rtos_status_inc_tx_to_can_fail(void)
{
    ++g_status.tx_to_can_fail;
}

/** @brief 递增 drop_queue_full。 */
void rtos_status_inc_drop_queue_full(void)
{
    ++g_status.drop_queue_full;
}

/** @brief 递增 drop_null。 */
void rtos_status_inc_drop_null(void)
{
    ++g_status.drop_null;
}

/** @brief 递增 drop_flag。 */
void rtos_status_inc_drop_flag(void)
{
    ++g_status.drop_flag;
}

/** @brief 递增 drop_can_id。 */
void rtos_status_inc_drop_can_id(void)
{
    ++g_status.drop_can_id;
}

/** @brief 递增 drop_dlc。 */
void rtos_status_inc_drop_dlc(void)
{
    ++g_status.drop_dlc;
}

/** @brief 递增 rx_from_can。 */
void rtos_status_inc_rx_from_can(void)
{
    ++g_status.rx_from_can;
}

/** @brief 递增 tx_to_linux。 */
void rtos_status_inc_tx_to_linux(void)
{
    ++g_status.tx_to_linux;
}

/** @brief 递增 drop_ring_full。 */
void rtos_status_inc_drop_ring_full(void)
{
    ++g_status.drop_ring_full;
}

/** @brief 递增 ipc_payload_drop。 */
void rtos_status_inc_ipc_payload_drop(void)
{
    ++g_status.ipc_payload_drop;
}

/** @brief 递增 rx_overrun。 */
void rtos_status_inc_rx_overrun(void)
{
    ++g_status.rx_overrun;
}

/** @brief 递增 xl2515_rx_overflow。 */
void rtos_status_inc_xl2515_rx_overflow(void)
{
    ++g_status.xl2515_rx_overflow;
}

/** @brief 递增 spi_error。 */
void rtos_status_inc_spi_error(void)
{
    ++g_status.spi_error;
}

/** @brief 递增 can_bus_off。 */
void rtos_status_inc_can_bus_off(void)
{
    ++g_status.can_bus_off;
}

/** @brief 递增 can_error_passive。 */
void rtos_status_inc_can_error_passive(void)
{
    ++g_status.can_error_passive;
}

/** @brief 递增 linux_heartbeat_timeout。 */
void rtos_status_inc_linux_heartbeat_timeout(void)
{
    ++g_status.linux_heartbeat_timeout;
}

/** @brief 递增 linux_offline_enter。 */
void rtos_status_inc_linux_offline_enter(void)
{
    ++g_status.linux_offline_enter;
}

/** @brief 累加 tx_queue_purged。 */
void rtos_status_add_tx_queue_purged(uint32_t count)
{
    g_status.tx_queue_purged += count;
}

/** @brief 递增 xl2515_tx_aborted。 */
void rtos_status_inc_xl2515_tx_aborted(void)
{
    ++g_status.xl2515_tx_aborted;
}

/** @brief 递增 listen_only_enter。 */
void rtos_status_inc_listen_only_enter(void)
{
    ++g_status.listen_only_enter;
}

/** @brief 递增 linux_rehandshake_ok。 */
void rtos_status_inc_linux_rehandshake_ok(void)
{
    ++g_status.linux_rehandshake_ok;
}
