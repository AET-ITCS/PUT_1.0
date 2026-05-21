/* RS485 Modbus RTU 写寄存器解析：Modbus 请求 -> protocol_parsed_msg_t。 */
#include "rs485_modbus.h"

#include <string.h>

#include "rs485_register_map.h"

static uint16_t load_u16_be(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8u) | (uint16_t)data[1];
}

static uint16_t load_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

static void store_u16_be(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)((value >> 8u) & 0xFFu);
    data[1] = (uint8_t)(value & 0xFFu);
}

static void append_crc(uint8_t *frame, size_t payload_length)
{
    uint16_t crc = rs485_modbus_crc16(frame, payload_length);
    frame[payload_length] = (uint8_t)(crc & 0xFFu);
    frame[payload_length + 1u] = (uint8_t)((crc >> 8u) & 0xFFu);
}

uint16_t rs485_modbus_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFu;

    if (data == NULL) {
        return 0u;
    }

    for (size_t i = 0u; i < length; ++i) {
        crc ^= (uint16_t)data[i];
        for (uint8_t bit = 0u; bit < 8u; ++bit) {
            if ((crc & 0x0001u) != 0u) {
                crc = (uint16_t)((crc >> 1u) ^ 0xA001u);
            } else {
                crc >>= 1u;
            }
        }
    }

    return crc;
}

size_t rs485_modbus_expected_length(const uint8_t *buffer, size_t buffered_length)
{
    uint8_t function_code;
    uint8_t byte_count;
    size_t expected;

    if (buffer == NULL) {
        return RS485_MODBUS_FRAME_INVALID;
    }

    if (buffered_length < 2u) {
        return RS485_MODBUS_FRAME_INCOMPLETE;
    }

    function_code = buffer[1];
    if (function_code == RS485_MODBUS_FUNC_WRITE_SINGLE) {
        return 8u;
    }

    if (function_code == RS485_MODBUS_FUNC_WRITE_MULTIPLE) {
        if (buffered_length < 7u) {
            return RS485_MODBUS_FRAME_INCOMPLETE;
        }

        byte_count = buffer[6];
        expected = 9u + (size_t)byte_count;
        if ((byte_count == 0u) || ((byte_count % 2u) != 0u) || (expected > RS485_MODBUS_MAX_FRAME_LENGTH)) {
            return RS485_MODBUS_FRAME_INVALID;
        }
        return expected;
    }

    /* 第一版对未知功能码按 8 字节请求尝试校验并返回 exception。 */
    return 8u;
}

static void build_exception_response(uint8_t slave_id,
                                     uint8_t function_code,
                                     uint8_t exception_code,
                                     rs485_modbus_result_t *result)
{
    result->action = RS485_MODBUS_ACTION_EXCEPTION;
    result->exception_code = exception_code;
    result->response[0] = slave_id;
    result->response[1] = (uint8_t)(function_code | 0x80u);
    result->response[2] = exception_code;
    append_crc(result->response, 3u);
    result->response_length = 5u;
}

static void set_exception_result(uint8_t slave_id,
                                 uint8_t function_code,
                                 uint8_t exception_code,
                                 rs485_modbus_result_t *result)
{
    result->action = RS485_MODBUS_ACTION_EXCEPTION;
    result->exception_code = exception_code;
    if (result->response_required) {
        build_exception_response(slave_id, function_code, exception_code, result);
    }
}

static bool frame_address_matches(uint8_t address, uint8_t slave_id)
{
    return (address == RS485_MODBUS_BROADCAST_SLAVE_ID) || (address == slave_id);
}

static unified_error_t validate_common(const uint8_t *frame,
                                       size_t length,
                                       uint8_t slave_id,
                                       bool response_enabled,
                                       rs485_modbus_result_t *result)
{
    uint16_t expected_crc;
    uint16_t actual_crc;
    size_t expected_length;
    uint8_t address;

    if ((frame == NULL) || (result == NULL)) {
        return UNIFIED_ERR_NULL;
    }

    memset(result, 0, sizeof(*result));
    result->action = RS485_MODBUS_ACTION_NONE;
    result->response_required = false;
    result->error = UNIFIED_OK;

    if (length < 4u) {
        result->error = UNIFIED_ERR_LENGTH;
        return UNIFIED_ERR_LENGTH;
    }

    expected_length = rs485_modbus_expected_length(frame, length);
    if ((expected_length == RS485_MODBUS_FRAME_INCOMPLETE) ||
        (expected_length == RS485_MODBUS_FRAME_INVALID) ||
        (length != expected_length)) {
        result->error = UNIFIED_ERR_LENGTH;
        return UNIFIED_ERR_LENGTH;
    }

    address = frame[0];
    if (!frame_address_matches(address, slave_id)) {
        result->error = UNIFIED_ERR_INVALID_ARG;
        return UNIFIED_ERR_INVALID_ARG;
    }

    expected_crc = load_u16_le(&frame[length - 2u]);
    actual_crc = rs485_modbus_crc16(frame, length - 2u);
    if (expected_crc != actual_crc) {
        result->error = UNIFIED_ERR_CRC;
        return UNIFIED_ERR_CRC;
    }

    result->response_required =
        response_enabled && (address != RS485_MODBUS_BROADCAST_SLAVE_ID);
    return UNIFIED_OK;
}

