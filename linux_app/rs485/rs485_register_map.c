/* RS485 Modbus demo 寄存器映射实现。 */
#include "rs485_register_map.h"

#include <string.h>

#include "protocol_type.h"

typedef struct {
    uint16_t address;
    uint8_t vehicle_type;
    uint32_t can_id;
} single_register_mapping_t;

static const single_register_mapping_t g_single_mappings[] = {
    {0x0001u, (uint8_t)VEHICLE_MSG_TYPE_LIGHT_CONTROL, 0x123u},
    {0x0002u, (uint8_t)VEHICLE_MSG_TYPE_WINDOW_CONTROL, 0x321u},
    {0x0003u, (uint8_t)VEHICLE_MSG_TYPE_SEAT_CONTROL, 0x220u},
};

static void fill_common(protocol_parsed_msg_t *out_msg, uint8_t vehicle_type, uint32_t can_id)
{
    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->source_protocol = PROTOCOL_TYPE_RS485;
    out_msg->vehicle_type = vehicle_type;
    out_msg->can_flags = (uint8_t)UNIFIED_CAN_FLAG_NONE;
    out_msg->can_dlc = 8u;
    out_msg->can_id = can_id;
}

unified_error_t rs485_register_map_write_single(uint16_t address,
                                                uint16_t value,
                                                protocol_parsed_msg_t *out_msg)
{
    if (out_msg == NULL) {
        return UNIFIED_ERR_NULL;
    }

    for (size_t i = 0u; i < (sizeof(g_single_mappings) / sizeof(g_single_mappings[0])); ++i) {
        if (g_single_mappings[i].address == address) {
            fill_common(out_msg, g_single_mappings[i].vehicle_type, g_single_mappings[i].can_id);
            out_msg->can_data[0] = (uint8_t)((value >> 8u) & 0xFFu);
            out_msg->can_data[1] = (uint8_t)(value & 0xFFu);
            return UNIFIED_OK;
        }
    }

    return UNIFIED_ERR_UNKNOWN_TYPE;
}

unified_error_t rs485_register_map_write_multiple(uint16_t start_address,
                                                  const uint16_t *values,
                                                  uint16_t quantity,
                                                  protocol_parsed_msg_t *out_msg)
{
    if ((values == NULL) || (out_msg == NULL)) {
        return UNIFIED_ERR_NULL;
    }

    if ((start_address != 0x0100u) || (quantity != 4u)) {
        return UNIFIED_ERR_UNKNOWN_TYPE;
    }

    fill_common(out_msg, (uint8_t)VEHICLE_MSG_TYPE_SEAT_CONTROL, 0x220u);
    for (uint16_t i = 0u; i < quantity; ++i) {
        out_msg->can_data[(size_t)i * 2u] = (uint8_t)((values[i] >> 8u) & 0xFFu);
        out_msg->can_data[((size_t)i * 2u) + 1u] = (uint8_t)(values[i] & 0xFFu);
    }

    return UNIFIED_OK;
}
