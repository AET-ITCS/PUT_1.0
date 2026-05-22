/**
 * @file clock.c
 * @brief rtos_firmware clock 占位初始化。
 * @author Yukikaze
 */
#include "rtos_bsp.h"

/**
 * @brief 初始化 clock 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_clock_init(void)
{
    /* 当前阶段不修改真实时钟树，后续 BSP 接入时替换此占位实现。 */
    return UNIFIED_OK;
}
