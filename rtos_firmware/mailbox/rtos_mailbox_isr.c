/**
 * @file rtos_mailbox_isr.c
 * @brief P2 host/mock mailbox ISR 实现。
 * @author Yukikaze
 */
#include "rtos_mailbox.h"

/**
 * @brief 确认并清理 P2 host/mock mailbox 中断。
 *
 * @return UNIFIED_OK 表示 host/mock acknowledge 成功。
 */
unified_error_t rtos_mailbox_isr_acknowledge(void)
{
    return UNIFIED_OK;
}