static unified_error_t parse_write_single(const uint8_t *frame,
                                          bool response_enabled,
                                          rs485_modbus_result_t *result)
{
    uint16_t address = load_u16_be(&frame[2]);
    uint16_t value = load_u16_be(&frame[4]);
    unified_error_t err;

    err = rs485_register_map_write_single(address, value, &result->msg);
    if (err != UNIFIED_OK) {
        result->error = err;
        set_exception_result(frame[0],
                             frame[1],
                             RS485_MODBUS_EXC_ILLEGAL_DATA_ADDRESS,
                             result);
        return UNIFIED_OK;
    }

    result->action = RS485_MODBUS_ACTION_FORWARD;
    result->error = UNIFIED_OK;
    if (result->response_required && response_enabled) {
        memcpy(result->response, frame, 6u);
        append_crc(result->response, 6u);
        result->response_length = 8u;
    }
    return UNIFIED_OK;
}

static unified_error_t parse_write_multiple(const uint8_t *frame,
                                            size_t length,
                                            bool response_enabled,
                                            rs485_modbus_result_t *result)
{
    uint16_t start_address = load_u16_be(&frame[2]);
    uint16_t quantity = load_u16_be(&frame[4]);
    uint8_t byte_count = frame[6];
    uint16_t values[123];
    unified_error_t err;

    if ((quantity == 0u) || (quantity > 123u) || (byte_count != (uint8_t)(quantity * 2u)) ||
        (length != (size_t)(9u + byte_count))) {
        result->error = UNIFIED_ERR_LENGTH;
        return UNIFIED_ERR_LENGTH;
    }

    for (uint16_t i = 0u; i < quantity; ++i) {
        values[i] = load_u16_be(&frame[7u + ((size_t)i * 2u)]);
    }

    err = rs485_register_map_write_multiple(start_address, values, quantity, &result->msg);
    if (err != UNIFIED_OK) {
        result->error = err;
        set_exception_result(frame[0],
                             frame[1],
                             RS485_MODBUS_EXC_ILLEGAL_DATA_ADDRESS,
                             result);
        return UNIFIED_OK;
    }

    result->action = RS485_MODBUS_ACTION_FORWARD;
    result->error = UNIFIED_OK;
    if (result->response_required && response_enabled) {
        result->response[0] = frame[0];
        result->response[1] = frame[1];
        store_u16_be(&result->response[2], start_address);
        store_u16_be(&result->response[4], quantity);
        append_crc(result->response, 6u);
        result->response_length = 8u;
    }
    return UNIFIED_OK;
}

unified_error_t rs485_modbus_parse_request(const uint8_t *frame,
                                           size_t length,
                                           uint8_t slave_id,
                                           bool response_enabled,
                                           rs485_modbus_result_t *out_result)
{
    unified_error_t err;
    uint8_t function_code;

    err = validate_common(frame, length, slave_id, response_enabled, out_result);
    if (err != UNIFIED_OK) {
        return err;
    }

    function_code = frame[1];
    if (function_code == RS485_MODBUS_FUNC_WRITE_SINGLE) {
        return parse_write_single(frame, response_enabled, out_result);
    }

    if (function_code == RS485_MODBUS_FUNC_WRITE_MULTIPLE) {
        return parse_write_multiple(frame, length, response_enabled, out_result);
    }

    out_result->error = UNIFIED_ERR_UNKNOWN_TYPE;
    set_exception_result(frame[0],
                         function_code,
                         RS485_MODBUS_EXC_ILLEGAL_FUNCTION,
                         out_result);
    return UNIFIED_OK;
}
