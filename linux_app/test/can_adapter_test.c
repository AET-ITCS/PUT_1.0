#include "can_adapter.h"

#include <stdio.h>
#include <string.h>

#include "anymsg_frame.h"
#include "crc16.h"
#include "shared_memory_ipc.h"

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                          #condition);                                              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define TEST_MAX_CAN_FRAGMENTS \
    (1u + ((PUT_SHM_FRAME_POOL_BLOCK_SIZE + CAN_ADAPTER_DATA_PAYLOAD_SIZE - 1u) / \
           CAN_ADAPTER_DATA_PAYLOAD_SIZE))

static put_shm_region_t g_region;

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
    header->destination_cid[0] = destination_first;
    header->source_cid[0] = source_first;
    write_le16(header->payload_length, payload_length);
    header->type = ANYMSG_TYPE_RAW_CAN;
    for (uint16_t i = 0u; i < payload_length; ++i) {
        buffer[ANYMSG_HEADER_SIZE + i] = (uint8_t)(0x30u + (i & 0x3Fu));
    }
    return msg_length;
}

static int setup_ipc(linux_shm_ipc_t *ipc, uint32_t linux_epoch)
{
    linux_shm_ipc_init(ipc);
    return (linux_shm_ipc_format_region(ipc, &g_region, linux_epoch, 0u, NULL) == UNIFIED_OK) ?
        0 : 1;
}

static void build_can_fragments(const uint8_t *msg,
                                size_t msg_len,
                                canid_t can_id,
                                uint8_t session_id,
                                bool corrupt_crc,
                                struct can_frame *frames,
                                size_t *out_count)
{
    uint8_t data_frame_count;
    uint16_t crc;

    data_frame_count = (uint8_t)((msg_len + CAN_ADAPTER_DATA_PAYLOAD_SIZE - 1u) /
                                 CAN_ADAPTER_DATA_PAYLOAD_SIZE);
    crc = unified_crc16_ccitt_false(msg, msg_len);
    if (corrupt_crc) {
        crc = (uint16_t)(crc ^ 0xFFFFu);
    }

    memset(frames, 0, sizeof(struct can_frame) * TEST_MAX_CAN_FRAGMENTS);
    frames[0].can_id = can_id;
    frames[0].can_dlc = CAN_ADAPTER_CLASSIC_DLC;
    frames[0].data[0] = CAN_ADAPTER_FRAME_KIND_SOF;
    frames[0].data[1] = session_id;
    write_le16(&frames[0].data[2], (uint16_t)msg_len);
    write_le16(&frames[0].data[4], crc);
    frames[0].data[6] = ((msg_len % CAN_ADAPTER_DATA_PAYLOAD_SIZE) == 0u) ? 0u : 1u;
    frames[0].data[7] = data_frame_count;

    for (uint8_t seq = 0u; seq < data_frame_count; ++seq) {
        size_t offset;
        size_t copy_len;
        struct can_frame *frame;

        offset = (size_t)seq * CAN_ADAPTER_DATA_PAYLOAD_SIZE;
        copy_len = msg_len - offset;
        if (copy_len > CAN_ADAPTER_DATA_PAYLOAD_SIZE) {
            copy_len = CAN_ADAPTER_DATA_PAYLOAD_SIZE;
        }
        frame = &frames[(size_t)seq + 1u];
        frame->can_id = can_id;
        frame->can_dlc = CAN_ADAPTER_CLASSIC_DLC;
        frame->data[0] = CAN_ADAPTER_FRAME_KIND_DATA;
        frame->data[1] = session_id;
        frame->data[2] = seq;
        memcpy(&frame->data[3], msg + offset, copy_len);
    }

    *out_count = (size_t)data_frame_count + 1u;
}

