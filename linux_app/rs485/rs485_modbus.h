/* RS485 Modbus RTU 写寄存器协议解析接口。 */
#ifndef RS485_MODBUS_H
#define RS485_MODBUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_code.h"
#include "protocol_parsed_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RS485_MODBUS_MAX_FRAME_LENGTH 256u
#define RS485_MODBUS_MAX_RESPONSE_LENGTH 8u
#define RS485_MODBUS_FRAME_INCOMPLETE 0u
#define RS485_MODBUS_FRAME_INVALID ((size_t)-1)

#define RS485_MODBUS_FUNC_WRITE_SINGLE 0x06u
#define RS485_MODBUS_FUNC_WRITE_MULTIPLE 0x10u
#define RS485_MODBUS_EXC_ILLEGAL_FUNCTION 0x01u
#define RS485_MODBUS_EXC_ILLEGAL_DATA_ADDRESS 0x02u
#define RS485_MODBUS_BROADCAST_SLAVE_ID 0x00u

typedef enum {
    RS485_MODBUS_ACTION_NONE = 0,
    RS485_MODBUS_ACTION_FORWARD,
    RS485_MODBUS_ACTION_EXCEPTION,
} rs485_modbus_action_t;

typedef struct {
    rs485_modbus_action_t action;
    bool response_required;
    uint8_t response[RS485_MODBUS_MAX_RESPONSE_LENGTH];
    size_t response_length;
    protocol_parsed_msg_t msg;
    unified_error_t error;
    uint8_t exception_code;
} rs485_modbus_result_t;

uint16_t rs485_modbus_crc16(const uint8_t *data, size_t length);
size_t rs485_modbus_expected_length(const uint8_t *buffer, size_t buffered_length);
unified_error_t rs485_modbus_parse_request(const uint8_t *frame,
                                           size_t length,
                                           uint8_t slave_id,
                                           bool response_enabled,
                                           rs485_modbus_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif /* RS485_MODBUS_H */
