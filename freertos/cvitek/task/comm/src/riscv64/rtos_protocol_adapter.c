/* FreeRTOS comm 协议适配占位：后续统一协议确定后在此接入解析。 */
#include "rtos_protocol_adapter.h"

static rtos_protocol_adapter_linux_payload_to_can_fn_t g_linux_payload_to_can;

void rtos_protocol_adapter_init(void)
{
    g_linux_payload_to_can = 0;
}

void rtos_protocol_adapter_set_linux_payload_to_can(
    rtos_protocol_adapter_linux_payload_to_can_fn_t handler)
{
    g_linux_payload_to_can = handler;
}

unified_error_t rtos_protocol_adapter_linux_payload_to_can(
    const rtos_ipc_payload_t *payload,
    rtos_can_message_t *out_message)
{
    rtos_ipc_payload_view_t view;

    if ((payload == 0) || (out_message == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (payload->length > RTOS_IPC_PAYLOAD_MAX_LEN) {
        return UNIFIED_ERR_LENGTH;
    }

    if (g_linux_payload_to_can == 0) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    view.bytes = payload->bytes;
    view.length = payload->length;
    return g_linux_payload_to_can(&view, out_message);
}
