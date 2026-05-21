/**
 * @file rtos_comm_stage6_test.c
 * @brief Host tests for stage 6 unified_frame_t protocol integration.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crc16.h"
#include "frame_packer.h"
#include "rtos_can_driver.h"
#include "rtos_can_forward.h"
#include "rtos_can_task.h"
#include "rtos_config.h"
#include "rtos_ipc.h"
#include "rtos_recovery.h"
#include "rtos_status.h"
#include "unified_frame.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                    __FILE__, __LINE__, #condition);                         \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static uint32_t g_payload_send_count;
static rtos_ipc_payload_t g_last_payload;
static bool g_fail_payload_send;

static void reset_payload_sender_state(void)
{
    g_payload_send_count = 0u;
    memset(&g_last_payload, 0, sizeof(g_last_payload));
    g_fail_payload_send = false;
}

static unified_error_t test_payload_sender(const rtos_ipc_payload_t *payload)
{
    if (payload == 0) {
        return UNIFIED_ERR_NULL;
    }

    ++g_payload_send_count;
    g_last_payload = *payload;
    return g_fail_payload_send ? UNIFIED_ERR_INVALID_ARG : UNIFIED_OK;
}

static void fill_parsed_msg(protocol_parsed_msg_t *msg,
                            uint32_t can_id,
                            uint8_t can_flags,
                            uint8_t can_dlc)
{
    uint8_t i;

    memset(msg, 0, sizeof(*msg));
    msg->source_protocol = PROTOCOL_TYPE_ETHERNET;
    msg->vehicle_type = (uint8_t)VEHICLE_MSG_TYPE_LIGHT_CONTROL;
    msg->source_id = 0x11u;
    msg->destination_id = 0x22u;
    msg->can_id = can_id;
    msg->can_dlc = can_dlc;
    msg->can_flags = can_flags;
    for (i = 0u; i < UNIFIED_CAN_FD_DATA_MAX_LEN; ++i) {
        msg->can_data[i] = (uint8_t)(0xA0u + i);
    }
}

static unified_error_t make_frame(unified_frame_t *frame,
                                  uint32_t can_id,
                                  uint8_t can_flags,
                                  uint8_t can_dlc)
{
    protocol_parsed_msg_t msg;

    fill_parsed_msg(&msg, can_id, can_flags, can_dlc);
    return frame_packer_pack(&msg, frame);
}

static void refresh_frame_crc(unified_frame_t *frame)
{
    frame->crc16 = unified_crc16_ccitt_false((const uint8_t *)frame,
                                             UNIFIED_FRAME_CRC_INPUT_LENGTH);
}

static int check_last_tx_matches_frame(const rtos_can_driver_mock_snapshot_t *driver,
                                       const unified_frame_t *frame)
{
    uint8_t expected_flags =
        ((frame->can_flags & (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID) != 0u) ?
        (uint8_t)RTOS_CAN_FLAG_EXTENDED_ID : (uint8_t)RTOS_CAN_FLAG_NONE;

    CHECK(driver->has_last_tx_message);
    CHECK(driver->last_tx_message.can_id == frame->can_id);
    CHECK(driver->last_tx_message.can_dlc == frame->can_dlc);
    CHECK(driver->last_tx_message.can_flags == expected_flags);
    CHECK(memcmp(driver->last_tx_message.can_data,
                 frame->can_data,
                 frame->can_dlc) == 0);
    return 0;
}

static int expect_frame_rejected(const unified_frame_t *frame, uint16_t length)
{
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    CHECK(gateway_forward_init() == UNIFIED_OK);
    CHECK(rtos_ipc_mock_receive_payload((const uint8_t *)frame, length) == UNIFIED_OK);
    Gateway_IPC_Task(0);

    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 0u);
    CHECK(status.tx_to_can_ok == 0u);
    CHECK(status.ipc_payload_drop == 1u);
    CHECK(driver.send_count == 0u);
    CHECK(!driver.has_last_tx_message);
    return 0;
}

static int test_valid_standard_frame_sends_to_can(void)
{
    unified_frame_t frame;
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    frame_packer_init(1u);
    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    CHECK(gateway_forward_init() == UNIFIED_OK);
    CHECK(rtos_ipc_mock_receive_payload((const uint8_t *)&frame,
                                        UNIFIED_FRAME_LENGTH) == UNIFIED_OK);
    Gateway_IPC_Task(0);

    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 1u);
    CHECK(status.tx_to_can_ok == 1u);
    CHECK(status.ipc_payload_drop == 0u);
    CHECK(driver.send_count == 1u);
    CHECK(check_last_tx_matches_frame(&driver, &frame) == 0);
    return 0;
}

static int test_valid_extended_frame_maps_flag(void)
{
    unified_frame_t frame;
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    frame_packer_init(1u);
    CHECK(make_frame(&frame,
                     0x18DAF110u,
                     (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID,
                     3u) == UNIFIED_OK);
    CHECK(gateway_forward_init() == UNIFIED_OK);
    CHECK(rtos_ipc_mock_receive_payload((const uint8_t *)&frame,
                                        UNIFIED_FRAME_LENGTH) == UNIFIED_OK);
    Gateway_IPC_Task(0);

    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 1u);
    CHECK(status.tx_to_can_ok == 1u);
    CHECK(driver.send_count == 1u);
    CHECK(check_last_tx_matches_frame(&driver, &frame) == 0);
    CHECK(driver.last_tx_message.can_flags == (uint8_t)RTOS_CAN_FLAG_EXTENDED_ID);
    return 0;
}

static int test_invalid_frames_are_rejected(void)
{
    unified_frame_t frame;

    frame_packer_init(1u);
    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    CHECK(expect_frame_rejected(&frame, (uint16_t)(UNIFIED_FRAME_LENGTH - 1u)) == 0);

    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    frame.magic = 0x0000u;
    CHECK(expect_frame_rejected(&frame, UNIFIED_FRAME_LENGTH) == 0);

    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    frame.version = 0x7Fu;
    CHECK(expect_frame_rejected(&frame, UNIFIED_FRAME_LENGTH) == 0);

    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    frame.frame_type = (uint8_t)UNIFIED_FRAME_TYPE_STATUS;
    refresh_frame_crc(&frame);
    CHECK(expect_frame_rejected(&frame, UNIFIED_FRAME_LENGTH) == 0);

    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    frame.source_protocol = 0xEEu;
    refresh_frame_crc(&frame);
    CHECK(expect_frame_rejected(&frame, UNIFIED_FRAME_LENGTH) == 0);

    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    frame.vehicle_type = 0x00u;
    refresh_frame_crc(&frame);
    CHECK(expect_frame_rejected(&frame, UNIFIED_FRAME_LENGTH) == 0);

    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    frame.can_flags = (uint8_t)UNIFIED_CAN_FLAG_FD;
    refresh_frame_crc(&frame);
    CHECK(expect_frame_rejected(&frame, UNIFIED_FRAME_LENGTH) == 0);

    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    frame.can_flags = (uint8_t)UNIFIED_CAN_FLAG_RTR;
    refresh_frame_crc(&frame);
    CHECK(expect_frame_rejected(&frame, UNIFIED_FRAME_LENGTH) == 0);

    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    frame.can_flags = (uint8_t)UNIFIED_CAN_FLAG_BRS;
    refresh_frame_crc(&frame);
    CHECK(expect_frame_rejected(&frame, UNIFIED_FRAME_LENGTH) == 0);

    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    frame.can_dlc = (uint8_t)(RTOS_CAN_CLASSIC_DATA_MAX_LEN + 1u);
    refresh_frame_crc(&frame);
    CHECK(expect_frame_rejected(&frame, UNIFIED_FRAME_LENGTH) == 0);

    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    frame.can_id = (uint32_t)RTOS_CAN_STANDARD_ID_MAX + 1u;
    refresh_frame_crc(&frame);
    CHECK(expect_frame_rejected(&frame, UNIFIED_FRAME_LENGTH) == 0);

    CHECK(make_frame(&frame,
                     RTOS_CAN_EXTENDED_ID_MAX,
                     (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID,
                     8u) == UNIFIED_OK);
    frame.can_id = RTOS_CAN_EXTENDED_ID_MAX + 1u;
    refresh_frame_crc(&frame);
    CHECK(expect_frame_rejected(&frame, UNIFIED_FRAME_LENGTH) == 0);

    CHECK(make_frame(&frame, 0x123u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    frame.can_data[0] ^= 0x01u;
    CHECK(expect_frame_rejected(&frame, UNIFIED_FRAME_LENGTH) == 0);
    return 0;
}

static void fill_can_rx_message(rtos_can_message_t *message)
{
    static const uint8_t payload[4] = { 0x12u, 0x34u, 0x56u, 0x78u };

    memset(message, 0, sizeof(*message));
    message->can_id = 0x18DAF121u;
    message->can_dlc = (uint8_t)sizeof(payload);
    message->can_flags = (uint8_t)RTOS_CAN_FLAG_EXTENDED_ID;
    memcpy(message->can_data, payload, sizeof(payload));
}

static int test_can_rx_packs_unified_payload(void)
{
    rtos_can_message_t message;
    rtos_status_snapshot_t status;
    unified_frame_t frame;
    uint16_t actual_crc;

    reset_payload_sender_state();
    CHECK(gateway_forward_init() == UNIFIED_OK);
    rtos_ipc_set_payload_sender(test_payload_sender);
    fill_can_rx_message(&message);
    CHECK(rtos_can_driver_mock_inject_rx(&message) == UNIFIED_OK);

    rtos_can_task_gpio14_irq_notify();
    CAN_RX_Task(0);

    rtos_status_get_snapshot(&status);
    CHECK(status.rx_from_can == 1u);
    CHECK(status.tx_to_linux == 1u);
    CHECK(status.drop_ring_full == 0u);
    CHECK(g_payload_send_count == 1u);
    CHECK(g_last_payload.length == UNIFIED_FRAME_LENGTH);

    memcpy(&frame, g_last_payload.bytes, sizeof(frame));
    actual_crc = unified_crc16_ccitt_false((const uint8_t *)&frame,
                                           UNIFIED_FRAME_CRC_INPUT_LENGTH);
    CHECK(frame.magic == UNIFIED_FRAME_MAGIC);
    CHECK(frame.version == UNIFIED_FRAME_VERSION);
    CHECK(frame.frame_type == (uint8_t)UNIFIED_FRAME_TYPE_CAN_DATA);
    CHECK(frame.source_protocol == (uint8_t)PROTOCOL_TYPE_UNKNOWN);
    CHECK(frame.vehicle_type == 0u);
    CHECK(frame.source_id == 0u);
    CHECK(frame.destination_id == 0u);
    CHECK(frame.timestamp_ms == 0u);
    CHECK(frame.can_id == message.can_id);
    CHECK(frame.can_dlc == message.can_dlc);
    CHECK(frame.can_flags == (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID);
    CHECK(memcmp(frame.can_data, message.can_data, message.can_dlc) == 0);
    CHECK(actual_crc == frame.crc16);
    return 0;
}

static int test_offline_drops_old_payload_and_rehandshake_sends_new(void)
{
    unified_frame_t old_frame;
    unified_frame_t new_frame;
    rtos_can_driver_mock_snapshot_t driver;

    frame_packer_init(1u);
    CHECK(make_frame(&old_frame, 0x601u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    CHECK(make_frame(&new_frame, 0x602u, (uint8_t)UNIFIED_CAN_FLAG_NONE, 8u) == UNIFIED_OK);
    CHECK(gateway_forward_init() == UNIFIED_OK);

    CHECK(rtos_recovery_watchdog_check_once(RTOS_LINUX_HEARTBEAT_TIMEOUT_MS + 1u) ==
          UNIFIED_OK);
    CHECK(rtos_ipc_mock_receive_payload((const uint8_t *)&old_frame,
                                        UNIFIED_FRAME_LENGTH) == UNIFIED_OK);
    Gateway_IPC_Task(0);

    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(driver.send_count == 0u);
    CHECK(!driver.has_last_tx_message);
    CHECK(rtos_can_forward_get_tx_queue_depth() == 0u);

    CHECK(rtos_recovery_complete_linux_rehandshake(RTOS_LINUX_HEARTBEAT_TIMEOUT_MS + 2u) ==
          UNIFIED_OK);
    CHECK(rtos_ipc_mock_receive_payload((const uint8_t *)&new_frame,
                                        UNIFIED_FRAME_LENGTH) == UNIFIED_OK);
    Gateway_IPC_Task(0);

    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(driver.send_count == 1u);
    CHECK(driver.has_last_tx_message);
    CHECK(driver.last_tx_message.can_id == new_frame.can_id);
    CHECK(driver.last_tx_message.can_id != old_frame.can_id);
    return 0;
}

int main(void)
{
    CHECK(test_valid_standard_frame_sends_to_can() == 0);
    CHECK(test_valid_extended_frame_maps_flag() == 0);
    CHECK(test_invalid_frames_are_rejected() == 0);
    CHECK(test_can_rx_packs_unified_payload() == 0);
    CHECK(test_offline_drops_old_payload_and_rehandshake_sends_new() == 0);
    return 0;
}
