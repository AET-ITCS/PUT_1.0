/* FreeRTOS comm IPC 占位：真实共享内存/cmdqu 接入前保留 mock 与回传钩子。 */
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
    uint32_t event_type;
    uint32_t detail;
} rtos_ipc_event_t;

typedef enum {
    RTOS_IPC_EVENT_LINUX_HEARTBEAT_TIMEOUT = 1u,
    RTOS_IPC_EVENT_CAN_BUS_OFF = 2u,
    RTOS_IPC_EVENT_SPI_ERROR = 3u,
    RTOS_IPC_EVENT_RX_OVERFLOW = 4u,
} rtos_ipc_event_type_t;

typedef unified_error_t (*rtos_ipc_can_rx_sender_fn_t)(const rtos_can_message_t *message);
typedef unified_error_t (*rtos_ipc_status_sender_fn_t)(const rtos_status_snapshot_t *status);
typedef unified_error_t (*rtos_ipc_event_sender_fn_t)(const rtos_ipc_event_t *event);

unified_error_t rtos_ipc_init(void);
unified_error_t rtos_ipc_mock_receive_can_message(const rtos_can_message_t *message);
unified_error_t rtos_ipc_mock_receive_payload(const uint8_t *bytes, uint16_t length);
unified_error_t rtos_ipc_poll_linux_payload(rtos_ipc_payload_t *out_payload);
void rtos_ipc_set_can_rx_sender(rtos_ipc_can_rx_sender_fn_t sender);
void rtos_ipc_set_status_sender(rtos_ipc_status_sender_fn_t sender);
void rtos_ipc_set_event_sender(rtos_ipc_event_sender_fn_t sender);
unified_error_t rtos_ipc_send_can_rx(const rtos_can_message_t *message);
unified_error_t rtos_ipc_send_status(const rtos_status_snapshot_t *status);
unified_error_t rtos_ipc_send_error_event(const rtos_ipc_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_IPC_H */
