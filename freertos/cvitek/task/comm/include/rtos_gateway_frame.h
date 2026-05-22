/**
 * @file rtos_gateway_frame.h
 * @brief Legacy unified_frame_t 校验适配接口。
 *
 * 本文件保留旧实验链路的 frame validator，便于回归和参考；当前正式
 * FreeRTOS CAN 主链路不依赖该接口。
 */
#ifndef RTOS_GATEWAY_FRAME_H
#define RTOS_GATEWAY_FRAME_H

#include <stdint.h>

#include "rtos_can_message.h"
#include "unified_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTOS_FRAME_VALIDATE_OK = 0,             // 校验成功。
    RTOS_FRAME_VALIDATE_NULL,               // 输入 frame 为空。
    RTOS_FRAME_VALIDATE_MAGIC,              // magic 字段错误。
    RTOS_FRAME_VALIDATE_VERSION,            // version 字段错误。
    RTOS_FRAME_VALIDATE_TYPE,               // frame type 不支持。
    RTOS_FRAME_VALIDATE_SOURCE_PROTOCOL,    // source protocol 不支持。
    RTOS_FRAME_VALIDATE_VEHICLE_TYPE,       // vehicle type 不支持。
    RTOS_FRAME_VALIDATE_FLAG,               // CAN flag 不支持。
    RTOS_FRAME_VALIDATE_CAN_ID,             // CAN ID 越界。
    RTOS_FRAME_VALIDATE_DLC,                // CAN DLC 越界。
    RTOS_FRAME_VALIDATE_CRC,                // CRC 校验失败。
} rtos_frame_validate_error_t;              // Legacy frame validator 返回码。

rtos_frame_validate_error_t rtos_gateway_frame_validate(const unified_frame_t *frame,
                                                        rtos_can_message_t *out_msg);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_GATEWAY_FRAME_H */
