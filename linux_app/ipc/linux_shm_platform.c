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

/** @brief control 上下文 magic。 */
#define LINUX_SHM_PLATFORM_CONTROL_MAGIC 0x50555443u

/** @brief control 设备 open flags，兼容未暴露 O_CLOEXEC 的构建环境。 */
#ifdef O_CLOEXEC
#define LINUX_SHM_CONTROL_OPEN_FLAGS (O_RDWR | O_CLOEXEC)
#else
#define LINUX_SHM_CONTROL_OPEN_FLAGS O_RDWR
#endif

/**
 * @brief /dev/mem 映射上下文。
 */
typedef struct {
    int fd; /**< /dev/mem 文件描述符。 */
} linux_shm_devmem_mapping_t;

/**
 * @brief 判断 control 上下文是否有效。
 *
 * @param context control 上下文。
 * @return true 表示有效，false 表示无效。
 */
static bool is_control_context_ready(const linux_shm_platform_control_t *context)
{
    if ((context == 0) ||
        (context->context_magic != LINUX_SHM_PLATFORM_CONTROL_MAGIC) ||
        !context->control_enabled ||
        (context->control_device_path[0] == '\0')) {
        /* 未初始化、未启用或没有设备路径时不能使用 control ioctl。 */
        return false;
    }

    return true;
}

