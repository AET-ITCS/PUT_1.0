/* RS485 调试帧解析接口：复用 76 字节调试帧并转换为协议中间消息。 */
#ifndef RS485_DEBUG_H
#define RS485_DEBUG_H

#include <stddef.h>
#include <stdint.h>

#include "error_code.h"
#include "protocol_parsed_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RS485_DEBUG_FRAME_MAGIC 0x55AAu
#define RS485_DEBUG_FRAME_VERSION 0x01u
#define RS485_DEBUG_FRAME_HEADER_LENGTH 10u
#define RS485_DEBUG_FRAME_CRC_LENGTH 2u
#define RS485_DEBUG_FRAME_LENGTH \
    (RS485_DEBUG_FRAME_HEADER_LENGTH + UNIFIED_CAN_FD_DATA_MAX_LEN + RS485_DEBUG_FRAME_CRC_LENGTH)

unified_error_t rs485_debug_parse_frame(const uint8_t *buffer,
                                        size_t length,
                                        protocol_parsed_msg_t *out_msg);

#ifdef __cplusplus
}
#endif

#endif /* RS485_DEBUG_H */
