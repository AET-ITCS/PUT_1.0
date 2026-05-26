/**
 * @file anymsg_frame.h
 * @brief anyMSG 统一数据帧公共定义和基础校验 helper。
   @author B
 *
 * 本文件严格描述 docs/设计文档/统一数据帧设计.md 中的 40B 固定帧头、
 * CID 地址段、type 类型和基础合法性约束。多字节字段以 raw bytes 表达，
 * common 层不假设 little-endian 或 big-endian。
 */
#ifndef ANYMSG_FRAME_H
#define ANYMSG_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ANYMSG_PACKED __attribute__((packed))
#else
#define ANYMSG_PACKED
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define ANYMSG_STATIC_ASSERT(condition, message) _Static_assert((condition), message)
#else
#define ANYMSG_CONCAT_INNER(a, b) a##b
#define ANYMSG_CONCAT(a, b) ANYMSG_CONCAT_INNER(a, b)
#define ANYMSG_STATIC_ASSERT(condition, message) \
    typedef char ANYMSG_CONCAT(anymsg_static_assert_, __LINE__)[(condition) ? 1 : -1]
#endif

/** @brief anyMSG 固定帧头长度。 */
#define ANYMSG_HEADER_SIZE 40u
/** @brief anyMSG 固定帧头长度别名。 */
#define ANYMSG_HEADER_LENGTH ANYMSG_HEADER_SIZE
/** @brief verify_string 字段长度。 */
#define ANYMSG_VERIFY_STRING_LENGTH 16u
/** @brief CID 字段长度。 */
#define ANYMSG_CID_LENGTH 4u
/** @brief anyMSG msg_length/payload_length 字段长度。 */
#define ANYMSG_LENGTH_FIELD_LENGTH 2u
/** @brief anyMSG 16-bit msg_length 字段可表达的最大完整帧长度。 */
#define ANYMSG_MAX_MSG_LENGTH 0xFFFFu
/** @brief anyMSG 16-bit payload_length 字段可表达的最大 payload 长度。 */
#define ANYMSG_MAX_PAYLOAD_LENGTH (ANYMSG_MAX_MSG_LENGTH - ANYMSG_HEADER_SIZE)

/** @brief anyMSG 字段 offset。 */
#define ANYMSG_OFFSET_MSG_LENGTH 0u
#define ANYMSG_OFFSET_RETRIES 2u
#define ANYMSG_OFFSET_RESERVED 3u
#define ANYMSG_OFFSET_SRCHLD 4u
#define ANYMSG_OFFSET_DESTINATION_CID 8u
#define ANYMSG_OFFSET_SOURCE_CID 12u
#define ANYMSG_OFFSET_LOCAL_TIME 16u
#define ANYMSG_OFFSET_VERIFY_STRING 20u
#define ANYMSG_OFFSET_PAYLOAD_LENGTH 36u
#define ANYMSG_OFFSET_TYPE 38u
#define ANYMSG_OFFSET_PADDING 39u
#define ANYMSG_OFFSET_PAYLOAD 40u

/** @brief anyMSG 字段 length。 */
#define ANYMSG_FIELD_MSG_LENGTH_SIZE 2u
#define ANYMSG_FIELD_RETRIES_SIZE 1u
#define ANYMSG_FIELD_RESERVED_SIZE 1u
#define ANYMSG_FIELD_SRCHLD_SIZE 4u
#define ANYMSG_FIELD_DESTINATION_CID_SIZE 4u
#define ANYMSG_FIELD_SOURCE_CID_SIZE 4u
#define ANYMSG_FIELD_LOCAL_TIME_SIZE 4u
#define ANYMSG_FIELD_VERIFY_STRING_SIZE 16u
#define ANYMSG_FIELD_PAYLOAD_LENGTH_SIZE 2u
#define ANYMSG_FIELD_TYPE_SIZE 1u
#define ANYMSG_FIELD_PADDING_SIZE 1u

