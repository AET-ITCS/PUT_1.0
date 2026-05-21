/**
 * @file rtos_status.h
 * @brief FreeRTOS comm 状态统计接口。
 *
 * 本模块维护小核 CAN、IPC、错误恢复和 fail-safe 相关计数。状态快照由
 * RTOS->Linux 回传 hook 发送给 Linux，再由 Linux 整理给 Web 使用。
 */
#ifndef RTOS_STATUS_H
#define RTOS_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 小核 comm 状态快照。 */
typedef struct {
    /** @brief 从 Linux 通道读取并成功交给 CAN 层的消息数。 */
    uint32_t rx_from_linux;
    /** @brief 成功发送到 CAN 的消息数。 */
    uint32_t tx_to_can_ok;
    /** @brief CAN 发送失败次数。 */
    uint32_t tx_to_can_fail;
    /** @brief 空 CAN message 指针丢弃次数。 */
    uint32_t drop_null;
    /** @brief 不支持 CAN flag 丢弃次数。 */
    uint32_t drop_flag;
    /** @brief CAN ID 越界丢弃次数。 */
    uint32_t drop_can_id;
    /** @brief CAN DLC 越界丢弃次数。 */
    uint32_t drop_dlc;
    /** @brief CAN TX 队列满丢弃次数。 */
    uint32_t drop_queue_full;
    /** @brief CAN driver 是否 ready。 */
    bool can_ready;
    /** @brief Linux heartbeat/rehandshake 状态。 */
    bool linux_online;
    /** @brief 从 CAN 总线收到的消息数。 */
    uint32_t rx_from_can;
    /** @brief 回传 Linux 成功次数。 */
    uint32_t tx_to_linux;
    /** @brief 回传通道满或 hook 失败丢弃次数。 */
    uint32_t drop_ring_full;
    /** @brief IPC payload 入队或适配失败丢弃次数。 */
    uint32_t ipc_payload_drop;
    /** @brief 小核 RX 处理过载次数。 */
    uint32_t rx_overrun;
    /** @brief XL2515 RX0/RX1 overflow 次数。 */
    uint32_t xl2515_rx_overflow;
    /** @brief SPI/driver 错误次数。 */
    uint32_t spi_error;
    /** @brief CAN bus-off 次数。 */
    uint32_t can_bus_off;
    /** @brief CAN error passive 次数。 */
    uint32_t can_error_passive;
    /** @brief Linux heartbeat 超时次数。 */
    uint32_t linux_heartbeat_timeout;
    /** @brief 进入 fail-safe offline 次数。 */
    uint32_t linux_offline_enter;
    /** @brief fail-safe 清空 CAN TX 队列的消息总数。 */
    uint32_t tx_queue_purged;
    /** @brief fail-safe abort XL2515 TX 成功次数。 */
    uint32_t xl2515_tx_aborted;
    /** @brief 切入 Listen-Only 次数。 */
    uint32_t listen_only_enter;
    /** @brief Linux 重新握手成功次数。 */
    uint32_t linux_rehandshake_ok;
} rtos_status_snapshot_t;

/** @brief 初始化状态统计模块。 */
void rtos_status_init(void);

/** @brief 清零状态统计并设置默认在线状态。 */
void rtos_status_reset(void);

/**
 * @brief 获取状态快照。
 * @param[out] out_snapshot 输出快照；NULL 时忽略。
 */
void rtos_status_get_snapshot(rtos_status_snapshot_t *out_snapshot);

/** @brief 设置 CAN ready 状态。 */
void rtos_status_set_can_ready(bool ready);

/** @brief 设置 Linux online 状态。 */
void rtos_status_set_linux_online(bool online);

/** @brief 递增 rx_from_linux。 */
void rtos_status_inc_rx_from_linux(void);

/** @brief 递增 tx_to_can_ok。 */
void rtos_status_inc_tx_to_can_ok(void);

/** @brief 递增 tx_to_can_fail。 */
void rtos_status_inc_tx_to_can_fail(void);

/** @brief 递增 drop_queue_full。 */
void rtos_status_inc_drop_queue_full(void);

/** @brief 递增 drop_null。 */
void rtos_status_inc_drop_null(void);

/** @brief 递增 drop_flag。 */
void rtos_status_inc_drop_flag(void);

/** @brief 递增 drop_can_id。 */
void rtos_status_inc_drop_can_id(void);

/** @brief 递增 drop_dlc。 */
void rtos_status_inc_drop_dlc(void);

/** @brief 递增 rx_from_can。 */
void rtos_status_inc_rx_from_can(void);

/** @brief 递增 tx_to_linux。 */
void rtos_status_inc_tx_to_linux(void);

/** @brief 递增 drop_ring_full。 */
void rtos_status_inc_drop_ring_full(void);

/** @brief 递增 ipc_payload_drop。 */
void rtos_status_inc_ipc_payload_drop(void);

/** @brief 递增 rx_overrun。 */
void rtos_status_inc_rx_overrun(void);

/** @brief 递增 xl2515_rx_overflow。 */
void rtos_status_inc_xl2515_rx_overflow(void);

/** @brief 递增 spi_error。 */
void rtos_status_inc_spi_error(void);

/** @brief 递增 can_bus_off。 */
void rtos_status_inc_can_bus_off(void);

/** @brief 递增 can_error_passive。 */
void rtos_status_inc_can_error_passive(void);

/** @brief 递增 linux_heartbeat_timeout。 */
void rtos_status_inc_linux_heartbeat_timeout(void);

/** @brief 递增 linux_offline_enter。 */
void rtos_status_inc_linux_offline_enter(void);

/** @brief 累加 tx_queue_purged。 */
void rtos_status_add_tx_queue_purged(uint32_t count);

/** @brief 递增 xl2515_tx_aborted。 */
void rtos_status_inc_xl2515_tx_aborted(void);

/** @brief 递增 listen_only_enter。 */
void rtos_status_inc_listen_only_enter(void);

/** @brief 递增 linux_rehandshake_ok。 */
void rtos_status_inc_linux_rehandshake_ok(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_STATUS_H */
