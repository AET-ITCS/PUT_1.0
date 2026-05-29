/**
 * @file rtos_mailbox.h
 * @brief P1 mailbox/doorbell 占位接口。
 * @author Yukikaze
 */
#ifndef RTOS_MAILBOX_H
#define RTOS_MAILBOX_H

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

unified_error_t rtos_mailbox_port_init(void);
unified_error_t rtos_mailbox_isr_acknowledge(void);
unified_error_t rtos_mailbox_event_signal(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_MAILBOX_H */
