/* FreeRTOS comm IPC 占位：真实共享内存/cmdqu 接入前只保留 mock 注入入口。 */
#ifndef RTOS_IPC_H
#define RTOS_IPC_H

#include "error_code.h"
#include "unified_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

unified_error_t rtos_ipc_init(void);
unified_error_t rtos_ipc_mock_receive_frame(const unified_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_IPC_H */
