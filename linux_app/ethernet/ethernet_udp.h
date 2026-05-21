/* 以太网 UDP 简单二进制协议解析接口：把 UDP 原始帧转换为协议中间消息。 */
#ifndef ETHERNET_UDP_H
#define ETHERNET_UDP_H

#include <stddef.h>
#include <stdint.h>

#include "error_code.h"
#include "protocol_parsed_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ETHERNET_UDP_FRAME_MAGIC 0x55AAu
#define ETHERNET_UDP_FRAME_VERSION 0x01u
#define ETHERNET_UDP_FRAME_HEADER_LENGTH 10u
#define ETHERNET_UDP_FRAME_CRC_LENGTH 2u
#define ETHERNET_UDP_FRAME_LENGTH \
    (ETHERNET_UDP_FRAME_HEADER_LENGTH + UNIFIED_CAN_FD_DATA_MAX_LEN + ETHERNET_UDP_FRAME_CRC_LENGTH)

/**
 * @brief 解析第一版以太网 UDP 简单二进制帧。
 *
 * 固定 76 字节，小端字段：
 * magic(2), version(1), vehicle_type(1), can_flags(1), can_dlc(1),
 * can_id(4), can_data(64), crc16(2)。
 */
unified_error_t ethernet_udp_parse_frame(const uint8_t *buffer,
                                         size_t length,
                                         protocol_parsed_msg_t *out_msg);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_UDP_H */
