/* 统一帧打包接口：把 CAN 转发字段封装为大核到小核的 unified_frame_t。 */
#ifndef FRAME_PACKER_H
#define FRAME_PACKER_H

#include <stdint.h>

#include "error_code.h"
#include "protocol_parsed_msg.h"
#include "unified_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

void frame_packer_init(uint32_t initial_sequence);
unified_error_t frame_packer_pack(const protocol_parsed_msg_t *msg, unified_frame_t *out_frame);
uint16_t frame_packer_calculate_crc(const unified_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_PACKER_H */