/** @brief CID 地址首字节范围。 */
#define ANYMSG_CID_RESERVED_LOW_MIN 0x00u
#define ANYMSG_CID_RESERVED_LOW_MAX 0x1Fu
#define ANYMSG_CID_CAN_MIN 0x20u
#define ANYMSG_CID_CAN_MAX 0x3Fu
#define ANYMSG_CID_ETHERNET_MIN 0x40u
#define ANYMSG_CID_ETHERNET_MAX 0x5Fu
#define ANYMSG_CID_WIFI_MIN 0x60u
#define ANYMSG_CID_WIFI_MAX 0x7Fu
#define ANYMSG_CID_BLUETOOTH_MIN 0x80u
#define ANYMSG_CID_BLUETOOTH_MAX 0x9Fu
#define ANYMSG_CID_4G_MIN 0xA0u
#define ANYMSG_CID_4G_MAX 0xBFu
#define ANYMSG_CID_RS485_MIN 0xC0u
#define ANYMSG_CID_RS485_MAX 0xDFu
#define ANYMSG_CID_RESERVED_HIGH_MIN 0xE0u
#define ANYMSG_CID_RESERVED_HIGH_MAX 0xFFu

/** @brief anyMSG type 类型值和范围。 */
#define ANYMSG_TYPE_ENDPOINT_TO_GATEWAY_HEARTBEAT 0x00u
#define ANYMSG_TYPE_GATEWAY_TO_ENDPOINT_HEARTBEAT 0x01u
#define ANYMSG_TYPE_ENDPOINT_TO_GATEWAY_HEALTH 0x02u
#define ANYMSG_TYPE_GATEWAY_TO_ENDPOINT_HEALTH 0x03u
#define ANYMSG_TYPE_NETWORK_AUTH_MIN 0x04u
#define ANYMSG_TYPE_NETWORK_AUTH_MAX 0x0Au
#define ANYMSG_TYPE_SERVICE_INDEX_MIN 0x0Du
#define ANYMSG_TYPE_SERVICE_INDEX_MAX 0x12u
#define ANYMSG_TYPE_TUNNEL_PORT_MAPPING_MIN 0x15u
#define ANYMSG_TYPE_TUNNEL_PORT_MAPPING_MAX 0x1Eu
#define ANYMSG_TYPE_RESERVED_MIDDLE_MIN 0x1Fu
#define ANYMSG_TYPE_RESERVED_MIDDLE_MAX 0x46u
#define ANYMSG_TYPE_MODBUS_RTU 0x47u
#define ANYMSG_TYPE_MODBUS_TCP 0x48u
#define ANYMSG_TYPE_RAW_CAN 0x49u
#define ANYMSG_TYPE_CAN_FD 0x4Au
#define ANYMSG_TYPE_CAN_ISOTP 0x4Bu
#define ANYMSG_TYPE_UDS 0x4Cu
#define ANYMSG_TYPE_J1939 0x4Du
#define ANYMSG_TYPE_CANOPEN 0x4Eu
#define ANYMSG_TYPE_RESERVED_HIGH_MIN 0x4Fu
#define ANYMSG_TYPE_RESERVED_HIGH_MAX 0xFFu

/**
 * @brief anyMSG 40B 固定帧头。
 *
 * 多字节字段使用 raw byte 数组表示，字节序由具体通信接口统一约定。
 */
