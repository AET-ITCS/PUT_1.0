/**
 * @file comm_main.c
 * @brief FreeRTOS comm 小核入口。
 */
#include "rtos_can_forward.h"

/**
 * @brief 初始化 comm 转发链路并返回平台入口状态码。
 * @return 0 表示初始化成功，-1 表示初始化失败。
 */
int comm_main(void)
{
    return (gateway_forward_init() == UNIFIED_OK) ? 0 : -1;
}
