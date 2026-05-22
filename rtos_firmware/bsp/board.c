/**
 * @file board.c
 * @brief rtos_firmware board 占位初始化。
 * @author Yukikaze
 */
#include "rtos_bsp.h"

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
