/**
 * @file rtos_mailbox_event.c
 * @brief P2 host/mock mailbox event 实现。
 * @author Yukikaze
 */
#include "rtos_mailbox.h"

/**
 * @brief 触发 P2 host/mock mailbox event 信号。
 *
 * @return UNIFIED_OK 表示 host/mock 唤醒成功。
 */
unified_error_t rtos_mailbox_event_signal(void)
{
    return UNIFIED_OK;
}
