/**
 * @file linux_shm_platform.c
 * @brief Linux 侧共享内存 IPC v2 平台抽象默认实现。
 * @author Yukikaze
 */
#include "linux_shm_platform.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/**
 * @brief /dev/mem 映射上下文。
 */
typedef struct {
    int fd; /**< /dev/mem 文件描述符。 */
} linux_shm_devmem_mapping_t;

/**
 * @brief host 后端映射共享内存。
 *
 * @param physical_base host 后端忽略的物理地址。
 * @param region_size 映射长度。
 * @param out_address 输出虚拟地址。
 * @param out_mapping_context 输出映射上下文。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t host_map_region(uintptr_t physical_base,
                                       size_t region_size,
                                       void **out_address,
                                       void **out_mapping_context,
                                       void *user_context)
{
    void *address; /**< 分配得到的 host 内存地址。 */

    (void)physical_base;
    (void)user_context;

    if ((out_address == 0) || (out_mapping_context == 0) || (region_size == 0u)) {
        /* 输出指针或长度非法时不能映射。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    address = 0;
    if (posix_memalign(&address, PUT_SHM_CACHE_LINE_SIZE, region_size) != 0) {
        /* host 内存不足时按 IPC 未就绪处理。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    memset(address, 0, region_size);
    *out_address = address;
    *out_mapping_context = address;
    return UNIFIED_OK;
}

/**
 * @brief host 后端解除映射。
 *
 * @param address host 映射地址。
 * @param region_size 映射长度。
 * @param mapping_context 映射上下文。
 * @param user_context 平台私有上下文。
 */
static void host_unmap_region(void *address,
                              size_t region_size,
                              void *mapping_context,
                              void *user_context)
{
    (void)region_size;
    (void)mapping_context;
    (void)user_context;

    /* host 后端由 posix_memalign 分配，解除映射时直接 free。 */
    free(address);
}

/**
 * @brief /dev/mem 后端映射共享内存。
 *
 * @param physical_base reserved-memory 物理基地址。
 * @param region_size 映射长度。
 * @param out_address 输出虚拟地址。
 * @param out_mapping_context 输出映射上下文。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t devmem_map_region(uintptr_t physical_base,
                                         size_t region_size,
                                         void **out_address,
                                         void **out_mapping_context,
                                         void *user_context)
{
    linux_shm_devmem_mapping_t *mapping; /**< /dev/mem 映射上下文。 */
    void *address;                       /**< mmap 后的虚拟地址。 */

    (void)user_context;

    if ((out_address == 0) || (out_mapping_context == 0) || (region_size == 0u)) {
        /* 输出指针或长度非法时不能映射。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    mapping = (linux_shm_devmem_mapping_t *)calloc(1u, sizeof(*mapping));
    if (mapping == 0) {
        /* 无法分配映射上下文。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    mapping->fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mapping->fd < 0) {
        /* /dev/mem 打开失败，通常表示权限或平台不支持。 */
        free(mapping);
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    address = mmap(0, region_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   mapping->fd, (off_t)physical_base);
    if (address == MAP_FAILED) {
        /* mmap 失败时必须关闭 fd 并释放上下文。 */
        (void)close(mapping->fd);
        free(mapping);
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    *out_address = address;
    *out_mapping_context = mapping;
    return UNIFIED_OK;
}

/**
 * @brief /dev/mem 后端解除映射。
 *
 * @param address 映射地址。
 * @param region_size 映射长度。
 * @param mapping_context 映射上下文。
 * @param user_context 平台私有上下文。
 */
static void devmem_unmap_region(void *address,
                                size_t region_size,
                                void *mapping_context,
                                void *user_context)
{
    linux_shm_devmem_mapping_t *mapping; /**< /dev/mem 映射上下文。 */

    (void)user_context;

    mapping = (linux_shm_devmem_mapping_t *)mapping_context;
    if ((address != 0) && (region_size != 0u)) {
        /* 解除 mmap 映射，失败仅作为清理期错误忽略。 */
        (void)munmap(address, region_size);
    }

    if (mapping != 0) {
        /* 关闭 /dev/mem 文件描述符并释放上下文。 */
        (void)close(mapping->fd);
        free(mapping);
    }
}

/**
 * @brief no-op cache 同步。
 *
 * @param address 同步地址。
 * @param length 同步长度。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t noop_cache_op(const void *address, size_t length, void *user_context)
{
    (void)address;
    (void)length;
    (void)user_context;

    /* 第一版由后续内核驱动或 ioctl 替换真实 cache maintenance。 */
    return UNIFIED_OK;
}

/**
 * @brief 默认内存屏障。
 *
 * @param user_context 平台私有上下文。
 */
static void default_memory_barrier(void *user_context)
{
    (void)user_context;

#if defined(__GNUC__) || defined(__clang__)
    /* 编译器级内存屏障，约束 host 构建下的访问重排。 */
    __asm__ __volatile__("" ::: "memory");
#endif
}

/**
 * @brief no-op doorbell 通知。
 *
 * @param direction 通知方向。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t noop_notify(put_shm_direction_t direction, void *user_context)
{
    (void)direction;
    (void)user_context;

    /* host 阶段不触发真实 mailbox/cmdqu。 */
    return UNIFIED_OK;
}

