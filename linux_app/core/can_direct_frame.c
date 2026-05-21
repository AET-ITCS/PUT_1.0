/* CAN direct 网关帧字段提取实现：校验帧头/CRC 后提取 CAN 转发字段。 */
#include "can_direct_frame.h"

#include <string.h>

#include "crc16.h"
#include "unified_frame.h"

#define OFFSET_MAGIC 0u
#define OFFSET_VERSION 2u
#define OFFSET_RESERVED 3u
#define OFFSET_CAN_FLAGS 4u
#define OFFSET_CAN_DLC 5u
#define OFFSET_CAN_ID 6u
#define OFFSET_CAN_DATA 10u
#define OFFSET_CRC (CAN_DIRECT_FRAME_LENGTH - CAN_DIRECT_FRAME_CRC_LENGTH)

#define CAN_DIRECT_KNOWN_FLAGS \
    ((uint8_t)((uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID | \
               (uint8_t)UNIFIED_CAN_FLAG_FD | \
               (uint8_t)UNIFIED_CAN_FLAG_RTR | \
               (uint8_t)UNIFIED_CAN_FLAG_BRS))

static uint16_t load_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

static uint32_t load_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8u) |
           ((uint32_t)data[2] << 16u) |
           ((uint32_t)data[3] << 24u);
}

unified_error_t can_direct_parse_frame(const uint8_t *buffer,
                                       size_t length,
                                       protocol_type_t source_protocol,
                                       protocol_parsed_msg_t *out_msg)
{
    uint16_t expected_crc;
    uint16_t actual_crc;
    uint8_t can_flags;
    uint8_t can_dlc;
    uint32_t can_id;

    if ((buffer == NULL) || (out_msg == NULL)) {
        return UNIFIED_ERR_NULL;
    }

    if (length != CAN_DIRECT_FRAME_LENGTH) {
        return UNIFIED_ERR_LENGTH;
    }

    if (load_u16_le(&buffer[OFFSET_MAGIC]) != CAN_DIRECT_FRAME_MAGIC) {
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if ((buffer[OFFSET_VERSION] != CAN_DIRECT_FRAME_VERSION) || (buffer[OFFSET_RESERVED] != 0u)) {
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    expected_crc = load_u16_le(&buffer[OFFSET_CRC]);
    actual_crc = unified_crc16_ccitt_false(buffer, OFFSET_CRC);
    if (expected_crc != actual_crc) {
        return UNIFIED_ERR_CRC;
    }

    can_flags = buffer[OFFSET_CAN_FLAGS];
    can_dlc = buffer[OFFSET_CAN_DLC];
    can_id = load_u32_le(&buffer[OFFSET_CAN_ID]);

    if ((can_flags & (uint8_t)~CAN_DIRECT_KNOWN_FLAGS) != 0u) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (!unified_frame_can_dlc_is_valid(can_dlc, can_flags)) {
        return UNIFIED_ERR_CAN_DLC;
    }

    if (((can_flags & (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID) == 0u) && (can_id > 0x7FFu)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (((can_flags & (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID) != 0u) && (can_id > 0x1FFFFFFFu)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->source_protocol = source_protocol;
    out_msg->vehicle_type = (uint8_t)VEHICLE_MSG_TYPE_RAW_CAN;
    out_msg->can_flags = can_flags;
    out_msg->can_dlc = can_dlc;
    out_msg->can_id = can_id;
    memcpy(out_msg->can_data, &buffer[OFFSET_CAN_DATA], UNIFIED_CAN_FD_DATA_MAX_LEN);

    return UNIFIED_OK;
}
