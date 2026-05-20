/* 大小核统一帧定义：描述大核 Linux 发送给小核 RTOS 的内部转发帧格式。 */
#ifndef UNIFIED_FRAME_H
#define UNIFIED_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol_type.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define UNIFIED_PACKED __attribute__((packed))
#else
#define UNIFIED_PACKED
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define UNIFIED_STATIC_ASSERT(condition, message) _Static_assert((condition), message)
#else
#define UNIFIED_CONCAT_INNER(a, b) a##b
#define UNIFIED_CONCAT(a, b) UNIFIED_CONCAT_INNER(a, b)
#define UNIFIED_STATIC_ASSERT(condition, message) \
    typedef char UNIFIED_CONCAT(unified_static_assert_, __LINE__)[(condition) ? 1 : -1]
#endif

/**
 * @brief 项目内部统一转发帧常量。
 *
 * 注意：该帧是项目自定义的“大核 Linux -> 小核 RTOS”内部协议，
 * 不是 CAN 官方帧格式。小核收到该结构后再组装官方 CAN/CAN FD 报文。
 */
#define UNIFIED_FRAME_MAGIC 0xA55Au
#define UNIFIED_FRAME_VERSION 0x01u
#define UNIFIED_FRAME_LENGTH 96u
#define UNIFIED_CAN_CLASSIC_DATA_MAX_LEN 8u
#define UNIFIED_CAN_FD_DATA_MAX_LEN 64u
#define UNIFIED_FRAME_RESERVED_LENGTH 2u
#define UNIFIED_FRAME_CRC_LENGTH 2u
#define UNIFIED_FRAME_CRC_INPUT_LENGTH (UNIFIED_FRAME_LENGTH - UNIFIED_FRAME_CRC_LENGTH)

/**
 * @brief 大核 Linux 到小核 RTOS 的项目内部统一转发帧。
 *
 * can_dlc 在本项目内部表示 CAN 数据字节数：
 * - 普通 CAN：0 ~ 8；
 * - CAN FD：0 ~ 64，需同时设置 UNIFIED_CAN_FLAG_FD。
 */
typedef struct UNIFIED_PACKED {
    uint16_t magic;             /**< 固定为 UNIFIED_FRAME_MAGIC */
    uint8_t version;            /**< 固定为 UNIFIED_FRAME_VERSION */
    uint8_t frame_type;         /**< unified_frame_type_t */
    uint8_t source_protocol;    /**< protocol_type_t */
    uint8_t vehicle_type;       /**< vehicle_msg_type_t */
    uint8_t can_dlc;            /**< CAN 数据字节数 */
    uint8_t can_flags;          /**< unified_can_flag_t bitmask */
    uint32_t sequence;          /**< 帧序号 */
    uint32_t timestamp_ms;      /**< 时间戳，单位 ms */
    uint32_t source_id;         /**< 逻辑来源 ID，由大核协议适配层生成 */
    uint32_t destination_id;    /**< 逻辑目的 ID，由大核协议适配层生成 */
    uint32_t can_id;            /**< CAN 标准 ID 或扩展 ID */
    uint8_t reserved[UNIFIED_FRAME_RESERVED_LENGTH]; /**< 保留字段，填 0 */
    uint8_t can_data[UNIFIED_CAN_FD_DATA_MAX_LEN];   /**< CAN/CAN FD 数据 */
    uint16_t crc16;             /**< CRC-16/CCITT-FALSE，覆盖本字段之前的所有字节 */
} unified_frame_t;

UNIFIED_STATIC_ASSERT(sizeof(unified_frame_t) == UNIFIED_FRAME_LENGTH,
                      "unified_frame_t must be 96 bytes");

static inline bool vehicle_msg_type_is_valid(uint8_t type)
{
    return (type == (uint8_t)VEHICLE_MSG_TYPE_SEAT_CONTROL) ||
           (type == (uint8_t)VEHICLE_MSG_TYPE_SEAT_FEEDBACK) ||
           (type == (uint8_t)VEHICLE_MSG_TYPE_LIGHT_CONTROL) ||
           (type == (uint8_t)VEHICLE_MSG_TYPE_LIGHT_FEEDBACK) ||
           (type == (uint8_t)VEHICLE_MSG_TYPE_WINDOW_CONTROL) ||
           (type == (uint8_t)VEHICLE_MSG_TYPE_WINDOW_FEEDBACK);
}

static inline bool unified_frame_can_dlc_is_valid(uint8_t can_dlc, uint8_t can_flags)
{
    if ((can_flags & (uint8_t)UNIFIED_CAN_FLAG_FD) != 0u) {
        return can_dlc <= UNIFIED_CAN_FD_DATA_MAX_LEN;
    }

    return can_dlc <= UNIFIED_CAN_CLASSIC_DATA_MAX_LEN;
}

#ifdef __cplusplus
}
#endif

#endif /* UNIFIED_FRAME_H */
