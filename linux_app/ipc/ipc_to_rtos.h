/* 大核到小核发送接口：当前为 stub，后续替换为共享内存写入实现。 */
#ifndef IPC_TO_RTOS_H
#define IPC_TO_RTOS_H

#include "error_code.h"
#include "unified_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 发送统一帧给小核。
 *
 * 当前第一版是 stdout stub；队友共享内存模块完成后，只替换本函数内部实现。
 */
unified_error_t ipc_to_rtos_send(const unified_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* IPC_TO_RTOS_H */
