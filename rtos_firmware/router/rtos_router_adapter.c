/**
 * @file rtos_router_adapter.c
 * @brief P1 适配层边界占位实现。
 * @author Yukikaze
 */
#include "rtos_router.h"

/**
 * @brief 检查 P1 适配层边界符号是否可链接。
 *
 * @return UNIFIED_OK 表示 P1 边界占位可用。
 */
unified_error_t rtos_router_adapter_p1_boundary_check(void)
{
    /* descriptor 到路由输入的真实适配留到 P2 IPC 集成阶段。 */
    return UNIFIED_OK;
}
