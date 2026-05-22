/**
 * @file rtos_ipc.h
 * @brief FreeRTOS comm IPC 占位和回传 hook。
 *
 * 本文件只提供小核内部 IPC 接入点，不定义共享内存 ring、cmdqu/mailbox
 * 命令或 payload ABI。真实共享内存模块接入后可替换这些 hook。
 */
#ifndef RTOS_IPC_H
#define RTOS_IPC_H

#include <stdint.h>

#include "error_code.h"
#include "rtos_can_message.h"
#include "rtos_protocol_adapter.h"
#include "rtos_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t event_type;    // 事件类型，取值见 rtos_ipc_event_type_t。

    uint32_t detail;        // 事件附加信息，由事件类型解释。
} rtos_ipc_event_t;         // RTOS->Linux 错误/状态事件占位结构。

typedef enum {
    RTOS_IPC_EVENT_LINUX_HEARTBEAT_TIMEOUT = 1u,  // Linux heartbeat 超时。
    RTOS_IPC_EVENT_CAN_BUS_OFF = 2u,              // CAN bus-off。
    RTOS_IPC_EVENT_SPI_ERROR = 3u,                // SPI/driver 错误。
    RTOS_IPC_EVENT_RX_OVERFLOW = 4u,              // XL2515 RX overflow。
} rtos_ipc_event_type_t;                          // RTOS->Linux 事件类型。

typedef unified_error_t (*rtos_ipc_can_rx_sender_fn_t)(const rtos_can_message_t *message);      // CAN RX 回传 hook 类型。

typedef unified_error_t (*rtos_ipc_payload_sender_fn_t)(const rtos_ipc_payload_t *payload);     // RTOS->Linux payload 回传 hook 类型。

typedef unified_error_t (*rtos_ipc_status_sender_fn_t)(const rtos_status_snapshot_t *status);   // 状态快照回传 hook 类型。

typedef unified_error_t (*rtos_ipc_event_sender_fn_t)(const rtos_ipc_event_t *event);           // 错误事件回传 hook 类型。

unified_error_t rtos_ipc_init(void);

unified_error_t rtos_ipc_mock_receive_can_message(const rtos_can_message_t *message);

unified_error_t rtos_ipc_mock_receive_payload(const uint8_t *bytes, uint16_t length);

unified_error_t rtos_ipc_poll_linux_payload(rtos_ipc_payload_t *out_payload);

void rtos_ipc_set_can_rx_sender(rtos_ipc_can_rx_sender_fn_t sender);

void rtos_ipc_set_payload_sender(rtos_ipc_payload_sender_fn_t sender);

void rtos_ipc_set_status_sender(rtos_ipc_status_sender_fn_t sender);

void rtos_ipc_set_event_sender(rtos_ipc_event_sender_fn_t sender);

unified_error_t rtos_ipc_send_can_rx(const rtos_can_message_t *message);

unified_error_t rtos_ipc_send_status(const rtos_status_snapshot_t *status);

unified_error_t rtos_ipc_send_error_event(const rtos_ipc_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_IPC_H */
