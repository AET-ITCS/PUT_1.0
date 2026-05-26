/**
 * @file rtos_shm_ipc.h
 * @brief rtos_firmware 共享内存 IPC v2 descriptor 搬运接口。
 * @author Yukikaze
 */
#ifndef RTOS_SHM_IPC_H
#define RTOS_SHM_IPC_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_shm_platform.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief rtos_firmware 共享内存 IPC 上下文。
 */
typedef struct {
    put_shm_region_t *region;             /**< 绑定的共享内存 v2 region。 */
    rtos_shm_platform_ops_t platform_ops; /**< 当前使用的平台操作集合。 */
    bool initialized;                     /**< 是否已成功 attach。 */
} rtos_shm_ipc_t;

/**
 * @brief 初始化共享内存 v2 region。
 *
 * @param region 共享内存区域指针。
 * @param linux_epoch Linux 启动纪元。
 * @param rtos_epoch RTOS 启动纪元。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_format_region(put_shm_region_t *region,
                                           uint32_t linux_epoch,
                                           uint32_t rtos_epoch);

/**
 * @brief 绑定并校验共享内存 IPC v2 region。
 *
 * @param ipc IPC 上下文。
 * @param region 共享内存区域指针。
 * @param ops 平台操作集合；NULL 时使用默认 no-op 平台操作。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_attach(rtos_shm_ipc_t *ipc,
                                    put_shm_region_t *region,
                                    const rtos_shm_platform_ops_t *ops);

/**
 * @brief 从指定物理接口 RX ring 读取一个 descriptor。
 *
 * @param ipc IPC 上下文。
 * @param interface_id 物理接口 ID，取值见 put_shm_interface_t。
 * @param out_descriptor 输出 descriptor。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_dequeue_rx_descriptor(rtos_shm_ipc_t *ipc,
                                                   put_shm_interface_t interface_id,
                                                   put_shm_descriptor_t *out_descriptor);

/**
 * @brief 向指定物理接口 TX ring 写入一个 descriptor。
 *
 * @param ipc IPC 上下文。
 * @param interface_id 目标物理接口 ID，取值见 put_shm_interface_t。
 * @param descriptor 待写入 descriptor。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_enqueue_tx_descriptor(rtos_shm_ipc_t *ipc,
                                                   put_shm_interface_t interface_id,
                                                   const put_shm_descriptor_t *descriptor);

/**
 * @brief 写入 Frame Pool 回收 descriptor。
 *
 * @param ipc IPC 上下文。
 * @param frame_id 需要 Linux 回收的 Frame Pool block ID。
 * @param reason 回收原因。
 * @param source_interface 原始来源物理接口。
 * @param target_interface 原始目标物理接口。
 * @param epoch Linux 启动纪元。
 * @param flags 附加标志。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_reclaim_frame(rtos_shm_ipc_t *ipc,
                                           uint32_t frame_id,
                                           put_shm_reclaim_reason_t reason,
                                           put_shm_interface_t source_interface,
                                           put_shm_interface_t target_interface,
                                           uint32_t epoch,
                                           uint32_t flags);

/**
 * @brief 根据 descriptor 获取 Frame Pool 中的只读完整 anyMSG。
 *
 * @param ipc IPC 上下文。
 * @param descriptor descriptor 指针。
 * @param out_frame 输出完整 anyMSG 起始地址。
 * @param out_frame_length 输出完整 anyMSG 字节数。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_get_frame_const(const rtos_shm_ipc_t *ipc,
                                             const put_shm_descriptor_t *descriptor,
                                             const uint8_t **out_frame,
                                             uint16_t *out_frame_length);

/**
 * @brief 向 descriptor ring 写入一个 descriptor。
 *
 * @param ring descriptor ring 指针。
 * @param pending_line pending bitmap 控制行。
 * @param descriptor 待写入 descriptor。
 * @param notify_direction doorbell 通知方向。
 * @param ops 平台操作集合；NULL 时使用默认 no-op 平台操作。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_descriptor_ring_enqueue(put_shm_descriptor_ring_t *ring,
                                                 put_shm_pending_line_t *pending_line,
                                                 const put_shm_descriptor_t *descriptor,
                                                 put_shm_direction_t notify_direction,
                                                 const rtos_shm_platform_ops_t *ops);

/**
 * @brief 从 descriptor ring 读取一个 descriptor。
 *
 * @param ring descriptor ring 指针。
 * @param pending_line pending bitmap 控制行。
 * @param out_descriptor 输出 descriptor。
 * @param ops 平台操作集合；NULL 时使用默认 no-op 平台操作。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_descriptor_ring_dequeue(put_shm_descriptor_ring_t *ring,
                                                 put_shm_pending_line_t *pending_line,
                                                 put_shm_descriptor_t *out_descriptor,
                                                 const rtos_shm_platform_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_SHM_IPC_H */
