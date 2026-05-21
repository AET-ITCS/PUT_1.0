/* FreeRTOS comm CAN 转发入口：接收协议适配层输出的内部 CAN 消息。 */
#ifndef RTOS_CAN_FORWARD_H
#define RTOS_CAN_FORWARD_H

#include "error_code.h"
#include "rtos_can_message.h"

#ifdef __cplusplus
extern "C" {
#endif

unified_error_t gateway_forward_init(void);
unified_error_t rtos_can_forward_submit_message(const rtos_can_message_t *message);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_FORWARD_H */
