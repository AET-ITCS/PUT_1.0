/* FreeRTOS comm CAN 转发入口：接收统一帧，校验后交给 CAN driver。 */
#ifndef RTOS_CAN_FORWARD_H
#define RTOS_CAN_FORWARD_H

#include "error_code.h"
#include "unified_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

unified_error_t gateway_forward_init(void);
unified_error_t rtos_can_forward_submit_frame(const unified_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_FORWARD_H */
