/* FreeRTOS comm 小核入口：初始化 CAN 转发 mock 链路。 */
#include "rtos_can_forward.h"

int comm_main(void)
{
    return (gateway_forward_init() == UNIFIED_OK) ? 0 : -1;
}
