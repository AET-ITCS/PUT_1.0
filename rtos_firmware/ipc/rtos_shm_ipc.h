/**
 * @file rtos_shm_ipc.h
 * @brief rtos_firmware 共享内存 IPC ring 搬运接口。
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
 * @brief 从共享内存 slot 取出的 opaque 消息。
 */
typedef struct {
    uint16_t message_type;                     /**< 消息类型，取值见 put_shm_message_type_t。 */
    uint32_t sequence;                         /**< slot 序号。 */
    uint32_t epoch;                            /**< 发送方启动纪元。 */
    uint16_t payload_length;                   /**< payload 实际长度。 */
    uint8_t payload[PUT_SHM_PAYLOAD_MAX_LEN];  /**< opaque payload，IPC 层不解释内容。 */
} rtos_shm_message_t;

/**
 * @brief rtos_firmware 共享内存 IPC 上下文。
 */
typedef struct {
    put_shm_region_t *region;             /**< 绑定的共享内存区域。 */
    rtos_shm_platform_ops_t platform_ops; /**< 当前使用的平台操作集合。 */
    bool initialized;                     /**< 是否已成功 attach。 */
} rtos_shm_ipc_t;

/**
 * @brief 初始化共享内存 region 和两个 ring。
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
 * @brief 绑定并校验共享内存 IPC region。
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
 * @brief 从 Linux -> RTOS ring 接收一个 opaque payload。
 *
 * @param ipc IPC 上下文。
 * @param out_message 输出消息。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_receive_from_linux(rtos_shm_ipc_t *ipc,
                                                rtos_shm_message_t *out_message);

/**
 * @brief 向 RTOS -> Linux ring 发送一个 opaque payload。
 *
 * @param ipc IPC 上下文。
 * @param message_type 消息类型。
 * @param payload payload 字节指针；payload_length 为 0 时可为 NULL。
 * @param payload_length payload 长度。
 * @param epoch 发送方启动纪元。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_send_to_linux(rtos_shm_ipc_t *ipc,
                                           uint16_t message_type,
                                           const uint8_t *payload,
                                           uint16_t payload_length,
                                           uint32_t epoch);

/**
 * @brief 向指定 SPSC ring 写入一个 opaque payload。
 *
 * @param ring ring 指针。
 * @param message_type 消息类型。
 * @param payload payload 字节指针；payload_length 为 0 时可为 NULL。
 * @param payload_length payload 长度。
 * @param epoch 发送方启动纪元。
 * @param ops 平台操作集合；NULL 时使用默认 no-op 平台操作。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ring_enqueue(put_shm_ring_t *ring,
                                      uint16_t message_type,
                                      const uint8_t *payload,
                                      uint16_t payload_length,
                                      uint32_t epoch,
                                      const rtos_shm_platform_ops_t *ops);

/**
 * @brief 从指定 SPSC ring 读取一个 opaque payload。
 *
 * @param ring ring 指针。
 * @param out_message 输出消息。
 * @param ops 平台操作集合；NULL 时使用默认 no-op 平台操作。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ring_dequeue(put_shm_ring_t *ring,
                                      rtos_shm_message_t *out_message,
                                      const rtos_shm_platform_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_SHM_IPC_H */
