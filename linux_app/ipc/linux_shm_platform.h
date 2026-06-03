/**
 * @file linux_shm_platform.h
 * @brief Linux 侧共享内存 IPC v2 平台抽象接口。
 * @author Yukikaze
 */
#ifndef LINUX_SHM_PLATFORM_H
#define LINUX_SHM_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

#include "error_code.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Linux shared memory control 设备路径最大长度。 */
#define LINUX_SHM_CONTROL_DEVICE_PATH_MAX 128u

/** @brief Linux shared memory control ioctl magic。 */
#define LINUX_SHM_CONTROL_IOCTL_MAGIC 'P'

/**
 * @brief cache maintenance ioctl 的地址范围。
 */
typedef struct {
    uint64_t physical_address; /**< 待同步物理地址。 */
    uint64_t offset;           /**< 相对共享内存 region 起点的偏移。 */
    uint64_t length;           /**< 待同步字节数。 */
} linux_shm_control_range_t;

/**
 * @brief doorbell/mailbox ioctl 请求。
 */
typedef struct {
    uint32_t direction; /**< put_shm_direction_t 通知方向。 */
    uint32_t reserved;  /**< 保留字段，必须写 0。 */
} linux_shm_control_notify_t;

/** @brief flush cache range ioctl。 */
#define LINUX_SHM_CONTROL_IOCTL_CACHE_FLUSH \
    _IOW(LINUX_SHM_CONTROL_IOCTL_MAGIC, 1, linux_shm_control_range_t)

/** @brief invalidate cache range ioctl。 */
#define LINUX_SHM_CONTROL_IOCTL_CACHE_INVALIDATE \
    _IOW(LINUX_SHM_CONTROL_IOCTL_MAGIC, 2, linux_shm_control_range_t)

/** @brief Linux -> RTOS doorbell/mailbox ioctl。 */
#define LINUX_SHM_CONTROL_IOCTL_NOTIFY \
    _IOW(LINUX_SHM_CONTROL_IOCTL_MAGIC, 3, linux_shm_control_notify_t)

/**
 * @brief 共享内存区域映射函数类型。
 *
 * @param physical_base reserved-memory 物理基地址；host 后端可忽略。
 * @param region_size 需要映射的字节数。
 * @param out_address 输出映射后的虚拟地址。
 * @param out_mapping_context 输出映射私有上下文。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
typedef unified_error_t (*linux_shm_map_region_op_t)(uintptr_t physical_base,
                                                     size_t region_size,
                                                     void **out_address,
                                                     void **out_mapping_context,
                                                     void *user_context);

/**
 * @brief 共享内存区域解除映射函数类型。
 *
 * @param address 已映射的虚拟地址。
 * @param region_size 映射字节数。
 * @param mapping_context 映射私有上下文。
 * @param user_context 平台私有上下文。
 */
typedef void (*linux_shm_unmap_region_op_t)(void *address,
                                            size_t region_size,
                                            void *mapping_context,
                                            void *user_context);

/**
 * @brief cache 同步函数类型。
 *
 * @param address 待同步地址。
 * @param length 待同步字节数。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
typedef unified_error_t (*linux_shm_cache_op_t)(const void *address,
                                                size_t length,
                                                void *user_context);

/**
 * @brief memory barrier 函数类型。
 *
 * @param user_context 平台私有上下文。
 */
typedef void (*linux_shm_barrier_op_t)(void *user_context);

/**
 * @brief doorbell/mailbox 通知函数类型。
 *
 * @param direction 通知方向。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示通知成功，否则返回公共错误码。
 */
typedef unified_error_t (*linux_shm_notify_op_t)(put_shm_direction_t direction,
                                                 void *user_context);

