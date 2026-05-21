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

/** @brief Legacy frame validator 返回码。 */
typedef enum {

    RTOS_FRAME_VALIDATE_OK = 0,             //校验成功
    /** @brief 输入 frame 为空。 */
    RTOS_FRAME_VALIDATE_NULL,               
    /** @brief magic 字段错误。 */
    RTOS_FRAME_VALIDATE_MAGIC,
    /** @brief version 字段错误。 */
    RTOS_FRAME_VALIDATE_VERSION,
    /** @brief frame type 不支持。 */
    RTOS_FRAME_VALIDATE_TYPE,
    /** @brief source protocol 不支持。 */
    RTOS_FRAME_VALIDATE_SOURCE_PROTOCOL,
    /** @brief vehicle type 不支持。 */
    RTOS_FRAME_VALIDATE_VEHICLE_TYPE,
    /** @brief CAN flag 不支持。 */
    RTOS_FRAME_VALIDATE_FLAG,
    /** @brief CAN ID 越界。 */
    RTOS_FRAME_VALIDATE_CAN_ID,
    /** @brief CAN DLC 越界。 */
    RTOS_FRAME_VALIDATE_DLC,
    /** @brief CRC 校验失败。 */
    RTOS_FRAME_VALIDATE_CRC,
} rtos_frame_validate_error_t;

/**
 * @brief 校验 legacy unified_frame_t 并转换为内部 CAN 消息。
 * @param frame 待校验 legacy frame。
 * @param[out] out_msg 校验成功时输出 CAN 消息；可为 NULL。
 * @return legacy validator 返回码。
 */
rtos_frame_validate_error_t rtos_gateway_frame_validate(const unified_frame_t *frame,
                                                        rtos_can_message_t *out_msg);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_GATEWAY_FRAME_H */
