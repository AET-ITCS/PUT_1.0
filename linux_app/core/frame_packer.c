/* 统一帧打包实现：校验协议中间消息并生成 unified_frame_t、序号、时间戳和 CRC。 */
#define _POSIX_C_SOURCE 200809L

#include "frame_packer.h"

#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "crc16.h"

static uint32_t g_next_sequence = 1u;

static bool protocol_type_is_valid(uint8_t type)
{
    return (type == (uint8_t)PROTOCOL_TYPE_4G) ||
           (type == (uint8_t)PROTOCOL_TYPE_WIFI) ||
           (type == (uint8_t)PROTOCOL_TYPE_BLUETOOTH) ||
           (type == (uint8_t)PROTOCOL_TYPE_ETHERNET) ||
           (type == (uint8_t)PROTOCOL_TYPE_RS485) ||
           (type == (uint8_t)PROTOCOL_TYPE_CLOUD_MQTT);
}

static bool can_id_is_valid(uint32_t can_id, uint8_t can_flags)
{
    if ((can_flags & (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID) != 0u) {
        return can_id <= 0x1FFFFFFFu;
    }

    return can_id <= 0x7FFu;
}

static uint32_t current_timestamp_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }

    return (uint32_t)(((uint64_t)ts.tv_sec * 1000ull) + ((uint64_t)ts.tv_nsec / 1000000ull));
}

void frame_packer_init(uint32_t initial_sequence)
{
    g_next_sequence = (initial_sequence == 0u) ? 1u : initial_sequence;
}

uint16_t frame_packer_calculate_crc(const unified_frame_t *frame)
{
    if (frame == NULL) {
        return 0u;
    }

    return unified_crc16_ccitt_false((const uint8_t *)frame, UNIFIED_FRAME_CRC_INPUT_LENGTH);
}

unified_error_t frame_packer_pack(const protocol_parsed_msg_t *msg, unified_frame_t *out_frame)
{
    if ((msg == NULL) || (out_frame == NULL)) {
        return UNIFIED_ERR_NULL;
    }

    if (!protocol_type_is_valid((uint8_t)msg->source_protocol) ||
        !vehicle_msg_type_is_valid(msg->vehicle_type)) {
        return UNIFIED_ERR_UNKNOWN_TYPE;
    }

    if (!unified_frame_can_dlc_is_valid(msg->can_dlc, msg->can_flags)) {
        return UNIFIED_ERR_CAN_DLC;
    }

    if (!can_id_is_valid(msg->can_id, msg->can_flags)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->magic = UNIFIED_FRAME_MAGIC;
    out_frame->version = UNIFIED_FRAME_VERSION;
    out_frame->frame_type = (uint8_t)UNIFIED_FRAME_TYPE_CAN_DATA;
    out_frame->source_protocol = (uint8_t)msg->source_protocol;
    out_frame->vehicle_type = msg->vehicle_type;
    out_frame->can_dlc = msg->can_dlc;
    out_frame->can_flags = msg->can_flags;
    out_frame->sequence = g_next_sequence++;
    out_frame->timestamp_ms = current_timestamp_ms();
    out_frame->source_id = msg->source_id;
    out_frame->destination_id = msg->destination_id;
    out_frame->can_id = msg->can_id;
    memcpy(out_frame->can_data, msg->can_data, sizeof(out_frame->can_data));
    out_frame->crc16 = frame_packer_calculate_crc(out_frame);

    return UNIFIED_OK;
}