typedef struct ANYMSG_PACKED {
    uint8_t msg_length[ANYMSG_FIELD_MSG_LENGTH_SIZE];       /**< 完整帧长度 raw bytes。 */
    uint8_t retries;                                        /**< 重试次数。 */
    uint8_t reserved;                                       /**< __RESERVED__，当前填 0。 */
    uint8_t srchld[ANYMSG_FIELD_SRCHLD_SIZE];               /**< __SRCHLD__，当前填 0。 */
    uint8_t destination_cid[ANYMSG_FIELD_DESTINATION_CID_SIZE]; /**< 目的通信地址 raw bytes。 */
    uint8_t source_cid[ANYMSG_FIELD_SOURCE_CID_SIZE];       /**< 源通信地址 raw bytes。 */
    uint8_t local_time[ANYMSG_FIELD_LOCAL_TIME_SIZE];       /**< 本地时间戳 raw bytes。 */
    uint8_t verify_string[ANYMSG_FIELD_VERIFY_STRING_SIZE]; /**< 校验码 raw bytes，算法未定义。 */
    uint8_t payload_length[ANYMSG_FIELD_PAYLOAD_LENGTH_SIZE]; /**< payload 长度 raw bytes。 */
    uint8_t type;                                           /**< payload 类型。 */
    uint8_t padding;                                        /**< __PADDING__，当前填 0。 */
} anymsg_header_t;

/** @brief CID 地址段分类。 */
typedef enum {
    ANYMSG_CID_SEGMENT_RESERVED_LOW = 0,
    ANYMSG_CID_SEGMENT_CAN,
    ANYMSG_CID_SEGMENT_ETHERNET,
    ANYMSG_CID_SEGMENT_WIFI,
    ANYMSG_CID_SEGMENT_BLUETOOTH,
    ANYMSG_CID_SEGMENT_4G,
    ANYMSG_CID_SEGMENT_RS485,
    ANYMSG_CID_SEGMENT_RESERVED_HIGH,
} anymsg_cid_segment_t;

ANYMSG_STATIC_ASSERT(sizeof(anymsg_header_t) == ANYMSG_HEADER_SIZE,
                     "anymsg_header_t must be 40 bytes");
ANYMSG_STATIC_ASSERT(ANYMSG_OFFSET_PAYLOAD == ANYMSG_HEADER_SIZE,
                     "payload offset must equal anyMSG header size");

/**
 * @brief 获取 payload 起始指针。
 */
static inline uint8_t *anymsg_payload(void *frame)
{
    if (frame == 0) {
        return 0;
    }

    return ((uint8_t *)frame) + ANYMSG_OFFSET_PAYLOAD;
}

/**
 * @brief 获取只读 payload 起始指针。
 */
static inline const uint8_t *anymsg_payload_const(const void *frame)
{
    if (frame == 0) {
        return 0;
    }

    return ((const uint8_t *)frame) + ANYMSG_OFFSET_PAYLOAD;
}

/**
 * @brief 获取目的 CID 的 4B raw bytes。
 */
static inline const uint8_t *anymsg_destination_cid_raw(const anymsg_header_t *header)
{
    if (header == 0) {
        return 0;
    }

    return header->destination_cid;
}

/**
 * @brief 获取源 CID 的 4B raw bytes。
 */
static inline const uint8_t *anymsg_source_cid_raw(const anymsg_header_t *header)
{
    if (header == 0) {
        return 0;
    }

    return header->source_cid;
}

/**
 * @brief 获取目的 CID 地址首字节。
 */
static inline uint8_t anymsg_destination_cid_first_byte(const anymsg_header_t *header)
{
    if (header == 0) {
        return 0u;
    }

    return header->destination_cid[0];
}

/**
 * @brief 获取源 CID 地址首字节。
 */
static inline uint8_t anymsg_source_cid_first_byte(const anymsg_header_t *header)
{
    if (header == 0) {
        return 0u;
    }

    return header->source_cid[0];
}

/**
 * @brief 获取 anyMSG type。
 */
static inline uint8_t anymsg_type(const anymsg_header_t *header)
{
    if (header == 0) {
        return 0u;
    }

    return header->type;
}

/**
 * @brief 根据 CID 地址首字节解析地址段。
 */
