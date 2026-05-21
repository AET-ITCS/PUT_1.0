/**
 * @file rtos_protocol_adapter.c
 * @brief FreeRTOS 协议适配实现。
 *
 * 默认实现解析仓库内联调使用的 unified_frame_t payload，并保留可替换
 * hook 以兼容测试和后续真实 IPC 接入。
 */
#include "rtos_protocol_adapter.h"

#include <stdbool.h>
#include <string.h>

#include "crc16.h"
#include "unified_frame.h"

#if RTOS_IPC_PAYLOAD_MAX_LEN < UNIFIED_FRAME_LENGTH
#error "RTOS_IPC_PAYLOAD_MAX_LEN must fit unified_frame_t"
#endif

static rtos_protocol_adapter_linux_payload_to_can_fn_t g_linux_payload_to_can;
static uint32_t g_rtos_to_linux_sequence;

static bool protocol_type_is_valid(uint8_t type)
{
    return (type == (uint8_t)PROTOCOL_TYPE_UNKNOWN) ||
           (type == (uint8_t)PROTOCOL_TYPE_4G) ||
           (type == (uint8_t)PROTOCOL_TYPE_WIFI) ||
           (type == (uint8_t)PROTOCOL_TYPE_BLUETOOTH) ||
           (type == (uint8_t)PROTOCOL_TYPE_ETHERNET) ||
           (type == (uint8_t)PROTOCOL_TYPE_RS485) ||
           (type == (uint8_t)PROTOCOL_TYPE_CLOUD_MQTT);
}

static bool unified_flags_are_supported(uint8_t can_flags)
{
    return (can_flags & (uint8_t)~UNIFIED_CAN_FLAG_EXTENDED_ID) == 0u;
}

static bool rtos_flags_are_supported(uint8_t can_flags)
{
    return (can_flags & (uint8_t)~RTOS_CAN_FLAG_EXTENDED_ID) == 0u;
}

static bool can_id_is_valid(uint32_t can_id, uint8_t can_flags)
{
    if ((can_flags & (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID) != 0u) {
        return can_id <= RTOS_CAN_EXTENDED_ID_MAX;
    }

    return can_id <= RTOS_CAN_STANDARD_ID_MAX;
}

static uint8_t unified_flags_to_rtos(uint8_t can_flags)
{
    return ((can_flags & (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID) != 0u) ?
           (uint8_t)RTOS_CAN_FLAG_EXTENDED_ID : (uint8_t)RTOS_CAN_FLAG_NONE;
}

static uint8_t rtos_flags_to_unified(uint8_t can_flags)
{
    return ((can_flags & (uint8_t)RTOS_CAN_FLAG_EXTENDED_ID) != 0u) ?
           (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID : (uint8_t)UNIFIED_CAN_FLAG_NONE;
}

static unified_error_t default_linux_payload_to_can(
    const rtos_ipc_payload_view_t *payload,
    rtos_can_message_t *out_message)
{
    unified_frame_t frame;
    uint16_t actual_crc;

    if ((payload == 0) || (out_message == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if ((payload->bytes == 0) || (payload->length != UNIFIED_FRAME_LENGTH)) {
        return UNIFIED_ERR_LENGTH;
    }

    memcpy(&frame, payload->bytes, sizeof(frame));

    if (frame.magic != UNIFIED_FRAME_MAGIC) {
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if (frame.version != UNIFIED_FRAME_VERSION) {
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if (frame.frame_type != (uint8_t)UNIFIED_FRAME_TYPE_CAN_DATA) {
        return UNIFIED_ERR_UNKNOWN_TYPE;
    }

    if (!protocol_type_is_valid(frame.source_protocol) ||
        !vehicle_msg_type_is_valid(frame.vehicle_type)) {
        return UNIFIED_ERR_UNKNOWN_TYPE;
    }

    if (!unified_flags_are_supported(frame.can_flags)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (!unified_frame_can_dlc_is_valid(frame.can_dlc, frame.can_flags) ||
        (frame.can_dlc > RTOS_CAN_CLASSIC_DATA_MAX_LEN)) {
        return UNIFIED_ERR_CAN_DLC;
    }

    actual_crc = unified_crc16_ccitt_false((const uint8_t *)&frame,
                                           UNIFIED_FRAME_CRC_INPUT_LENGTH);
    if (actual_crc != frame.crc16) {
        return UNIFIED_ERR_CRC;
    }

    if (!can_id_is_valid(frame.can_id, frame.can_flags)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(out_message, 0, sizeof(*out_message));
    out_message->can_id = frame.can_id;
    out_message->can_dlc = frame.can_dlc;
    out_message->can_flags = unified_flags_to_rtos(frame.can_flags);
    if (frame.can_dlc > 0u) {
        memcpy(out_message->can_data, frame.can_data, frame.can_dlc);
    }

    return UNIFIED_OK;
}

void rtos_protocol_adapter_init(void)
{
    g_linux_payload_to_can = 0;
    g_rtos_to_linux_sequence = 1u;
}

void rtos_protocol_adapter_set_linux_payload_to_can(
    rtos_protocol_adapter_linux_payload_to_can_fn_t handler)
{
    g_linux_payload_to_can = handler;
}

unified_error_t rtos_protocol_adapter_linux_payload_to_can(
    const rtos_ipc_payload_t *payload,
    rtos_can_message_t *out_message)
{
    rtos_ipc_payload_view_t view;

    if ((payload == 0) || (out_message == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (payload->length > RTOS_IPC_PAYLOAD_MAX_LEN) {
        return UNIFIED_ERR_LENGTH;
    }

    if (g_linux_payload_to_can != 0) {
        view.bytes = payload->bytes;
        view.length = payload->length;
        return g_linux_payload_to_can(&view, out_message);
    }

    view.bytes = payload->bytes;
    view.length = payload->length;
    return default_linux_payload_to_can(&view, out_message);
}

unified_error_t rtos_protocol_adapter_can_rx_to_linux_payload(
    const rtos_can_message_t *message,
    rtos_ipc_payload_t *out_payload)
{
    unified_frame_t frame;

    if ((message == 0) || (out_payload == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (!rtos_flags_are_supported(message->can_flags) ||
        !can_id_is_valid(message->can_id, message->can_flags)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (message->can_dlc > RTOS_CAN_CLASSIC_DATA_MAX_LEN) {
        return UNIFIED_ERR_CAN_DLC;
    }

    memset(&frame, 0, sizeof(frame));
    frame.magic = UNIFIED_FRAME_MAGIC;
    frame.version = UNIFIED_FRAME_VERSION;
    frame.frame_type = (uint8_t)UNIFIED_FRAME_TYPE_CAN_DATA;
    frame.source_protocol = (uint8_t)PROTOCOL_TYPE_UNKNOWN;
    frame.vehicle_type = 0u;
    frame.can_dlc = message->can_dlc;
    frame.can_flags = rtos_flags_to_unified(message->can_flags);
    frame.sequence = g_rtos_to_linux_sequence++;
    frame.timestamp_ms = 0u;
    frame.source_id = 0u;
    frame.destination_id = 0u;
    frame.can_id = message->can_id;
    if (message->can_dlc > 0u) {
        memcpy(frame.can_data, message->can_data, message->can_dlc);
    }
    frame.crc16 = unified_crc16_ccitt_false((const uint8_t *)&frame,
                                            UNIFIED_FRAME_CRC_INPUT_LENGTH);

    memset(out_payload, 0, sizeof(*out_payload));
    out_payload->length = UNIFIED_FRAME_LENGTH;
    memcpy(out_payload->bytes, &frame, sizeof(frame));
    return UNIFIED_OK;
}
