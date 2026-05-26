/**
 * @file rtos_shm_platform.c
 * @brief rtos_firmware 共享内存 IPC 默认平台抽象实现。
 * @author Yukikaze
 */
#include "rtos_shm_platform.h"

/**
 * @brief 默认 no-op cache 操作。
 *
 * @param address 待处理地址。
 * @param length 待处理长度。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t default_cache_op(const void *address, size_t length, void *user_context)
{
    (void)address;
    (void)length;
    (void)user_context;

    /* host 测试阶段不需要真实 cache 操作。 */
    return UNIFIED_OK;
}

/**
 * @brief 默认内存屏障操作。
 *
 * @param user_context 平台私有上下文。
 */
static void default_memory_barrier(void *user_context)
{
    (void)user_context;

#if defined(__GNUC__) || defined(__clang__)
    /* 编译器屏障用于约束 host 构建下的访问重排。 */
    __asm__ __volatile__("" ::: "memory");
#endif
}

/**
 * @brief 默认 no-op 通知操作。
 *
 * @param direction 需要通知的 ring 方向。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t default_notify(put_shm_direction_t direction, void *user_context)
{
    (void)direction;
    (void)user_context;

    /* host 测试阶段不触发真实 doorbell/mailbox。 */
    return UNIFIED_OK;
}

/**
 * @brief 默认 32 位原子 OR 操作。
 *
 * @param address 待原子更新的共享字段地址。
 * @param mask OR 操作使用的 bit mask。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t default_atomic_or_u32(volatile uint32_t *address,
                                             uint32_t mask,
                                             void *user_context)
{
    (void)user_context;

    if (address == 0) {
        /* 地址为空时不能执行原子更新。 */
        return UNIFIED_ERR_NULL;
    }

#if defined(__GNUC__) || defined(__clang__)
    /* host 默认实现使用编译器原子内建，真实 BSP 可替换为硬件平台 API。 */
    (void)__atomic_fetch_or(address, mask, __ATOMIC_SEQ_CST);
#else
    /* 非 GCC/Clang host 构建只提供单线程 fallback。 */
    *address = *address | mask;
#endif

    return UNIFIED_OK;
}

/**
 * @brief 默认 32 位原子 AND 操作。
 *
 * @param address 待原子更新的共享字段地址。
 * @param mask AND 操作使用的 bit mask。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t default_atomic_and_u32(volatile uint32_t *address,
                                              uint32_t mask,
                                              void *user_context)
{
    (void)user_context;

    if (address == 0) {
        /* 地址为空时不能执行原子更新。 */
        return UNIFIED_ERR_NULL;
    }

#if defined(__GNUC__) || defined(__clang__)
    /* host 默认实现使用编译器原子内建，真实 BSP 可替换为硬件平台 API。 */
    (void)__atomic_fetch_and(address, mask, __ATOMIC_SEQ_CST);
#else
    /* 非 GCC/Clang host 构建只提供单线程 fallback。 */
    *address = *address & mask;
#endif

    return UNIFIED_OK;
}

/**
 * @brief 默认 32 位原子 ADD 操作。
 *
 * @param address 待原子累加的共享字段地址。
 * @param value 需要累加的数值。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t default_atomic_add_u32(volatile uint32_t *address,
                                              uint32_t value,
                                              void *user_context)
{
    (void)user_context;

    if (address == 0) {
        /* 地址为空时不能执行原子累加。 */
        return UNIFIED_ERR_NULL;
    }

#if defined(__GNUC__) || defined(__clang__)
    /* host 默认实现使用编译器原子内建，真实 BSP 可替换为硬件平台 API。 */
    (void)__atomic_fetch_add(address, value, __ATOMIC_SEQ_CST);
#else
    /* 非 GCC/Clang host 构建只提供单线程 fallback。 */
    *address = *address + value;
#endif

    return UNIFIED_OK;
}

/** @brief 默认平台操作集合。 */
static const rtos_shm_platform_ops_t g_default_ops = {
    default_cache_op,
    default_cache_op,
    default_memory_barrier,
    default_notify,
    default_atomic_or_u32,
    default_atomic_and_u32,
    default_atomic_add_u32,
    0,
};

/**
 * @brief 获取默认平台操作集合。
 *
 * @return 默认平台操作集合指针。
 */
const rtos_shm_platform_ops_t *rtos_shm_platform_default_ops(void)
{
    return &g_default_ops;
}
