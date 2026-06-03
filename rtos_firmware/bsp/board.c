/**
 * @file board.c
 * @brief rtos_firmware board 占位初始化。
 * @author Yukikaze
 */
#include "rtos_bsp.h"

#include <string.h>

#include "rtos_firmware_config.h"
#include "rtos_shm_platform.h"

/**
 * @brief 初始化 BSP 骨架。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_init(void)
{
    unified_error_t result; /**< 当前子模块初始化结果。 */

    result = rtos_bsp_board_init();
    if (result != UNIFIED_OK) {
        /* board 初始化失败时停止后续 BSP 初始化。 */
        return result;
    }

    result = rtos_bsp_clock_init();
    if (result != UNIFIED_OK) {
        /* clock 初始化失败时不能继续配置外设。 */
        return result;
    }

    result = rtos_bsp_pinmux_init();
    if (result != UNIFIED_OK) {
        /* pinmux 初始化失败时外设管脚状态不可预测。 */
        return result;
    }

    result = rtos_bsp_interrupt_init();
    if (result != UNIFIED_OK) {
        /* interrupt 初始化失败时任务无法可靠接收硬件事件。 */
        return result;
    }

    return UNIFIED_OK;
}

/**
 * @brief 初始化 board 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_board_init(void)
{
    /* 当前阶段只建立工程骨架，真实 board 初始化后续接入。 */
    return UNIFIED_OK;
}

/**
 * @brief 获取 BSP 配置的共享内存物理/总线基地址。
 *
 * @return 共享内存基地址，0 表示当前构建未绑定真实板端地址。
 */
uintptr_t rtos_bsp_get_shared_memory_base(void)
{
    uintptr_t shared_memory_base; /**< 编译期配置的共享内存基地址。 */

    shared_memory_base = (uintptr_t)RTOS_FIRMWARE_SHARED_MEMORY_BASE;
    return shared_memory_base;
}

/**
 * @brief 获取 BSP 配置的共享内存大小。
 *
 * @return 共享内存大小，当前 ABI 必须等于 PUT_SHM_REGION_SIZE。
 */
size_t rtos_bsp_get_shared_memory_size(void)
{
    size_t shared_memory_size; /**< 编译期配置的共享内存大小。 */

    shared_memory_size = (size_t)RTOS_FIRMWARE_SHARED_MEMORY_SIZE;
    return shared_memory_size;
}

/**
 * @brief 获取 BSP 配置的共享内存 region 指针。
 *
 * @return 共享内存 region 指针；未配置真实地址时返回 NULL。
 */
put_shm_region_t *rtos_bsp_get_shared_memory_region(void)
{
    uintptr_t shared_memory_base; /**< 编译期配置的共享内存基地址。 */

    shared_memory_base = rtos_bsp_get_shared_memory_base();
    if (shared_memory_base == 0u) {
        /* host/mock 构建或板端尚未配置地址时，不能返回伪 region。 */
        return 0;
    }

    if (rtos_bsp_get_shared_memory_size() != PUT_SHM_REGION_SIZE) {
        /* 编译期大小与 ABI 不一致时拒绝绑定，避免越界访问共享内存。 */
        return 0;
    }

    if ((shared_memory_base % PUT_SHM_CACHE_LINE_SIZE) != 0u) {
        /* 共享内存基地址必须按 cache line 对齐，保证跨核 cache 同步边界清晰。 */
        return 0;
    }

    return (put_shm_region_t *)shared_memory_base;
}

/**
 * @brief 获取 BSP 当前绑定的共享内存平台操作集合。
 *
 * 默认实现返回 host/mock 平台操作；真实板端移植时在 BSP 层替换为
 * cache maintenance 和 Mailbox/CMDQU doorbell 操作。
 *
 * @return 平台操作集合指针。
 */
const rtos_shm_platform_ops_t *rtos_bsp_get_shm_platform_ops(void)
{
    const rtos_shm_platform_ops_t *platform_ops; /**< 当前 BSP 共享内存平台操作集合。 */

    platform_ops = rtos_shm_platform_default_ops();
    return platform_ops;
}

/**
 * @brief 根据 BSP 当前配置生成 runtime 初始化配置。
 *
 * @param out_config 输出 runtime 初始化配置。
 * @param time_source runtime 时间源。
 * @param time_context 时间源用户上下文。
 * @return UNIFIED_OK 表示生成成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_make_runtime_config(rtos_runtime_config_t *out_config,
                                             rtos_router_time_source_t time_source,
                                             void *time_context)
{
    put_shm_region_t *shared_region;                   /**< BSP 绑定的共享内存 region。 */
    const rtos_shm_platform_ops_t *platform_ops;       /**< BSP 绑定的平台操作集合。 */

    if (out_config == 0) {
        /* 输出配置为空时无法写入 runtime 初始化参数。 */
        return UNIFIED_ERR_NULL;
    }

    shared_region = rtos_bsp_get_shared_memory_region();
    if (shared_region == 0) {
        /* 板端地址未配置或配置非法时，runtime 不应 attach 伪 region。 */
        (void)memset(out_config, 0, sizeof(*out_config));
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    platform_ops = rtos_bsp_get_shm_platform_ops();
    if (platform_ops == 0) {
        /* 平台操作集合缺失时不能执行 cache 同步或 doorbell 通知。 */
        (void)memset(out_config, 0, sizeof(*out_config));
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    (void)memset(out_config, 0, sizeof(*out_config));
    out_config->region = shared_region;
    out_config->platform_ops = platform_ops;
    out_config->time_source = time_source;
    out_config->time_context = time_context;
    out_config->scheduler_budget = 0u;
    out_config->recovery_budget = 0u;
    return UNIFIED_OK;
}
