/* 共享内存 IPC 接口占位：只声明共享内存模块需要提供的最小发送能力。 */
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
 * - 本文件只是接口预留，不实现共享内存模块；
 * - 共享内存具体布局、地址、cache 处理、cmdqu/mailbox 通知由共享内存负责人实现；
 * - 本文件不定义共享内存 ABI，不固定 ring queue / mailbox / DMA 等内部结构；
 * - 大核协议转换层只依赖“可以发送一帧 const unified_frame_t”这个最小能力；
 * - 实现方不得修改传入的 frame，发送成功返回 UNIFIED_OK，失败返回公共错误码。
 */
typedef unified_error_t (*shared_memory_ipc_send_fn_t)(const unified_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_MEMORY_IPC_H */
