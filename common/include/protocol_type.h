/* 公共协议类型定义：描述外部来源协议、车身业务类型和统一帧/CAN 标志。 */
#ifndef PROTOCOL_TYPE_H
#define PROTOCOL_TYPE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 外部来源协议类型。
 *
 * 该枚举描述数据最初来自哪一路外部通信方式，不代表 CAN 官方协议类型。
 */
typedef enum {
    PROTOCOL_TYPE_UNKNOWN = 0x00,
    PROTOCOL_TYPE_4G = 0x01,
    PROTOCOL_TYPE_WIFI = 0x02,
    PROTOCOL_TYPE_BLUETOOTH = 0x03,
    PROTOCOL_TYPE_ETHERNET = 0x04,
    PROTOCOL_TYPE_RS485 = 0x05,
    PROTOCOL_TYPE_CLOUD_MQTT = 0x06,
} protocol_type_t;

/**
 * @brief 车身业务类型。
 *
 * 数值参考老师《车身网联控制协议 v1.0 草案》中的业务 type，
 * 但本项目主流程不要求外部输入先转换为 anyMSG。
 */
typedef enum {
    VEHICLE_MSG_TYPE_RAW_CAN = 0x00,
    VEHICLE_MSG_TYPE_SEAT_CONTROL = 0x47,
    VEHICLE_MSG_TYPE_SEAT_FEEDBACK = 0x48,
    VEHICLE_MSG_TYPE_LIGHT_CONTROL = 0x49,
    VEHICLE_MSG_TYPE_LIGHT_FEEDBACK = 0x50,
    VEHICLE_MSG_TYPE_WINDOW_CONTROL = 0x51,
    VEHICLE_MSG_TYPE_WINDOW_FEEDBACK = 0x52,
} vehicle_msg_type_t;

/**
 * @brief 大核到小核的内部统一帧类型。
 */
typedef enum {
    UNIFIED_FRAME_TYPE_CAN_DATA = 0x01,
    UNIFIED_FRAME_TYPE_STATUS = 0x02,
    UNIFIED_FRAME_TYPE_HEARTBEAT = 0x03,
    UNIFIED_FRAME_TYPE_ERROR = 0x04,
} unified_frame_type_t;

/**
 * @brief 内部统一帧中的 CAN 标志位。
 */
typedef enum {
    UNIFIED_CAN_FLAG_NONE = 0x00,
    UNIFIED_CAN_FLAG_EXTENDED_ID = (uint8_t)(1u << 0),
    UNIFIED_CAN_FLAG_FD = (uint8_t)(1u << 1),
    UNIFIED_CAN_FLAG_RTR = (uint8_t)(1u << 2),
    UNIFIED_CAN_FLAG_BRS = (uint8_t)(1u << 3),
} unified_can_flag_t;

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_TYPE_H */