static int test_decode_valid_and_invalid_anymsg(void)
{
    adapter_rx_result_t rx;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    size_t frame_len;
    anymsg_header_t *header;

    frame_len = make_anymsg(frame, 0u, 0x20u, 0x40u);
    CHECK(can_adapter_decode_anymsg(frame, frame_len, &rx) == UNIFIED_OK);
    CHECK(rx.msg_length == ANYMSG_HEADER_SIZE);
    CHECK(rx.payload_length == 0u);
    CHECK(rx.source_cid[0] == 0x20u);
    CHECK(rx.destination_cid[0] == 0x40u);

    frame_len = make_anymsg(frame,
                            (uint16_t)(PUT_SHM_FRAME_POOL_BLOCK_SIZE - ANYMSG_HEADER_SIZE),
                            0x3Fu,
                            0xC0u);
    CHECK(can_adapter_decode_anymsg(frame, frame_len, &rx) == UNIFIED_OK);
    CHECK(rx.msg_length == PUT_SHM_FRAME_POOL_BLOCK_SIZE);

    frame_len = make_anymsg(frame, 4u, 0x20u, 0x40u);
    header = (anymsg_header_t *)frame;
    CHECK(can_adapter_decode_anymsg(frame, ANYMSG_HEADER_SIZE - 1u, &rx) != UNIFIED_OK);
    CHECK(can_adapter_decode_anymsg(frame, PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u, &rx) !=
          UNIFIED_OK);

    write_le16(header->msg_length, (uint16_t)(frame_len + 1u));
    CHECK(can_adapter_decode_anymsg(frame, frame_len, &rx) != UNIFIED_OK);

    frame_len = make_anymsg(frame, 4u, 0x20u, 0x40u);
    header = (anymsg_header_t *)frame;
    write_le16(header->payload_length, 3u);
    CHECK(can_adapter_decode_anymsg(frame, frame_len, &rx) != UNIFIED_OK);

    frame_len = make_anymsg(frame, 4u, 0x20u, 0x40u);
    header = (anymsg_header_t *)frame;
    header->reserved = 1u;
    CHECK(can_adapter_decode_anymsg(frame, frame_len, &rx) != UNIFIED_OK);

    frame_len = make_anymsg(frame, 4u, 0x40u, 0x20u);
    CHECK(can_adapter_decode_anymsg(frame, frame_len, &rx) != UNIFIED_OK);
    return 0;
}

static int test_ipc_route_hints_enter_can_rx_ring(void)
{
    linux_shm_ipc_t ipc;
    put_shm_descriptor_ring_t *ring;
    adapter_rx_result_t rx;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    uint8_t destinations[8] = {0x20u, 0x40u, 0x60u, 0x80u, 0xA0u, 0xC0u, 0x00u, 0xE0u};
    put_shm_interface_t expected[8] = {
        PUT_SHM_INTERFACE_CAN,
        PUT_SHM_INTERFACE_ETHERNET,
        PUT_SHM_INTERFACE_WIFI,
        PUT_SHM_INTERFACE_BLUETOOTH,
        PUT_SHM_INTERFACE_4G,
        PUT_SHM_INTERFACE_RS485,
        PUT_SHM_INTERFACE_CAN,
        PUT_SHM_INTERFACE_CAN,
    };
    size_t frame_len;

    CHECK(setup_ipc(&ipc, 1234u) == 0);
    ring = &g_region.rx_rings[PUT_SHM_INTERFACE_CAN];

    for (uint32_t i = 0u; i < 8u; ++i) {
        frame_len = make_anymsg(frame, 4u, 0x20u, destinations[i]);
        frame[ANYMSG_OFFSET_LOCAL_TIME] = (uint8_t)i;
        CHECK(can_adapter_decode_anymsg(frame, frame_len, &rx) == UNIFIED_OK);
        CHECK(can_adapter_submit_to_ipc(&ipc, frame, &rx, 1234u) == UNIFIED_OK);
        CHECK(memcmp(g_region.frame_pool[i].bytes, frame, frame_len) == 0);
        CHECK(ring->descriptors[i].frame_id == i);
        CHECK(ring->descriptors[i].frame_length == (uint16_t)frame_len);
        CHECK(ring->descriptors[i].source_interface == (uint8_t)PUT_SHM_INTERFACE_CAN);
        CHECK(ring->descriptors[i].target_interface == (uint8_t)expected[i]);
        CHECK(ring->descriptors[i].source_cid[0] == 0x20u);
        CHECK(ring->descriptors[i].destination_cid[0] == destinations[i]);
        CHECK(ring->descriptors[i].type == ANYMSG_TYPE_RAW_CAN);
        CHECK(ring->descriptors[i].priority == CAN_ADAPTER_DEFAULT_PRIORITY);
        CHECK(ring->descriptors[i].ttl == CAN_ADAPTER_DEFAULT_TTL);
        CHECK(ring->descriptors[i].epoch == 1234u);
    }

    CHECK(ring->producer.write_seq == 8u);
    CHECK(g_region.rx_pending_bitmap.bits == (uint32_t)(1u << PUT_SHM_INTERFACE_CAN));
    return 0;
}

