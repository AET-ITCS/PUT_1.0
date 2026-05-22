/**
 * @file rtos_watchdog.c
 * @brief rtos_firmware watchdog 占位实现。
 * @author Yukikaze
 */
#include "rtos_watchdog.h"

/**
 * @brief 初始化 watchdog 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_watchdog_init(void)
{
    /* 当前阶段不触碰真实硬件看门狗，后续 recovery 任务接入时补齐。 */
    return UNIFIED_OK;
}
