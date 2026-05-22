/**
 * @file rtos_can_forward.h
 * @brief FreeRTOS 小核 CAN 转发核心接口。
 *
 * 本模块接收协议适配层输出的 @ref rtos_can_message_t，完成 CAN 层校验、
 * 软件 TX 队列管理、发送重试和错误恢复。
 */
#ifndef RTOS_CAN_FORWARD_H
#define RTOS_CAN_FORWARD_H

#include "error_code.h"
#include "rtos_can_message.h"

#ifdef __cplusplus
extern "C" {
#endif

unified_error_t gateway_forward_init(void);

unified_error_t rtos_can_forward_submit_message(const rtos_can_message_t *message);

unified_error_t rtos_can_forward_enqueue_message(const rtos_can_message_t *message);

uint32_t rtos_can_forward_drain_tx_queue_once(void);

uint32_t rtos_can_forward_purge_tx_queue(void);

uint32_t rtos_can_forward_get_tx_queue_depth(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_FORWARD_H */