static int test_ipc_resource_errors_have_no_leak(void)
{
    linux_shm_ipc_t ipc;
    linux_shm_ipc_stats_t stats;
    adapter_rx_result_t rx;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    size_t frame_len;

    CHECK(setup_ipc(&ipc, 2000u) == 0);
    CHECK(linux_shm_ipc_set_interface_quota(&ipc, PUT_SHM_INTERFACE_CAN, 0u) == UNIFIED_OK);
    frame_len = make_anymsg(frame, 0u, 0x20u, 0x40u);
    CHECK(can_adapter_decode_anymsg(frame, frame_len, &rx) == UNIFIED_OK);
    CHECK(can_adapter_submit_to_ipc(&ipc, frame, &rx, 2000u) ==
          UNIFIED_ERR_IPC_FRAME_POOL_FULL);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.frame_pool.used == 0u);
    CHECK(stats.frame_pool.full_count == 1u);
    CHECK(stats.rx_rings[PUT_SHM_INTERFACE_CAN].used == 0u);

    CHECK(setup_ipc(&ipc, 2001u) == 0);
    frame_len = make_anymsg(frame, 0u, 0x20u, 0x40u);
    CHECK(can_adapter_decode_anymsg(frame, frame_len, &rx) == UNIFIED_OK);
    for (uint32_t i = 0u; i < PUT_SHM_DESCRIPTOR_RING_DEPTH; ++i) {
        frame[ANYMSG_OFFSET_LOCAL_TIME] = (uint8_t)i;
        CHECK(can_adapter_submit_to_ipc(&ipc, frame, &rx, 2001u) == UNIFIED_OK);
    }
    CHECK(can_adapter_submit_to_ipc(&ipc, frame, &rx, 2001u) == UNIFIED_ERR_IPC_QUEUE_FULL);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.frame_pool.used == PUT_SHM_DESCRIPTOR_RING_DEPTH);
    CHECK(stats.frame_pool.allocated == PUT_SHM_DESCRIPTOR_RING_DEPTH + 1u);
    CHECK(stats.frame_pool.released == 1u);
    CHECK(stats.rx_rings[PUT_SHM_INTERFACE_CAN].used == PUT_SHM_DESCRIPTOR_RING_DEPTH);
    CHECK(stats.rx_rings[PUT_SHM_INTERFACE_CAN].full_count == 1u);
    return 0;
}

