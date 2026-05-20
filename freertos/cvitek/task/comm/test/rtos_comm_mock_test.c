#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crc16.h"
#include "rtos_can_driver.h"
#include "rtos_can_forward.h"
#include "rtos_status.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                    __FILE__, __LINE__, #condition);                         \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static void fill_valid_frame(unified_frame_t *frame)
{
    static const uint8_t payload[UNIFIED_CAN_CLASSIC_DATA_MAX_LEN] = {
        0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u, 0x70u, 0x80u,
    };

    memset(frame, 0, sizeof(*frame));
    frame->magic = UNIFIED_FRAME_MAGIC;
    frame->version = UNIFIED_FRAME_VERSION;
    frame->frame_type = (uint8_t)UNIFIED_FRAME_TYPE_CAN_DATA;
    frame->source_protocol = (uint8_t)PROTOCOL_TYPE_ETHERNET;
    frame->vehicle_type = (uint8_t)VEHICLE_MSG_TYPE_LIGHT_CONTROL;
    frame->can_dlc = (uint8_t)sizeof(payload);
    frame->can_flags = (uint8_t)UNIFIED_CAN_FLAG_NONE;
    frame->sequence = 1u;
    frame->timestamp_ms = 100u;
    frame->source_id = 1u;
    frame->destination_id = 2u;
    frame->can_id = 0x123u;
    memcpy(frame->can_data, payload, sizeof(payload));
    frame->crc16 = unified_crc16_ccitt_false((const uint8_t *)frame,
                                             UNIFIED_FRAME_CRC_INPUT_LENGTH);
}

int main(void)
{
    unified_frame_t frame;
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    fill_valid_frame(&frame);
    CHECK(gateway_forward_init() == UNIFIED_OK);
    CHECK(rtos_can_forward_submit_frame(&frame) == UNIFIED_OK);
    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 1u);
    CHECK(status.tx_to_can_ok == 1u);
    CHECK(status.tx_to_can_fail == 0u);
    CHECK(driver.send_count == 1u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    CHECK(rtos_can_forward_submit_frame(NULL) == UNIFIED_ERR_INVALID_ARG);
    rtos_status_get_snapshot(&status);
    CHECK(status.drop_null == 1u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_frame(&frame);
    frame.magic = 0u;
    CHECK(rtos_can_forward_submit_frame(&frame) == UNIFIED_ERR_INVALID_ARG);
    rtos_status_get_snapshot(&status);
    CHECK(status.drop_magic == 1u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_frame(&frame);
    frame.version = 0u;
    CHECK(rtos_can_forward_submit_frame(&frame) == UNIFIED_ERR_INVALID_ARG);
    rtos_status_get_snapshot(&status);
    CHECK(status.drop_version == 1u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_frame(&frame);
    frame.can_flags = (uint8_t)UNIFIED_CAN_FLAG_FD;
    CHECK(rtos_can_forward_submit_frame(&frame) == UNIFIED_ERR_INVALID_ARG);
    rtos_status_get_snapshot(&status);
    CHECK(status.drop_flag == 1u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_frame(&frame);
    frame.can_id = 0x800u;
    CHECK(rtos_can_forward_submit_frame(&frame) == UNIFIED_ERR_INVALID_ARG);
    rtos_status_get_snapshot(&status);
    CHECK(status.drop_can_id == 1u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_frame(&frame);
    frame.can_dlc = 9u;
    CHECK(rtos_can_forward_submit_frame(&frame) == UNIFIED_ERR_INVALID_ARG);
    rtos_status_get_snapshot(&status);
    CHECK(status.drop_dlc == 1u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_frame(&frame);
    frame.crc16 ^= 0xFFFFu;
    CHECK(rtos_can_forward_submit_frame(&frame) == UNIFIED_ERR_INVALID_ARG);
    rtos_status_get_snapshot(&status);
    CHECK(status.drop_crc == 1u);

    return 0;
}
