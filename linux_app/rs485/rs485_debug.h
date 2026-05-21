/* RS485 CAN direct 网关帧解析接口：保留旧文件名作为兼容入口。 */
#ifndef RS485_DEBUG_H
#define RS485_DEBUG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "can_direct_frame.h"
#include "error_code.h"
#include "protocol_parsed_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RS485_DEBUG_FRAME_MAGIC CAN_DIRECT_FRAME_MAGIC
#define RS485_DEBUG_FRAME_VERSION CAN_DIRECT_FRAME_VERSION
#define RS485_DEBUG_FRAME_HEADER_LENGTH CAN_DIRECT_FRAME_HEADER_LENGTH
#define RS485_DEBUG_FRAME_CRC_LENGTH CAN_DIRECT_FRAME_CRC_LENGTH
#define RS485_DEBUG_FRAME_LENGTH CAN_DIRECT_FRAME_LENGTH

typedef struct {
    uint8_t frame[RS485_DEBUG_FRAME_LENGTH];
    size_t pos;
} rs485_debug_sync_t;

void rs485_debug_sync_init(rs485_debug_sync_t *sync);
bool rs485_debug_sync_feed(rs485_debug_sync_t *sync,
                           uint8_t byte,
                           uint8_t out_frame[RS485_DEBUG_FRAME_LENGTH]);
unified_error_t rs485_debug_parse_frame(const uint8_t *buffer,
                                        size_t length,
                                        protocol_parsed_msg_t *out_msg);

#ifdef __cplusplus
}
#endif

#endif /* RS485_DEBUG_H */