static int test_reassembly_ordering_and_duplicates(void)
{
    can_reassembly_context_t ctx;
    can_status_t status;
    anymsg_buffer_t out;
    uint8_t msg[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    struct can_frame frames[TEST_MAX_CAN_FRAGMENTS];
    size_t msg_len;
    size_t frame_count;

    memset(&status, 0, sizeof(status));
    msg_len = make_anymsg(msg, 7u, 0x20u, 0x40u);
    build_can_fragments(msg, msg_len, 0x320u, 1u, false, frames, &frame_count);
    can_reassembly_context_init(&ctx, 500u, &status, NULL);
    for (size_t i = 0u; i < frame_count; ++i) {
        CHECK(can_adapter_reassemble_frame(&ctx, &frames[i], 100u + i, &out) == UNIFIED_OK);
    }
    CHECK(out.data != NULL);
    CHECK(out.len == msg_len);
    CHECK(memcmp(out.data, msg, msg_len) == 0);

    memset(&status, 0, sizeof(status));
    build_can_fragments(msg, msg_len, 0x320u, 2u, false, frames, &frame_count);
    can_reassembly_context_init(&ctx, 500u, &status, NULL);
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[0], 200u, &out) == UNIFIED_OK);
    for (size_t i = frame_count - 1u; i > 0u; --i) {
        CHECK(can_adapter_reassemble_frame(&ctx, &frames[i], 200u + i, &out) == UNIFIED_OK);
    }
    CHECK(out.data != NULL);
    CHECK(out.len == msg_len);
    CHECK(memcmp(out.data, msg, msg_len) == 0);

    memset(&status, 0, sizeof(status));
    build_can_fragments(msg, msg_len, 0x320u, 3u, false, frames, &frame_count);
    can_reassembly_context_init(&ctx, 500u, &status, NULL);
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[0], 300u, &out) == UNIFIED_OK);
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[1], 301u, &out) == UNIFIED_OK);
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[1], 302u, &out) == UNIFIED_OK);
    CHECK(status.duplicate_fragment_count == 1u);
    for (size_t i = 2u; i < frame_count; ++i) {
        CHECK(can_adapter_reassemble_frame(&ctx, &frames[i], 300u + i, &out) == UNIFIED_OK);
    }
    CHECK(out.data != NULL);
    CHECK(out.len == msg_len);
    CHECK(memcmp(out.data, msg, msg_len) == 0);
    return 0;
}

static int test_reassembly_output_survives_session_reuse(void)
{
    can_reassembly_context_t ctx;
    can_status_t status;
    anymsg_buffer_t out;
    uint8_t first_msg[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    uint8_t second_msg[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    struct can_frame frames[TEST_MAX_CAN_FRAGMENTS];
    const uint8_t *saved_data;
    size_t first_len;
    size_t second_len;
    size_t frame_count;

    memset(&status, 0, sizeof(status));
    can_reassembly_context_init(&ctx, 500u, &status, NULL);

    first_len = make_anymsg(first_msg, 7u, 0x20u, 0x40u);
    build_can_fragments(first_msg, first_len, 0x320u, 10u, false, frames, &frame_count);
    for (size_t i = 0u; i < frame_count; ++i) {
        CHECK(can_adapter_reassemble_frame(&ctx, &frames[i], 100u + i, &out) == UNIFIED_OK);
    }
    CHECK(out.data != NULL);
    CHECK(out.len == first_len);
    CHECK(memcmp(out.data, first_msg, first_len) == 0);
    saved_data = out.data;

    second_len = make_anymsg(second_msg, 13u, 0x20u, 0x40u);
    build_can_fragments(second_msg, second_len, 0x320u, 11u, false, frames, &frame_count);
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[0], 200u, &out) == UNIFIED_OK);

    CHECK(memcmp(saved_data, first_msg, first_len) == 0);
    return 0;
}

