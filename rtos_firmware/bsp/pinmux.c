/**
 * @file pinmux.c
 * @brief rtos_firmware pinmux 占位初始化。
 * @author Yukikaze
 */
#include "rtos_bsp.h"

/**
 * @brief 初始化 pinmux 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_pinmux_init(void)
{
    /* 当前阶段不配置真实引脚复用，后续 SPI2/GPIO14 接入时补齐。 */
    return UNIFIED_OK;
}
