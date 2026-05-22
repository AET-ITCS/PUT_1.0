/**
 * @file rtos_can.c
 * @brief rtos_firmware CAN 占位实现。
 * @author Yukikaze
 */
#include "rtos_can.h"

/**
 * @brief 初始化 CAN 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_can_init(void)
{
    /* 当前阶段不接入 CAN TX queue 或 XL2515 驱动。 */
    return UNIFIED_OK;
}