/**
 * @brief 32 位原子 OR 函数类型。
 *
 * @param address 待原子更新地址。
 * @param mask OR mask。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
typedef unified_error_t (*linux_shm_atomic_or_u32_op_t)(volatile uint32_t *address,
                                                        uint32_t mask,
                                                        void *user_context);

/**
 * @brief 32 位原子 AND 函数类型。
 *
 * @param address 待原子更新地址。
 * @param mask AND mask。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
typedef unified_error_t (*linux_shm_atomic_and_u32_op_t)(volatile uint32_t *address,
                                                         uint32_t mask,
                                                         void *user_context);

/**
 * @brief 32 位原子 ADD 函数类型。
 *
 * @param address 待原子累加地址。
 * @param value 累加值。
 * @param user_context 平台私有上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
typedef unified_error_t (*linux_shm_atomic_add_u32_op_t)(volatile uint32_t *address,
                                                         uint32_t value,
                                                         void *user_context);

/**
 * @brief Linux 侧共享内存 IPC 平台操作集合。
 */
typedef struct {
    linux_shm_map_region_op_t map_region;       /**< 映射共享内存区域。 */
    linux_shm_unmap_region_op_t unmap_region;   /**< 解除共享内存映射。 */
    linux_shm_cache_op_t cache_flush;           /**< cache flush 操作。 */
    linux_shm_cache_op_t cache_invalidate;      /**< cache invalidate 操作。 */
    linux_shm_barrier_op_t memory_barrier;      /**< 内存屏障操作。 */
    linux_shm_notify_op_t notify;               /**< doorbell/mailbox 通知操作。 */
    linux_shm_atomic_or_u32_op_t atomic_or_u32; /**< pending bit 原子置位。 */
    linux_shm_atomic_and_u32_op_t atomic_and_u32; /**< pending bit 原子清位。 */
    linux_shm_atomic_add_u32_op_t atomic_add_u32; /**< pending 统计原子累加。 */
    void *user_context;                         /**< 平台私有上下文。 */
} linux_shm_platform_ops_t;

/**
 * @brief `/dev/mem` 映射 + 内核 control ioctl 平台上下文。
 */
typedef struct {
    uint32_t context_magic;                                  /**< 上下文 magic。 */
    char control_device_path[LINUX_SHM_CONTROL_DEVICE_PATH_MAX]; /**< control 设备路径。 */
    uintptr_t physical_base;                                /**< 当前映射物理基地址。 */
    size_t region_size;                                     /**< 当前映射大小。 */
    void *mapped_base;                                      /**< 当前映射虚拟基地址。 */
    int control_fd;                                         /**< control 设备 fd。 */
    bool control_enabled;                                   /**< 是否启用 control ioctl。 */
} linux_shm_platform_control_t;

/**
 * @brief 获取 host/mock 默认平台操作集合。
 *
 * @return 默认平台操作集合。
 */
const linux_shm_platform_ops_t *linux_shm_platform_default_ops(void);

/**
 * @brief 获取 /dev/mem 平台操作集合。
 *
 * @return /dev/mem 平台操作集合。
 */
const linux_shm_platform_ops_t *linux_shm_platform_devmem_ops(void);

/**
 * @brief 初始化 Linux shared memory control 上下文。
 *
 * @param context control 上下文。
 */
void linux_shm_platform_control_init(linux_shm_platform_control_t *context);

/**
 * @brief 配置 Linux shared memory control 设备路径。
 *
 * @param context control 上下文。
 * @param control_device_path control 设备路径。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t linux_shm_platform_control_configure(
    linux_shm_platform_control_t *context,
    const char *control_device_path);

/**
 * @brief 关闭 Linux shared memory control 设备。
 *
 * @param context control 上下文。
 */
void linux_shm_platform_control_close(linux_shm_platform_control_t *context);

/**
 * @brief 构造 `/dev/mem` 映射 + control ioctl 平台操作集合。
 *
 * @param out_ops 输出平台操作集合。
 * @param context control 上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t linux_shm_platform_make_devmem_control_ops(
    linux_shm_platform_ops_t *out_ops,
    linux_shm_platform_control_t *context);

#ifdef __cplusplus
}
#endif

#endif /* LINUX_SHM_PLATFORM_H */
