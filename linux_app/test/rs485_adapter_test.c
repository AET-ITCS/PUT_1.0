#include "rs485_adapter.h"

#include <linux/serial.h>
#include <stdio.h>
#include <string.h>

#include "anymsg_frame.h"
#include "shared_memory_ipc.h"

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                          #condition);                                              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static void write_le16(uint8_t bytes[2], uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xFFu);
}

static size_t make_anymsg(uint8_t *buffer,
                          uint16_t payload_length,
                          uint8_t source_first,
                          uint8_t destination_first)
{
    anymsg_header_t *header;
    uint16_t msg_length;

    memset(buffer, 0, PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u);
    msg_length = (uint16_t)(ANYMSG_HEADER_SIZE + payload_length);
    header = (anymsg_header_t *)buffer;
    write_le16(header->msg_length, msg_length);
    header->retries = 1u;
    header->source_cid[0] = source_first;
    header->destination_cid[0] = destination_first;
    write_le16(header->payload_length, payload_length);
    header->type = ANYMSG_TYPE_RESERVED_MIDDLE_MIN;
    for (uint16_t i = 0u; i < payload_length; ++i) {
        buffer[ANYMSG_HEADER_SIZE + i] = (uint8_t)(0x30u + (i & 0x3Fu));
    }

    return msg_length;
}

static int test_default_config_matches_iob_uart4(void)
{
    rs485_adapter_config_t config;

    rs485_adapter_config_set_defaults(&config);
    CHECK(!config.enabled);
    CHECK(strcmp(config.uart_device, "/dev/ttyS4") == 0);
    CHECK(config.baudrate == 115200u);
    CHECK(config.rs485_flags == (uint32_t)(SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND));
    return 0;
}

static int test_decode_accepts_rs485_anymsg(void)
{
    adapter_rx_result_t rx;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    size_t frame_len;

    frame_len = make_anymsg(frame, 4u, 0xC0u, 0x20u);
    CHECK(rs485_adapter.decode_rx(NULL, frame, frame_len, &rx) == 0);
    CHECK(rx.msg_length == frame_len);
    CHECK(rx.payload_length == 4u);
    CHECK(rx.source_cid[0] == 0xC0u);
    CHECK(rx.destination_cid[0] == 0x20u);
    CHECK(rx.type == ANYMSG_TYPE_RESERVED_MIDDLE_MIN);
    return 0;
}

static int test_decode_rejects_invalid_anymsg(void)
{
    adapter_rx_result_t rx;
    anymsg_header_t *header;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    size_t frame_len;

    frame_len = make_anymsg(frame, 4u, 0xC0u, 0x20u);
    header = (anymsg_header_t *)frame;
    CHECK(rs485_adapter.decode_rx(NULL, frame, ANYMSG_HEADER_SIZE - 1u, &rx) != 0);

    frame_len = make_anymsg(frame, 4u, 0xC0u, 0x20u);
    header = (anymsg_header_t *)frame;
    write_le16(header->msg_length, (uint16_t)(frame_len + 1u));
    CHECK(rs485_adapter.decode_rx(NULL, frame, frame_len, &rx) != 0);

    frame_len = make_anymsg(frame, 4u, 0xC0u, 0x20u);
    header = (anymsg_header_t *)frame;
    header->reserved = 1u;
    CHECK(rs485_adapter.decode_rx(NULL, frame, frame_len, &rx) != 0);

    frame_len = make_anymsg(frame, 4u, 0x20u, 0xC0u);
    CHECK(rs485_adapter.decode_rx(NULL, frame, frame_len, &rx) != 0);
    return 0;
}

static int test_encapsulate_strips_anymsg_header(void)
{
    adapter_tx_packet_t packet;
    anymsg_buffer_t msg;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    size_t frame_len;

    frame_len = make_anymsg(frame, 6u, 0x20u, 0xC0u);
    msg.data = frame;
    msg.len = frame_len;
    memset(&packet, 0, sizeof(packet));

    CHECK(rs485_adapter.encapsulate(NULL, &msg, &packet) == 0);
    CHECK(packet.data == frame + ANYMSG_HEADER_SIZE);
    CHECK(packet.len == 6u);
    CHECK(memcmp(packet.data, frame + ANYMSG_HEADER_SIZE, packet.len) == 0);
    return 0;
}

static int test_offline_send_error_is_reported(void)
{
    adapter_tx_error_t tx_error;
    adapter_tx_packet_t packet;
    uint8_t payload[2] = {0x12u, 0x34u};

    packet.data = payload;
    packet.len = sizeof(payload);
    CHECK(rs485_adapter.send(NULL, &packet) != 0);
    CHECK(rs485_adapter.get_tx_error(NULL, &tx_error) == 0);
    CHECK(strcmp(tx_error.stage, "rs485_send_offline") == 0);
    CHECK(tx_error.err == UNIFIED_ERR_IPC_OFFLINE);
    return 0;
}

int main(void)
{
    if (test_default_config_matches_iob_uart4() != 0) {
        return 1;
    }
    if (test_decode_accepts_rs485_anymsg() != 0) {
        return 1;
    }
    if (test_decode_rejects_invalid_anymsg() != 0) {
        return 1;
    }
    if (test_encapsulate_strips_anymsg_header() != 0) {
        return 1;
    }
    if (test_offline_send_error_is_reported() != 0) {
        return 1;
    }

    puts("rs485_adapter_test: OK");
    return 0;
}