static int test_reassembly_rejects_bad_fragments(void)
{
    can_reassembly_context_t ctx;
    can_status_t status;
    anymsg_buffer_t out;
    uint8_t msg[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    struct can_frame frames[TEST_MAX_CAN_FRAGMENTS];
    struct can_frame bad;
    size_t msg_len;
    size_t frame_count;

    msg_len = make_anymsg(msg, 7u, 0x20u, 0x40u);
    build_can_fragments(msg, msg_len, 0x320u, 4u, false, frames, &frame_count);

    memset(&status, 0, sizeof(status));
    can_reassembly_context_init(&ctx, 500u, &status, NULL);
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[1], 100u, &out) != UNIFIED_OK);
    CHECK(status.orphan_fragment_count == 1u);

    memset(&status, 0, sizeof(status));
    can_reassembly_context_init(&ctx, 500u, &status, NULL);
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[0], 200u, &out) == UNIFIED_OK);
    bad = frames[1];
    bad.data[2] = 0xFFu;
    CHECK(can_adapter_reassemble_frame(&ctx, &bad, 201u, &out) != UNIFIED_OK);

    memset(&status, 0, sizeof(status));
    build_can_fragments(msg, msg_len, 0x320u, 5u, true, frames, &frame_count);
    can_reassembly_context_init(&ctx, 500u, &status, NULL);
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[0], 300u, &out) == UNIFIED_OK);
    for (size_t i = 1u; i + 1u < frame_count; ++i) {
        CHECK(can_adapter_reassemble_frame(&ctx, &frames[i], 300u + i, &out) == UNIFIED_OK);
    }
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[frame_count - 1u], 399u, &out) ==
          UNIFIED_ERR_CRC);
    CHECK(status.crc_error_count == 1u);

    memset(&status, 0, sizeof(status));
    can_reassembly_context_init(&ctx, 500u, &status, NULL);
    bad = frames[0];
    write_le16(&bad.data[2], ANYMSG_HEADER_SIZE - 1u);
    CHECK(can_adapter_reassemble_frame(&ctx, &bad, 400u, &out) != UNIFIED_OK);

    memset(&status, 0, sizeof(status));
    can_reassembly_context_init(&ctx, 500u, &status, NULL);
    bad = frames[0];
    bad.data[7] = (uint8_t)(bad.data[7] + 1u);
    CHECK(can_adapter_reassemble_frame(&ctx, &bad, 500u, &out) != UNIFIED_OK);
    return 0;
}

static int test_reassembly_session_lifecycle(void)
{
    can_reassembly_context_t ctx;
    can_status_t status;
    anymsg_buffer_t out;
    uint8_t msg[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    struct can_frame frames[TEST_MAX_CAN_FRAGMENTS];
    size_t msg_len;
    size_t frame_count;

    msg_len = make_anymsg(msg, 0u, 0x20u, 0x40u);
    build_can_fragments(msg, msg_len, 0x320u, 9u, false, frames, &frame_count);

    memset(&status, 0, sizeof(status));
    can_reassembly_context_init(&ctx, 500u, &status, NULL);
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[0], 100u, &out) == UNIFIED_OK);
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[0], 101u, &out) == UNIFIED_OK);
    CHECK(status.session_conflict_count == 1u);

    memset(&status, 0, sizeof(status));
    can_reassembly_context_init(&ctx, 500u, &status, NULL);
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[0], 200u, &out) == UNIFIED_OK);
    can_adapter_reassembly_scan_timeouts(&ctx, 701u);
    CHECK(status.reassemble_timeout_count == 1u);

    memset(&status, 0, sizeof(status));
    can_reassembly_context_init(&ctx, 500u, &status, NULL);
    for (uint8_t session_id = 0u; session_id < CAN_ADAPTER_REASSEMBLY_MAX_SESSIONS; ++session_id) {
        build_can_fragments(msg, msg_len, 0x320u, session_id, false, frames, &frame_count);
        CHECK(can_adapter_reassemble_frame(&ctx,
                                           &frames[0],
                                           300u + session_id,
                                           &out) == UNIFIED_OK);
    }
    build_can_fragments(msg,
                        msg_len,
                        0x320u,
                        (uint8_t)CAN_ADAPTER_REASSEMBLY_MAX_SESSIONS,
                        false,
                        frames,
                        &frame_count);
    CHECK(can_adapter_reassemble_frame(&ctx, &frames[0], 400u, &out) == UNIFIED_OK);
    CHECK(status.session_no_buffer_count == 1u);
    return 0;
}

