/**
 * @file rtos_mailbox_isr.c
 * @brief P1 mailbox ISR 占位实现。
 * @author Yukikaze
 */
#include "rtos_mailbox.h"

/**
 * @brief 确认并清理 P1 mailbox ISR 占位中断。
 *
 * @return UNIFIED_OK 表示占位操作成功。
 */
unified_error_t rtos_mailbox_isr_acknowledge(void)
{
    return UNIFIED_OK;
}
