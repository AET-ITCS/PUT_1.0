/**
 * @file rtos_firmware.h
 * @brief rtos_firmware 顶层入口接口。
 * @author Yukikaze
 */
#ifndef RTOS_FIRMWARE_H
#define RTOS_FIRMWARE_H

#include "error_code.h"
#include "rtos_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 rtos_firmware 骨架模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_firmware_main(void);

/**
 * @brief 初始化正式 runtime 入口上下文。
 *
 * @param runtime runtime 上下文。
 * @param config runtime 初始化配置。
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_firmware_runtime_init(rtos_runtime_context_t *runtime,
                                           const rtos_runtime_config_t *config);

/**
 * @brief 单步执行正式 runtime 入口。
 *
 * @param runtime runtime 上下文。
 * @param trigger 本轮触发来源。
 * @return 本轮处理动作数量。
 */
uint32_t rtos_firmware_runtime_run_once(rtos_runtime_context_t *runtime,
                                        rtos_runtime_trigger_t trigger);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_FIRMWARE_H */
