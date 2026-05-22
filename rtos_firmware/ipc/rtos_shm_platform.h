/**
 * @file rtos_shm_platform.h
 * @brief rtos_firmware 共享内存 IPC 平台抽象接口。
 * @author Yukikaze
 */
#ifndef RTOS_SHM_PLATFORM_H
#define RTOS_SHM_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "error_code.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief cache flush 平台操作函数类型。
 *
 * @param address 待 flush 的起始地址。
 * @param length 待 flush 的字节数。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
typedef unified_error_t (*rtos_shm_cache_op_t)(const void *address,
                                               size_t length,
                                               void *user_context);

/**
 * @brief memory barrier 平台操作函数类型。
 *
 * @param user_context 平台私有上下文。
 */
typedef void (*rtos_shm_barrier_op_t)(void *user_context);

/**
 * @brief doorbell/mailbox 通知平台操作函数类型。
 *
 * @param direction 需要通知的 ring 方向。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示通知成功，否则返回公共错误码。
 */
typedef unified_error_t (*rtos_shm_notify_op_t)(put_shm_direction_t direction,
                                                void *user_context);

/**
 * @brief 共享内存 IPC 平台操作集合。
 */
typedef struct {
    rtos_shm_cache_op_t cache_flush;      /**< cache flush 操作，host 默认 no-op。 */
    rtos_shm_cache_op_t cache_invalidate; /**< cache invalidate 操作，host 默认 no-op。 */
    rtos_shm_barrier_op_t memory_barrier; /**< 内存屏障操作，host 默认编译器屏障。 */
    rtos_shm_notify_op_t notify;          /**< doorbell/mailbox 通知操作，host 默认 no-op。 */
    void *user_context;                   /**< 平台私有上下文指针。 */
} rtos_shm_platform_ops_t;

/**
 * @brief 获取默认平台操作集合。
 *
 * 默认实现为 no-op cache、GCC/Clang 编译器屏障和 no-op notify，
 * 仅用于 host 测试和骨架阶段。
 *
 * @return 默认平台操作集合指针。
 */
const rtos_shm_platform_ops_t *rtos_shm_platform_default_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_SHM_PLATFORM_H */