/**
 * @brief 默认原子 OR。
 *
 * @param address 待更新地址。
 * @param mask OR mask。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t default_atomic_or_u32(volatile uint32_t *address,
                                             uint32_t mask,
                                             void *user_context)
{
    (void)user_context;

    if (address == 0) {
        /* 地址为空时不能执行原子操作。 */
        return UNIFIED_ERR_NULL;
    }

#if defined(__GNUC__) || defined(__clang__)
    (void)__atomic_fetch_or(address, mask, __ATOMIC_SEQ_CST);
#else
    *address = *address | mask;
#endif

    return UNIFIED_OK;
}

/**
 * @brief 默认原子 AND。
 *
 * @param address 待更新地址。
 * @param mask AND mask。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t default_atomic_and_u32(volatile uint32_t *address,
                                              uint32_t mask,
                                              void *user_context)
{
    (void)user_context;

    if (address == 0) {
        /* 地址为空时不能执行原子操作。 */
        return UNIFIED_ERR_NULL;
    }

#if defined(__GNUC__) || defined(__clang__)
    (void)__atomic_fetch_and(address, mask, __ATOMIC_SEQ_CST);
#else
    *address = *address & mask;
#endif

    return UNIFIED_OK;
}

/**
 * @brief 默认原子 ADD。
 *
 * @param address 待更新地址。
 * @param value 累加值。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t default_atomic_add_u32(volatile uint32_t *address,
                                              uint32_t value,
                                              void *user_context)
{
    (void)user_context;

    if (address == 0) {
        /* 地址为空时不能执行原子操作。 */
        return UNIFIED_ERR_NULL;
    }

#if defined(__GNUC__) || defined(__clang__)
    (void)__atomic_fetch_add(address, value, __ATOMIC_SEQ_CST);
#else
    *address = *address + value;
#endif

    return UNIFIED_OK;
}

/** @brief host/mock 默认平台操作集合。 */
static const linux_shm_platform_ops_t g_host_ops = {
    host_map_region,
    host_unmap_region,
    noop_cache_op,
    noop_cache_op,
    default_memory_barrier,
    noop_notify,
    default_atomic_or_u32,
    default_atomic_and_u32,
    default_atomic_add_u32,
    0,
};

/** @brief /dev/mem 平台操作集合。 */
static const linux_shm_platform_ops_t g_devmem_ops = {
    devmem_map_region,
    devmem_unmap_region,
    noop_cache_op,
    noop_cache_op,
    default_memory_barrier,
    noop_notify,
    default_atomic_or_u32,
    default_atomic_and_u32,
    default_atomic_add_u32,
    0,
};

/**
 * @brief 获取 host/mock 默认平台操作集合。
 *
 * @return 默认平台操作集合。
 */
const linux_shm_platform_ops_t *linux_shm_platform_default_ops(void)
{
    return &g_host_ops;
}

/**
 * @brief 获取 /dev/mem 平台操作集合。
 *
 * @return /dev/mem 平台操作集合。
 */
const linux_shm_platform_ops_t *linux_shm_platform_devmem_ops(void)
{
    return &g_devmem_ops;
}
