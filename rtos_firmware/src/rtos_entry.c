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
