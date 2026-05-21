/**
 * @file rtos_can_message.h
 * @brief FreeRTOS 小核 CAN 层稳定内部消息定义。
 *
 * 本文件只描述小核 CAN 层使用的经典 CAN 报文，不绑定大小核共享内存
 * payload、统一协议或外部业务协议。协议适配层负责把未来 TBD payload
 * 转换为本文件定义的 @ref rtos_can_message_t。
 */
#ifndef RTOS_CAN_MESSAGE_H
#define RTOS_CAN_MESSAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTOS_CAN_CLASSIC_DATA_MAX_LEN 8u            //经典 CAN 数据区最大长度

#define RTOS_CAN_STANDARD_ID_MAX 0x7FFu             //标准帧 ID 范围 0..0x7FF

#define RTOS_CAN_EXTENDED_ID_MAX 0x1FFFFFFFu        //扩展帧 ID 范围 0..0x1FFFFFFF

/**
 * @brief 小核 CAN 层支持的 CAN flag。
 *
 * v1 只支持经典 CAN 的标准帧和扩展帧，不支持 CAN FD、BRS 或 RTR。
 */
typedef enum {
    RTOS_CAN_FLAG_NONE = 0x00u,             //标准帧

    RTOS_CAN_FLAG_EXTENDED_ID = (uint8_t)(1u << 0),         //扩展帧
} rtos_can_flag_t;

/**
 * @brief 小核内部 CAN 报文。
 *
 * CAN 转发层只校验本结构中的 ID、DLC、flag 和 data。业务字段、
 * 来源信息、时间戳、序号和 payload 校验等由协议适配层负责。
 */
typedef struct {
    uint32_t can_id;                // CAN ID；标准帧范围 0..0x7FF，扩展帧范围 0..0x1FFFFFFF

    uint8_t can_dlc;                // CAN DLC；经典 CAN 范围 0..8，CAN FD 范围 0..64

    uint8_t can_flags;              // CAN flag bitmask；目前仅支持 RTOS_CAN_FLAG_EXTENDED_ID

    uint8_t can_data[RTOS_CAN_CLASSIC_DATA_MAX_LEN];            //未来可扩展为 CAN FD 的数据区
} rtos_can_message_t;

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_MESSAGE_H */
