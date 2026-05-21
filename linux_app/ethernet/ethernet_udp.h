/* 以太网 UDP CAN direct 网关接口：从 UDP payload 提取 CAN 字段。 */
#ifndef ETHERNET_UDP_H
#define ETHERNET_UDP_H

#include <stddef.h>
#include <stdint.h>

#include "can_direct_frame.h"
#include "error_code.h"
#include "protocol_parsed_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ETHERNET_UDP_FRAME_MAGIC CAN_DIRECT_FRAME_MAGIC
#define ETHERNET_UDP_FRAME_VERSION CAN_DIRECT_FRAME_VERSION
#define ETHERNET_UDP_FRAME_HEADER_LENGTH CAN_DIRECT_FRAME_HEADER_LENGTH
#define ETHERNET_UDP_FRAME_CRC_LENGTH CAN_DIRECT_FRAME_CRC_LENGTH
#define ETHERNET_UDP_FRAME_LENGTH CAN_DIRECT_FRAME_LENGTH

/**
 * @brief 解析以太网 UDP CAN direct 网关帧。
 *
 * 固定 76 字节，小端字段：
 * magic(2), version(1), reserved(1), can_flags(1), can_dlc(1),
 * can_id(4), can_data(64), crc16(2)。
 */
unified_error_t ethernet_udp_parse_frame(const uint8_t *buffer,
                                         size_t length,
                                         protocol_parsed_msg_t *out_msg);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_UDP_H */
