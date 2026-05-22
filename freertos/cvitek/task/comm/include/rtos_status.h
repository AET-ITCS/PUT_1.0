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

typedef struct {
    uint32_t rx_from_linux;              // 从 Linux 通道读取并成功交给 CAN 层的消息数。
    uint32_t tx_to_can_ok;               // 成功发送到 CAN 的消息数。
    uint32_t tx_to_can_fail;             // CAN 发送失败次数。
    uint32_t drop_null;                  // 空 CAN message 指针丢弃次数。
    uint32_t drop_flag;                  // 不支持 CAN flag 丢弃次数。
    uint32_t drop_can_id;                // CAN ID 越界丢弃次数。
    uint32_t drop_dlc;                   // CAN DLC 越界丢弃次数。
    uint32_t drop_queue_full;            // CAN TX 队列满丢弃次数。
    bool can_ready;                      // CAN driver 是否 ready。
    bool linux_online;                   // Linux heartbeat/rehandshake 状态。
    uint32_t rx_from_can;                // 从 CAN 总线收到的消息数。
    uint32_t tx_to_linux;                // 回传 Linux 成功次数。
    uint32_t drop_ring_full;             // 回传通道满或 hook 失败丢弃次数。
    uint32_t ipc_payload_drop;           // IPC payload 入队或适配失败丢弃次数。
    uint32_t rx_overrun;                 // 小核 RX 处理过载次数。
    uint32_t xl2515_rx_overflow;         // XL2515 RX0/RX1 overflow 次数。
    uint32_t spi_error;                  // SPI/driver 错误次数。
    uint32_t can_bus_off;                // CAN bus-off 次数。
    uint32_t can_error_passive;          // CAN error passive 次数。
    uint32_t linux_heartbeat_timeout;    // Linux heartbeat 超时次数。
    uint32_t linux_offline_enter;        // 进入 fail-safe offline 次数。
    uint32_t tx_queue_purged;            // fail-safe 清空 CAN TX 队列的消息总数。
    uint32_t xl2515_tx_aborted;          // fail-safe abort XL2515 TX 成功次数。
    uint32_t listen_only_enter;          // 切入 Listen-Only 次数。
    uint32_t linux_rehandshake_ok;       // Linux 重新握手成功次数。
} rtos_status_snapshot_t;                // 小核 comm 状态快照。

void rtos_status_init(void);

void rtos_status_reset(void);

void rtos_status_get_snapshot(rtos_status_snapshot_t *out_snapshot);

void rtos_status_set_can_ready(bool ready);

void rtos_status_set_linux_online(bool online);

void rtos_status_inc_rx_from_linux(void);

void rtos_status_inc_tx_to_can_ok(void);

void rtos_status_inc_tx_to_can_fail(void);

void rtos_status_inc_drop_queue_full(void);

void rtos_status_inc_drop_null(void);

void rtos_status_inc_drop_flag(void);

void rtos_status_inc_drop_can_id(void);

void rtos_status_inc_drop_dlc(void);

void rtos_status_inc_rx_from_can(void);

void rtos_status_inc_tx_to_linux(void);

void rtos_status_inc_drop_ring_full(void);

void rtos_status_inc_ipc_payload_drop(void);

void rtos_status_inc_rx_overrun(void);

void rtos_status_inc_xl2515_rx_overflow(void);

void rtos_status_inc_spi_error(void);

void rtos_status_inc_can_bus_off(void);

void rtos_status_inc_can_error_passive(void);

void rtos_status_inc_linux_heartbeat_timeout(void);

void rtos_status_inc_linux_offline_enter(void);

void rtos_status_add_tx_queue_purged(uint32_t count);

void rtos_status_inc_xl2515_tx_aborted(void);

void rtos_status_inc_listen_only_enter(void);

void rtos_status_inc_linux_rehandshake_ok(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_STATUS_H */
