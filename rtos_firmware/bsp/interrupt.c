/**
 * @file interrupt.c
 * @brief rtos_firmware interrupt 占位初始化。
 * @author Yukikaze
 */
#include "rtos_bsp.h"

/**
 * @brief 初始化 interrupt 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_interrupt_init(void)
{
    /* 当前阶段不注册真实中断，后续 mailbox/GPIO14 接入时补齐。 */
    return UNIFIED_OK;
}
