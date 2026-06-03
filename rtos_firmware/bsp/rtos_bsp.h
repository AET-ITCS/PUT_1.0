/**
 * @file rtos_bsp.h
 * @brief rtos_firmware BSP 占位接口。
 * @author Yukikaze
 */
#ifndef RTOS_BSP_H
#define RTOS_BSP_H

#include <stddef.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_runtime.h"
#include "rtos_shm_platform.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 BSP 骨架。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_init(void);

/**
 * @brief 初始化 board 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_board_init(void);

/**
 * @brief 初始化 clock 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_clock_init(void);

/**
 * @brief 初始化 pinmux 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_pinmux_init(void);

/**
 * @brief 初始化 interrupt 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_interrupt_init(void);

/**
 * @brief 获取 BSP 配置的共享内存物理/总线基地址。
 *
 * @return 共享内存基地址，0 表示当前构建未绑定真实板端地址。
 */
uintptr_t rtos_bsp_get_shared_memory_base(void);

/**
 * @brief 获取 BSP 配置的共享内存大小。
 *
 * @return 共享内存大小，当前 ABI 必须等于 PUT_SHM_REGION_SIZE。
 */
size_t rtos_bsp_get_shared_memory_size(void);

/**
 * @brief 获取 BSP 配置的共享内存 region 指针。
 *
 * @return 共享内存 region 指针；未配置真实地址时返回 NULL。
 */
put_shm_region_t *rtos_bsp_get_shared_memory_region(void);

/**
 * @brief 获取 BSP 当前绑定的共享内存平台操作集合。
 *
 * 默认实现返回 host/mock 平台操作；真实板端移植时在 BSP 层替换为
 * cache maintenance 和 Mailbox/CMDQU doorbell 操作。
 *
 * @return 平台操作集合指针。
 */
const rtos_shm_platform_ops_t *rtos_bsp_get_shm_platform_ops(void);

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
                                             void *time_context);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_BSP_H */
