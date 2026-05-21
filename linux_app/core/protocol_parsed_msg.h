/* CAN 转发字段结构：承接 UDP/RS485 等外部通道提取出的 CAN 字段。 */
#ifndef PROTOCOL_PARSED_MSG_H
#define PROTOCOL_PARSED_MSG_H

#include <stdint.h>

#include "unified_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 大核协议适配层提取出的 CAN 转发字段。
 *
 * 外部通道模块只负责把 UDP/TCP/RS485/蓝牙等输入中的 CAN 字段填入该结构，
 * 后续统一交给 frame_packer 打包成 unified_frame_t。
 */
typedef struct {
    protocol_type_t source_protocol;
    uint8_t vehicle_type;
    uint32_t source_id;
    uint32_t destination_id;
    uint32_t can_id;
    uint8_t can_dlc;
    uint8_t can_flags;
    uint8_t can_data[UNIFIED_CAN_FD_DATA_MAX_LEN];
} protocol_parsed_msg_t;

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_PARSED_MSG_H */
