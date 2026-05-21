/* RS485 CAN direct 网关转发入口：只做 AA55 分帧和 CAN 字段提取，不解释业务语义。 */
#include "rs485_can_direct.h"

#include <string.h>

#include "can_direct_frame.h"

void rs485_can_direct_sync_init(rs485_can_direct_sync_t *sync)
{
    if (sync == NULL) {
        return;
    }

    memset(sync, 0, sizeof(*sync));
}

bool rs485_can_direct_sync_feed(rs485_can_direct_sync_t *sync,
                                uint8_t byte,
                                uint8_t out_frame[RS485_CAN_DIRECT_FRAME_LENGTH])
{
    if ((sync == NULL) || (out_frame == NULL)) {
        return false;
    }

    if (sync->pos == 0u) {
        if (byte == 0xAAu) {
            sync->frame[0] = byte;
            sync->pos = 1u;
        }
        return false;
    }

    if (sync->pos == 1u) {
        if (byte == 0x55u) {
            sync->frame[1] = byte;
            sync->pos = 2u;
        } else if (byte == 0xAAu) {
            sync->frame[0] = byte;
            sync->pos = 1u;
        } else {
            sync->pos = 0u;
        }
        return false;
    }

    sync->frame[sync->pos++] = byte;
    if (sync->pos == RS485_CAN_DIRECT_FRAME_LENGTH) {
        memcpy(out_frame, sync->frame, RS485_CAN_DIRECT_FRAME_LENGTH);
        sync->pos = 0u;
        return true;
    }

    return false;
}

unified_error_t rs485_can_direct_parse_frame(const uint8_t *buffer,
                                             size_t length,
                                             protocol_parsed_msg_t *out_msg)
{
    /* 这里不是应用层解析，只是提取 CAN 字段。 */
    return can_direct_parse_frame(buffer, length, PROTOCOL_TYPE_RS485, out_msg);
}
