/* FreeRTOS comm 协议适配占位：payload 格式 TBD，不定义大小核 ABI。 */
#ifndef RTOS_PROTOCOL_ADAPTER_H
#define RTOS_PROTOCOL_ADAPTER_H

#include <stdint.h>

#include "error_code.h"
#include "rtos_can_message.h"
#include "rtos_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t length;
    uint8_t bytes[RTOS_IPC_PAYLOAD_MAX_LEN];
} rtos_ipc_payload_t;

typedef struct {
    const uint8_t *bytes;
    uint16_t length;
} rtos_ipc_payload_view_t;

typedef unified_error_t (*rtos_protocol_adapter_linux_payload_to_can_fn_t)(
    const rtos_ipc_payload_view_t *payload,
    rtos_can_message_t *out_message);

void rtos_protocol_adapter_init(void);
void rtos_protocol_adapter_set_linux_payload_to_can(
    rtos_protocol_adapter_linux_payload_to_can_fn_t handler);
unified_error_t rtos_protocol_adapter_linux_payload_to_can(
    const rtos_ipc_payload_t *payload,
    rtos_can_message_t *out_message);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_PROTOCOL_ADAPTER_H */
