/* 共享内存 IPC 接口占位：声明大核与小核共享内存模块对接时需要提供的最小发送接口。 */
#ifndef SHARED_MEMORY_IPC_H
#define SHARED_MEMORY_IPC_H

#include "error_code.h"
#include "unified_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 共享内存 IPC 发送接口契约。
 *
 * 说明：
 * - 共享内存具体布局、地址、cache 处理、cmdqu/mailbox 通知由对应队友实现；
 * - 本文件不定义共享内存 ABI，不固定 ring queue 结构；
 * - 大核协议转换层只依赖“可以发送 unified_frame_t”这个最小能力。
 */
typedef unified_error_t (*shared_memory_ipc_send_fn_t)(const unified_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_MEMORY_IPC_H */
