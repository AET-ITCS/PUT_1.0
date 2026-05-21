/* FreeRTOS comm IPC 占位：真实共享内存/cmdqu 接入前只保留 CAN 消息 mock 注入入口。 */
#ifndef RTOS_IPC_H
#define RTOS_IPC_H

#include "error_code.h"
#include "rtos_can_message.h"

#ifdef __cplusplus
extern "C" {
#endif

unified_error_t rtos_ipc_init(void);
unified_error_t rtos_ipc_mock_receive_can_message(const rtos_can_message_t *message);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_IPC_H */
