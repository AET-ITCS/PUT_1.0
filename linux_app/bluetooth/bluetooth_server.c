/* 蓝牙服务协议解析与低级网络接口实现。 */
#define _POSIX_C_SOURCE 200809L

#include "bluetooth_server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "crc16.h"

#define OFFSET_MAGIC 0u
#define OFFSET_VERSION 2u
#define OFFSET_VEHICLE_TYPE 3u
#define OFFSET_CAN_FLAGS 4u
#define OFFSET_CAN_DLC 5u
#define OFFSET_CAN_ID 6u
#define OFFSET_CAN_DATA 10u
#define OFFSET_CRC (BLUETOOTH_FRAME_LENGTH - BLUETOOTH_FRAME_CRC_LENGTH)

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

void format_bdaddr(const bdaddr_t *ba, char *out_str, size_t max_len)
{
    if ((ba == NULL) || (out_str == NULL) || (max_len < 18u)) {
        return;
    }
    (void)snprintf(out_str, max_len, "%02X:%02X:%02X:%02X:%02X:%02X",
                   ba->b[5], ba->b[4], ba->b[3], ba->b[2], ba->b[1], ba->b[0]);
}

ssize_t recv_all(int fd, uint8_t *buf, size_t len)
{
    size_t total_received = 0u;
    while (total_received < len) {
        ssize_t received = recv(fd, buf + total_received, len - total_received, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1; // 发生读取错误
        }
        if (received == 0) {
            return (ssize_t)total_received; // 远端关闭，返回已读到的字节数
        }
        total_received += (size_t)received;
    }
    return (ssize_t)total_received;
}

unified_error_t bluetooth_parse_frame(const uint8_t *buffer,
                                      size_t length,
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

    if (length != BLUETOOTH_FRAME_LENGTH) {
        return UNIFIED_ERR_LENGTH;
    }

    if (load_u16_le(&buffer[OFFSET_MAGIC]) != BLUETOOTH_FRAME_MAGIC) {
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if (buffer[OFFSET_VERSION] != BLUETOOTH_FRAME_VERSION) {
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

    if (!vehicle_msg_type_is_valid(buffer[OFFSET_VEHICLE_TYPE])) {
        return UNIFIED_ERR_UNKNOWN_TYPE;
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
    out_msg->source_protocol = PROTOCOL_TYPE_BLUETOOTH;
    out_msg->vehicle_type = buffer[OFFSET_VEHICLE_TYPE];
    out_msg->can_flags = can_flags;
    out_msg->can_dlc = can_dlc;
    out_msg->can_id = can_id;
    memcpy(out_msg->can_data, &buffer[OFFSET_CAN_DATA], UNIFIED_CAN_FD_DATA_MAX_LEN);

    return UNIFIED_OK;
}
