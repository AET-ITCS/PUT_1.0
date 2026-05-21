/* CAN direct 网关帧解析接口：外部通道只承载 CAN 字段，不解释应用语义。 */
#ifndef CAN_DIRECT_FRAME_H
#define CAN_DIRECT_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "error_code.h"
#include "protocol_parsed_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_DIRECT_FRAME_MAGIC 0x55AAu
#define CAN_DIRECT_FRAME_VERSION 0x01u
#define CAN_DIRECT_FRAME_HEADER_LENGTH 10u
#define CAN_DIRECT_FRAME_CRC_LENGTH 2u
#define CAN_DIRECT_FRAME_LENGTH \
    (CAN_DIRECT_FRAME_HEADER_LENGTH + UNIFIED_CAN_FD_DATA_MAX_LEN + CAN_DIRECT_FRAME_CRC_LENGTH)

/**
 * @brief 解析纯 CAN 网关帧。
 *
 * 固定 76 字节，小端字段：
 * magic(2), version(1), reserved(1), can_flags(1), can_dlc(1),
 * can_id(4), can_data(64), crc16(2)。
 */
unified_error_t can_direct_parse_frame(const uint8_t *buffer,
                                       size_t length,
                                       protocol_type_t source_protocol,
                                       protocol_parsed_msg_t *out_msg);

#ifdef __cplusplus
}
#endif

#endif /* CAN_DIRECT_FRAME_H */