static int test_fragment_tx_generates_sof_and_data_frames(void)
{
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    anymsg_buffer_t msg;
    adapter_tx_packet_t packet;
    adapter_tx_packet_list_t packets;
    can_tx_context_t tx_ctx;
    const struct can_frame *sof;
    const struct can_frame *data0;
    const struct can_frame *data1;
    uint16_t expected_crc;
    size_t frame_len;

    frame_len = make_anymsg(frame, 7u, 0x20u, 0x40u);
    msg.data = frame;
    msg.len = frame_len;
    memset(&packet, 0, sizeof(packet));
    memset(&packets, 0, sizeof(packets));
    can_tx_context_init(&tx_ctx, 0x321u, false);

    CHECK(can_adapter.get_mtu(NULL) == CAN_ADAPTER_DATA_PAYLOAD_SIZE);
    CHECK(can_adapter.encapsulate(&tx_ctx, &msg, &packet) != 0);
    CHECK(can_adapter.fragment_tx(&tx_ctx, &msg, &packets) == 0);
    CHECK(packets.count == (1u + ((frame_len + CAN_ADAPTER_DATA_PAYLOAD_SIZE - 1u) /
                                  CAN_ADAPTER_DATA_PAYLOAD_SIZE)));
    CHECK(packets.count >= 3u);
    CHECK(packets.packets[0].len == sizeof(struct can_frame));
    CHECK(packets.packets[1].len == sizeof(struct can_frame));

    sof = (const struct can_frame *)packets.packets[0].data;
    data0 = (const struct can_frame *)packets.packets[1].data;
    data1 = (const struct can_frame *)packets.packets[2].data;
    expected_crc = unified_crc16_ccitt_false(frame, frame_len);
    CHECK(sof->can_id == 0x321u);
    CHECK(sof->can_dlc == CAN_ADAPTER_CLASSIC_DLC);
    CHECK(sof->data[0] == CAN_ADAPTER_FRAME_KIND_SOF);
    CHECK(sof->data[1] == 0u);
    CHECK((uint16_t)(sof->data[2] | ((uint16_t)sof->data[3] << 8u)) == frame_len);
    CHECK((uint16_t)(sof->data[4] | ((uint16_t)sof->data[5] << 8u)) == expected_crc);
    CHECK(sof->data[6] == (((frame_len % CAN_ADAPTER_DATA_PAYLOAD_SIZE) == 0u) ? 0u : 1u));
    CHECK(sof->data[7] == (uint8_t)((frame_len + CAN_ADAPTER_DATA_PAYLOAD_SIZE - 1u) /
                                    CAN_ADAPTER_DATA_PAYLOAD_SIZE));
    CHECK(data0->data[0] == CAN_ADAPTER_FRAME_KIND_DATA);
    CHECK(data0->data[1] == sof->data[1]);
    CHECK(data0->data[2] == 0u);
    CHECK(memcmp(&data0->data[3], frame, CAN_ADAPTER_DATA_PAYLOAD_SIZE) == 0);
    CHECK(data1->data[0] == CAN_ADAPTER_FRAME_KIND_DATA);
    CHECK(data1->data[1] == sof->data[1]);
    CHECK(data1->data[2] == 1u);
    CHECK(memcmp(&data1->data[3],
                 frame + CAN_ADAPTER_DATA_PAYLOAD_SIZE,
                 CAN_ADAPTER_DATA_PAYLOAD_SIZE) == 0);
    CHECK(can_adapter.send(&tx_ctx, &packets.packets[0]) != 0);
    return 0;
}

int main(void)
{
    if (test_decode_valid_and_invalid_anymsg() != 0) {
        return 1;
    }
    if (test_ipc_route_hints_enter_can_rx_ring() != 0) {
        return 1;
    }
    if (test_ipc_resource_errors_have_no_leak() != 0) {
        return 1;
    }
    if (test_reassembly_ordering_and_duplicates() != 0) {
        return 1;
    }
    if (test_reassembly_output_survives_session_reuse() != 0) {
        return 1;
    }
    if (test_reassembly_rejects_bad_fragments() != 0) {
        return 1;
    }
    if (test_reassembly_session_lifecycle() != 0) {
        return 1;
    }
    if (test_fragment_tx_generates_sof_and_data_frames() != 0) {
        return 1;
    }

    puts("can_adapter_test: OK");
    return 0;
}
