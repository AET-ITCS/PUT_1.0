/**
 * @file rtos_entry.c
 * @brief rtos_firmware 顶层初始化流程。
 * @author Yukikaze
 */
#include "rtos_firmware.h"

#include "rtos_bsp.h"
#include "rtos_can.h"
#include "rtos_watchdog.h"

/**
 * @brief 初始化 rtos_firmware 骨架模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_firmware_main(void)
{
    unified_error_t result; /**< 当前阶段初始化结果。 */

    result = rtos_bsp_init();
    if (result != UNIFIED_OK) {
        /* BSP 初始化失败时立即返回，避免后续模块建立在错误硬件状态上。 */
        return result;
    }

    result = rtos_can_init();
    if (result != UNIFIED_OK) {
        /* CAN 占位层失败时直接返回，后续真实驱动可在这里接恢复策略。 */
        return result;
    }

    result = rtos_watchdog_init();
    if (result != UNIFIED_OK) {
        /* watchdog 占位层失败时直接返回，避免系统无保护地继续运行。 */
        return result;
    }

    return UNIFIED_OK;
}

/**
 * @brief 初始化正式 runtime 入口上下文。
 *
 * @param runtime runtime 上下文。
 * @param config runtime 初始化配置。
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_firmware_runtime_init(rtos_runtime_context_t *runtime,
                                           const rtos_runtime_config_t *config)
{
    unified_error_t result; /**< 当前 runtime 初始化结果。 */

    result = rtos_runtime_init(runtime, config);
    if (result != UNIFIED_OK) {
        /* runtime 初始化失败时由调用方决定是否进入板级 recovery。 */
        return result;
    }

    return UNIFIED_OK;
}

/**
 * @brief 使用 BSP 当前共享内存配置初始化正式 runtime 入口上下文。
 *
 * @param runtime runtime 上下文。
 * @param time_source runtime 时间源。
 * @param time_context 时间源用户上下文。
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_firmware_runtime_init_from_bsp(rtos_runtime_context_t *runtime,
                                                    rtos_router_time_source_t time_source,
                                                    void *time_context)
{
    rtos_runtime_config_t config; /**< 由 BSP 地址和平台 ops 生成的 runtime 配置。 */
    unified_error_t result;      /**< 当前 BSP 配置或 runtime 初始化结果。 */

    result = rtos_bsp_make_runtime_config(&config, time_source, time_context);
    if (result != UNIFIED_OK) {
        /* BSP 尚未绑定共享内存或平台 ops 时，不继续 attach runtime。 */
        return result;
    }

    result = rtos_firmware_runtime_init(runtime, &config);
    if (result != UNIFIED_OK) {
        /* runtime attach 失败时保持错误码向上传递，便于板端启动策略处理。 */
        return result;
    }

    return UNIFIED_OK;
}

/**
 * @brief 单步执行正式 runtime 入口。
 *
 * @param runtime runtime 上下文。
 * @param trigger 本轮触发来源。
 * @return 本轮处理动作数量。
 */
uint32_t rtos_firmware_runtime_run_once(rtos_runtime_context_t *runtime,
                                        rtos_runtime_trigger_t trigger)
{
    uint32_t processed_count; /**< 本轮处理动作数量。 */

    processed_count = rtos_runtime_run_once(runtime, trigger);
    return processed_count;
}