static inline anymsg_cid_segment_t anymsg_cid_segment_from_first_byte(uint8_t first_byte)
{
    if (first_byte <= ANYMSG_CID_RESERVED_LOW_MAX) {
        return ANYMSG_CID_SEGMENT_RESERVED_LOW;
    }
    if (first_byte <= ANYMSG_CID_CAN_MAX) {
        return ANYMSG_CID_SEGMENT_CAN;
    }
    if (first_byte <= ANYMSG_CID_ETHERNET_MAX) {
        return ANYMSG_CID_SEGMENT_ETHERNET;
    }
    if (first_byte <= ANYMSG_CID_WIFI_MAX) {
        return ANYMSG_CID_SEGMENT_WIFI;
    }
    if (first_byte <= ANYMSG_CID_BLUETOOTH_MAX) {
        return ANYMSG_CID_SEGMENT_BLUETOOTH;
    }
    if (first_byte <= ANYMSG_CID_4G_MAX) {
        return ANYMSG_CID_SEGMENT_4G;
    }
    if (first_byte <= ANYMSG_CID_RS485_MAX) {
        return ANYMSG_CID_SEGMENT_RS485;
    }

    return ANYMSG_CID_SEGMENT_RESERVED_HIGH;
}

/**
 * @brief 检查 CID 地址首字节是否落在文档定义的地址段或保留段。
 */
static inline bool anymsg_cid_first_byte_is_documented(uint8_t first_byte)
{
    switch (anymsg_cid_segment_from_first_byte(first_byte)) {
    case ANYMSG_CID_SEGMENT_RESERVED_LOW:
    case ANYMSG_CID_SEGMENT_CAN:
    case ANYMSG_CID_SEGMENT_ETHERNET:
    case ANYMSG_CID_SEGMENT_WIFI:
    case ANYMSG_CID_SEGMENT_BLUETOOTH:
    case ANYMSG_CID_SEGMENT_4G:
    case ANYMSG_CID_SEGMENT_RS485:
    case ANYMSG_CID_SEGMENT_RESERVED_HIGH:
        return true;
    default:
        return false;
    }
}

/**
 * @brief 检查 anyMSG 当前约定为 0 的保留字段。
 */
static inline bool anymsg_reserved_fields_are_zero(const anymsg_header_t *header)
{
    if (header == 0) {
        return false;
    }

    if ((header->reserved != 0u) || (header->padding != 0u)) {
        return false;
    }

    for (size_t i = 0u; i < ANYMSG_FIELD_SRCHLD_SIZE; ++i) {
        if (header->srchld[i] != 0u) {
            return false;
        }
    }

    return true;
}

/**
 * @brief 校验已按接口约定归一化后的长度字段。
 *
 * common 层不解释 msg_length/payload_length raw bytes 的字节序。
 */
static inline unified_error_t anymsg_validate_normalized_lengths(uint16_t msg_length,
                                                                 uint16_t payload_length,
                                                                 size_t actual_length)
{
    if (msg_length < (uint16_t)ANYMSG_HEADER_SIZE) {
        return UNIFIED_ERR_LENGTH;
    }

    if ((uint32_t)msg_length != ((uint32_t)ANYMSG_HEADER_SIZE + (uint32_t)payload_length)) {
        return UNIFIED_ERR_PAYLOAD_LENGTH;
    }

    if (actual_length != (size_t)msg_length) {
        return UNIFIED_ERR_LENGTH;
    }

    return UNIFIED_OK;
}

/**
 * @brief 校验 anyMSG header 中不依赖字节序的基础字段。
 */
static inline unified_error_t anymsg_validate_header_static_fields(const anymsg_header_t *header)
{
    if (header == 0) {
        return UNIFIED_ERR_NULL;
    }

    if (!anymsg_reserved_fields_are_zero(header)) {
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if (!anymsg_cid_first_byte_is_documented(anymsg_destination_cid_first_byte(header)) ||
        !anymsg_cid_first_byte_is_documented(anymsg_source_cid_first_byte(header))) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* ANYMSG_FRAME_H */
