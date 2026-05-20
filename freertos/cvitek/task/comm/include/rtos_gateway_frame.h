/* FreeRTOS comm 帧适配：统一帧校验和 RTOS 内部 CAN 消息定义。 */
#ifndef RTOS_GATEWAY_FRAME_H
#define RTOS_GATEWAY_FRAME_H

#include <stdint.h>

#include "unified_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTOS_FRAME_VALIDATE_OK = 0,
    RTOS_FRAME_VALIDATE_NULL,
    RTOS_FRAME_VALIDATE_MAGIC,
    RTOS_FRAME_VALIDATE_VERSION,
    RTOS_FRAME_VALIDATE_TYPE,
    RTOS_FRAME_VALIDATE_SOURCE_PROTOCOL,
    RTOS_FRAME_VALIDATE_VEHICLE_TYPE,
    RTOS_FRAME_VALIDATE_FLAG,
    RTOS_FRAME_VALIDATE_CAN_ID,
    RTOS_FRAME_VALIDATE_DLC,
    RTOS_FRAME_VALIDATE_CRC,
} rtos_frame_validate_error_t;

typedef struct {
    uint32_t can_id;
    uint8_t can_dlc;
    uint8_t can_flags;
    uint8_t can_data[UNIFIED_CAN_CLASSIC_DATA_MAX_LEN];
    uint32_t sequence;
    uint32_t timestamp_ms;
} rtos_can_message_t;

rtos_frame_validate_error_t rtos_gateway_frame_validate(const unified_frame_t *frame,
                                                        rtos_can_message_t *out_msg);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_GATEWAY_FRAME_H */
