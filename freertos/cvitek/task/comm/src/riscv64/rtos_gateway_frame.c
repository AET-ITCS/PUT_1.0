/* FreeRTOS comm 帧校验实现：只依赖公共统一帧 ABI 和 CRC16。 */
#include "rtos_gateway_frame.h"

#include <string.h>

#include "crc16.h"

#define RTOS_CAN_STANDARD_ID_MAX 0x7FFu
#define RTOS_CAN_EXTENDED_ID_MAX 0x1FFFFFFFu
#define RTOS_CAN_V1_SUPPORTED_FLAGS ((uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID)

static bool protocol_type_is_valid(uint8_t type)
{
    return (type == (uint8_t)PROTOCOL_TYPE_UNKNOWN) ||
           (type == (uint8_t)PROTOCOL_TYPE_4G) ||
           (type == (uint8_t)PROTOCOL_TYPE_WIFI) ||
           (type == (uint8_t)PROTOCOL_TYPE_BLUETOOTH) ||
           (type == (uint8_t)PROTOCOL_TYPE_ETHERNET) ||
           (type == (uint8_t)PROTOCOL_TYPE_RS485) ||
           (type == (uint8_t)PROTOCOL_TYPE_CLOUD_MQTT);
}

static bool frame_type_is_supported(uint8_t type)
{
    return (type == (uint8_t)UNIFIED_FRAME_TYPE_CAN_DATA) ||
           (type == (uint8_t)UNIFIED_FRAME_TYPE_STATUS) ||
           (type == (uint8_t)UNIFIED_FRAME_TYPE_HEARTBEAT);
}

static bool can_id_is_valid(uint32_t can_id, uint8_t can_flags)
{
    if ((can_flags & (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID) != 0u) {
        return can_id <= RTOS_CAN_EXTENDED_ID_MAX;
    }

    return can_id <= RTOS_CAN_STANDARD_ID_MAX;
}

static void fill_can_message(const unified_frame_t *frame, rtos_can_message_t *out_msg)
{
    if (out_msg == NULL) {
        return;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->can_id = frame->can_id;
    out_msg->can_dlc = frame->can_dlc;
    out_msg->can_flags = frame->can_flags;
    out_msg->sequence = frame->sequence;
    out_msg->timestamp_ms = frame->timestamp_ms;
    memcpy(out_msg->can_data, frame->can_data, UNIFIED_CAN_CLASSIC_DATA_MAX_LEN);
}

rtos_frame_validate_error_t rtos_gateway_frame_validate(const unified_frame_t *frame,
                                                        rtos_can_message_t *out_msg)
{
    uint16_t actual_crc;

    if (frame == NULL) {
        return RTOS_FRAME_VALIDATE_NULL;
    }

    if (frame->magic != UNIFIED_FRAME_MAGIC) {
        return RTOS_FRAME_VALIDATE_MAGIC;
    }

    if (frame->version != UNIFIED_FRAME_VERSION) {
        return RTOS_FRAME_VALIDATE_VERSION;
    }

    if (!frame_type_is_supported(frame->frame_type)) {
        return RTOS_FRAME_VALIDATE_TYPE;
    }

    if (!protocol_type_is_valid(frame->source_protocol)) {
        return RTOS_FRAME_VALIDATE_SOURCE_PROTOCOL;
    }

    if (!vehicle_msg_type_is_valid(frame->vehicle_type)) {
        return RTOS_FRAME_VALIDATE_VEHICLE_TYPE;
    }

    if ((frame->can_flags & (uint8_t)~RTOS_CAN_V1_SUPPORTED_FLAGS) != 0u) {
        return RTOS_FRAME_VALIDATE_FLAG;
    }

    if (!can_id_is_valid(frame->can_id, frame->can_flags)) {
        return RTOS_FRAME_VALIDATE_CAN_ID;
    }

    if (!unified_frame_can_dlc_is_valid(frame->can_dlc, frame->can_flags) ||
        (frame->can_dlc > UNIFIED_CAN_CLASSIC_DATA_MAX_LEN)) {
        return RTOS_FRAME_VALIDATE_DLC;
    }

    actual_crc = unified_crc16_ccitt_false((const uint8_t *)frame,
                                           UNIFIED_FRAME_CRC_INPUT_LENGTH);
    if (actual_crc != frame->crc16) {
        return RTOS_FRAME_VALIDATE_CRC;
    }

    fill_can_message(frame, out_msg);
    return RTOS_FRAME_VALIDATE_OK;
}
