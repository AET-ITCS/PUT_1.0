/* FreeRTOS comm IPC mock：真实共享内存/cmdqu 后续从这里接入。 */
#include "rtos_ipc.h"

#include "rtos_can_forward.h"

unified_error_t rtos_ipc_init(void)
{
    return UNIFIED_OK;
}

unified_error_t rtos_ipc_mock_receive_can_message(const rtos_can_message_t *message)
{
    return rtos_can_forward_submit_message(message);
}