/**
 * @brief 懒打开 control 设备。
 *
 * @param context control 上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t open_control_device(linux_shm_platform_control_t *context)
{
    if (!is_control_context_ready(context)) {
        /* control 上下文未就绪时不能执行 ioctl。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    if (context->control_fd >= 0) {
        /* 已打开时直接复用 fd。 */
        return UNIFIED_OK;
    }

    context->control_fd = open(context->control_device_path, LINUX_SHM_CONTROL_OPEN_FLAGS);
    if (context->control_fd < 0) {
        /* 设备未创建或权限不足时视为 IPC 平台未就绪。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    return UNIFIED_OK;
}

/**
 * @brief 将虚拟地址范围转换为 control ioctl range。
 *
 * @param context control 上下文。
 * @param address 虚拟地址。
 * @param length 字节数。
 * @param out_range 输出 ioctl range。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t make_control_range(const linux_shm_platform_control_t *context,
                                          const void *address,
                                          size_t length,
                                          linux_shm_control_range_t *out_range)
{
    uintptr_t mapped_base;   /**< 映射虚拟基地址。 */
    uintptr_t target;        /**< 待同步虚拟地址。 */
    uintptr_t offset;        /**< 相对 region 起点偏移。 */

    if ((context == 0) || (address == 0) || (out_range == 0) || (length == 0u)) {
        /* cache 同步范围必须完整指定。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    if ((context->mapped_base == 0) || (context->region_size == 0u)) {
        /* 尚未完成 /dev/mem 映射时无法换算物理地址。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    mapped_base = (uintptr_t)context->mapped_base;
    target = (uintptr_t)address;
    if (target < mapped_base) {
        /* 待同步地址不能落在共享内存映射之前。 */
        return UNIFIED_ERR_LENGTH;
    }

    offset = target - mapped_base;
    if ((offset > context->region_size) ||
        (length > (context->region_size - offset))) {
        /* 待同步范围必须完全位于共享内存 region 内。 */
        return UNIFIED_ERR_LENGTH;
    }

    out_range->physical_address = (uint64_t)context->physical_base + (uint64_t)offset;
    out_range->offset = (uint64_t)offset;
    out_range->length = (uint64_t)length;
    return UNIFIED_OK;
}

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
    linux_shm_platform_control_t *control; /**< control 上下文。 */
    void *address;                       /**< mmap 后的虚拟地址。 */

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
    control = (linux_shm_platform_control_t *)user_context;
    if ((control != 0) &&
        (control->context_magic == LINUX_SHM_PLATFORM_CONTROL_MAGIC)) {
        /* 记录映射信息，供后续 cache maintenance ioctl 换算物理地址。 */
        control->physical_base = physical_base;
        control->region_size = region_size;
        control->mapped_base = address;
    }
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
    linux_shm_platform_control_t *control; /**< control 上下文。 */

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

    control = (linux_shm_platform_control_t *)user_context;
    if ((control != 0) &&
        (control->context_magic == LINUX_SHM_PLATFORM_CONTROL_MAGIC)) {
        /* 解除映射时同步关闭 control fd，避免退出路径泄漏设备句柄。 */
        linux_shm_platform_control_close(control);
        control->mapped_base = 0;
        control->region_size = 0u;
        control->physical_base = 0u;
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
 * @brief 通过 control ioctl 执行 cache flush。
 *
 * @param address 待 flush 地址。
 * @param length 待 flush 长度。
 * @param user_context control 上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t control_cache_flush(const void *address,
                                           size_t length,
                                           void *user_context)
{
    linux_shm_platform_control_t *control; /**< control 上下文。 */
    linux_shm_control_range_t range;       /**< ioctl cache range。 */
    unified_error_t result;                /**< 操作结果。 */

    control = (linux_shm_platform_control_t *)user_context;
    result = make_control_range(control, address, length, &range);
    if (result != UNIFIED_OK) {
        /* range 换算失败时不能触发驱动 cache 操作。 */
        return result;
    }

    result = open_control_device(control);
    if (result != UNIFIED_OK) {
        /* control 设备不可用时向上报告平台未就绪。 */
        return result;
    }

    if (ioctl(control->control_fd, LINUX_SHM_CONTROL_IOCTL_CACHE_FLUSH, &range) != 0) {
        /* 驱动执行失败时由 IPC 层累计 cache_sync_error_count。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    return UNIFIED_OK;
}

/**
 * @brief 通过 control ioctl 执行 cache invalidate。
 *
 * @param address 待 invalidate 地址。
 * @param length 待 invalidate 长度。
 * @param user_context control 上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t control_cache_invalidate(const void *address,
                                                size_t length,
                                                void *user_context)
{
    linux_shm_platform_control_t *control; /**< control 上下文。 */
    linux_shm_control_range_t range;       /**< ioctl cache range。 */
    unified_error_t result;                /**< 操作结果。 */

    control = (linux_shm_platform_control_t *)user_context;
    result = make_control_range(control, address, length, &range);
    if (result != UNIFIED_OK) {
        /* range 换算失败时不能触发驱动 cache 操作。 */
        return result;
    }

    result = open_control_device(control);
    if (result != UNIFIED_OK) {
        /* control 设备不可用时向上报告平台未就绪。 */
        return result;
    }

    if (ioctl(control->control_fd, LINUX_SHM_CONTROL_IOCTL_CACHE_INVALIDATE, &range) != 0) {
        /* 驱动执行失败时由 IPC 层累计 cache_sync_error_count。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

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
 * @brief 通过 control ioctl 触发 doorbell/mailbox。
 *
 * @param direction 通知方向。
 * @param user_context control 上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t control_notify(put_shm_direction_t direction, void *user_context)
{
    linux_shm_platform_control_t *control; /**< control 上下文。 */
    linux_shm_control_notify_t request;    /**< ioctl doorbell 请求。 */
    unified_error_t result;                /**< 操作结果。 */

    control = (linux_shm_platform_control_t *)user_context;
    result = open_control_device(control);
    if (result != UNIFIED_OK) {
        /* control 设备不可用时，descriptor 仍已发布，由 pending/periodic 兜底。 */
        return UNIFIED_ERR_IPC_NOTIFY_FAILED;
    }

    memset(&request, 0, sizeof(request));
    request.direction = (uint32_t)direction;
    if (ioctl(control->control_fd, LINUX_SHM_CONTROL_IOCTL_NOTIFY, &request) != 0) {
        /* 驱动通知失败不代表 descriptor 未发布。 */
        return UNIFIED_ERR_IPC_NOTIFY_FAILED;
    }

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

/**
 * @brief 初始化 Linux shared memory control 上下文。
 *
 * @param context control 上下文。
 */
void linux_shm_platform_control_init(linux_shm_platform_control_t *context)
{
    if (context == 0) {
        return;
    }

    memset(context, 0, sizeof(*context));
    context->context_magic = LINUX_SHM_PLATFORM_CONTROL_MAGIC;
    context->control_fd = -1;
}

/**
 * @brief 配置 Linux shared memory control 设备路径。
 *
 * @param context control 上下文。
 * @param control_device_path control 设备路径。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t linux_shm_platform_control_configure(
    linux_shm_platform_control_t *context,
    const char *control_device_path)
{
    size_t path_length; /**< control 设备路径长度。 */

    if ((context == 0) || (control_device_path == 0)) {
        /* 上下文和路径都是配置 control ioctl 的必要输入。 */
        return UNIFIED_ERR_NULL;
    }

    if (context->context_magic != LINUX_SHM_PLATFORM_CONTROL_MAGIC) {
        /* 调用方必须先初始化 control 上下文。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    path_length = strlen(control_device_path);
    if ((path_length == 0u) ||
        (path_length >= sizeof(context->control_device_path))) {
        /* 空路径或超长路径都会导致后续 open 行为不明确。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    memcpy(context->control_device_path, control_device_path, path_length + 1u);
    context->control_enabled = true;
    return UNIFIED_OK;
}

/**
 * @brief 关闭 Linux shared memory control 设备。
 *
 * @param context control 上下文。
 */
void linux_shm_platform_control_close(linux_shm_platform_control_t *context)
{
    if ((context == 0) ||
        (context->context_magic != LINUX_SHM_PLATFORM_CONTROL_MAGIC)) {
        /* 未初始化上下文无需关闭。 */
        return;
    }

    if (context->control_fd >= 0) {
        /* 关闭 control fd，失败只作为清理期错误忽略。 */
        (void)close(context->control_fd);
        context->control_fd = -1;
    }
}

/**
 * @brief 构造 `/dev/mem` 映射 + control ioctl 平台操作集合。
 *
 * @param out_ops 输出平台操作集合。
 * @param context control 上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t linux_shm_platform_make_devmem_control_ops(
    linux_shm_platform_ops_t *out_ops,
    linux_shm_platform_control_t *context)
{
    if ((out_ops == 0) || (context == 0)) {
        /* 输出 ops 和 control 上下文都不能为空。 */
        return UNIFIED_ERR_NULL;
    }

    if (!is_control_context_ready(context)) {
        /* control 设备未配置时不能构造真实 cache/doorbell ops。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    out_ops->map_region = devmem_map_region;
    out_ops->unmap_region = devmem_unmap_region;
    out_ops->cache_flush = control_cache_flush;
    out_ops->cache_invalidate = control_cache_invalidate;
    out_ops->memory_barrier = default_memory_barrier;
    out_ops->notify = control_notify;
    out_ops->atomic_or_u32 = default_atomic_or_u32;
    out_ops->atomic_and_u32 = default_atomic_and_u32;
    out_ops->atomic_add_u32 = default_atomic_add_u32;
    out_ops->user_context = context;
    return UNIFIED_OK;
}
