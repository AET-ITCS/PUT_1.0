/* RS485 Modbus 寄存器到 CAN 中间消息的内置映射表。 */
#ifndef RS485_REGISTER_MAP_H
#define RS485_REGISTER_MAP_H

#include <stddef.h>
#include <stdint.h>

#include "error_code.h"
#include "protocol_parsed_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

unified_error_t rs485_register_map_write_single(uint16_t address,
                                                uint16_t value,
                                                protocol_parsed_msg_t *out_msg);
unified_error_t rs485_register_map_write_multiple(uint16_t start_address,
                                                  const uint16_t *values,
                                                  uint16_t quantity,
                                                  protocol_parsed_msg_t *out_msg);

#ifdef __cplusplus
}
#endif

#endif /* RS485_REGISTER_MAP_H */
