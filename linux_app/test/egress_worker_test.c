#include "egress_manager.h"
#include "egress_worker.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "crc16.h"

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                          #condition);                                              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

typedef struct {
    size_t mtu;
    int send_result;
    uint32_t send_count;
    uint32_t encapsulate_count;
    uint32_t fragment_count;
} mock_adapter_context_t;

static put_shm_region_t g_region;

static void write_le16(uint8_t bytes[2], uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xFFu);
}

static uint16_t test_descriptor_crc(const put_shm_descriptor_t *descriptor)
{
    return unified_crc16_ccitt_false((const uint8_t *)descriptor,
                                     offsetof(put_shm_descriptor_t, descriptor_crc16));
}

static uint16_t test_reclaim_crc(const put_shm_reclaim_descriptor_t *descriptor)
{
    return unified_crc16_ccitt_false((const uint8_t *)descriptor,
                                     offsetof(put_shm_reclaim_descriptor_t, descriptor_crc16));
}

static size_t make_anymsg(uint8_t *buffer,
                          uint16_t payload_length,
                          uint8_t source_first,
                          uint8_t destination_first)
{
    anymsg_header_t *header;
    uint16_t msg_length;

    memset(buffer, 0, PUT_SHM_FRAME_POOL_BLOCK_SIZE);
    msg_length = (uint16_t)(ANYMSG_HEADER_SIZE + payload_length);
    header = (anymsg_header_t *)buffer;
    write_le16(header->msg_length, msg_length);
    header->retries = 1u;
    header->source_cid[0] = source_first;
    header->destination_cid[0] = destination_first;
    write_le16(header->payload_length, payload_length);
    header->type = ANYMSG_TYPE_RAW_CAN;
    for (uint16_t i = 0u; i < payload_length; ++i) {
        buffer[ANYMSG_HEADER_SIZE + i] = (uint8_t)(0x50u + i);
    }
    return msg_length;
}

static int setup_ipc(linux_shm_ipc_t *ipc)
{
    linux_shm_ipc_init(ipc);
    return (linux_shm_ipc_format_region(ipc, &g_region, 77u, 0u, NULL) == UNIFIED_OK) ?
        0 : 1;
}

static void publish_tx_direct(put_shm_descriptor_ring_t *ring,
                              put_shm_pending_line_t *pending,
                              const put_shm_descriptor_t *descriptor)
{
    put_shm_descriptor_t copy;
    uint32_t index;

    copy = *descriptor;
    copy.descriptor_crc16 = test_descriptor_crc(&copy);
    index = ring->producer.write_seq % ring->producer.depth;
    ring->descriptors[index] = copy;
    ring->producer.write_seq++;
    ring->producer.enqueue_count++;
    pending->bits |= (uint32_t)(1u << ring->header.interface_id);
}

static void publish_reclaim_direct(put_shm_region_t *region,
                                   const put_shm_reclaim_descriptor_t *descriptor)
{
    put_shm_reclaim_descriptor_t copy;
    uint32_t index;

    copy = *descriptor;
    copy.descriptor_crc16 = test_reclaim_crc(&copy);
    index = region->reclaim_ring.producer.write_seq % region->reclaim_ring.producer.depth;
    region->reclaim_ring.descriptors[index] = copy;
    region->reclaim_ring.producer.write_seq++;
    region->reclaim_ring.producer.enqueue_count++;
    region->reclaim_pending.bits = 1u;
}

static int prepare_tx_frame(linux_shm_ipc_t *ipc,
                            put_shm_interface_t source_interface,
                            put_shm_interface_t target_interface,
                            uint8_t source_first,
                            uint8_t destination_first,
                            uint16_t payload_length,
                            uint32_t *out_frame_id,
                            put_shm_descriptor_t *out_descriptor)
{
    uint32_t frame_id;
    uint8_t *buffer;
    uint16_t capacity;
    size_t frame_len;
    anymsg_header_t *header;

    CHECK(linux_shm_frame_alloc(ipc, source_interface, &frame_id, &buffer, &capacity) ==
          UNIFIED_OK);
    CHECK(capacity == PUT_SHM_FRAME_POOL_BLOCK_SIZE);
    frame_len = make_anymsg(buffer, payload_length, source_first, destination_first);
    header = (anymsg_header_t *)buffer;

    ipc->frames[frame_id].state = LINUX_SHM_FRAME_STATE_RX_QUEUED;
    ipc->frames[frame_id].target_interface = (uint8_t)target_interface;

    memset(out_descriptor, 0, sizeof(*out_descriptor));
    out_descriptor->frame_id = frame_id;
    out_descriptor->frame_offset = frame_id * PUT_SHM_FRAME_POOL_BLOCK_SIZE;
    out_descriptor->frame_length = (uint16_t)frame_len;
    out_descriptor->source_interface = (uint8_t)source_interface;
    out_descriptor->target_interface = (uint8_t)target_interface;
    memcpy(out_descriptor->source_cid, header->source_cid, ANYMSG_CID_LENGTH);
    memcpy(out_descriptor->destination_cid, header->destination_cid, ANYMSG_CID_LENGTH);
    out_descriptor->type = header->type;
    out_descriptor->priority = 2u;
    out_descriptor->ttl = 8u;
    out_descriptor->epoch = 77u;

    *out_frame_id = frame_id;
    return 0;
}

static size_t mock_get_mtu(void *ctx)
{
    mock_adapter_context_t *mock = (mock_adapter_context_t *)ctx;
    return (mock == 0 || mock->mtu == 0u) ? PUT_SHM_FRAME_POOL_BLOCK_SIZE : mock->mtu;
}

static int mock_encapsulate(void *ctx,
                            const anymsg_buffer_t *msg,
                            adapter_tx_packet_t *out_packet)
{
    mock_adapter_context_t *mock = (mock_adapter_context_t *)ctx;

    if ((msg == 0) || (msg->data == 0) || (out_packet == 0)) {
        return -1;
    }

    if (mock != 0) {
        mock->encapsulate_count++;
    }
    out_packet->data = msg->data;
    out_packet->len = msg->len;
    return 0;
}

static int mock_fragment_tx(void *ctx,
                            const anymsg_buffer_t *msg,
                            adapter_tx_packet_list_t *out_packets)
{
    static adapter_tx_packet_t packet;
    mock_adapter_context_t *mock = (mock_adapter_context_t *)ctx;

    if ((msg == 0) || (msg->data == 0) || (out_packets == 0)) {
        return -1;
    }

    if (mock != 0) {
        mock->fragment_count++;
    }
    packet.data = msg->data;
    packet.len = msg->len;
    out_packets->packets = &packet;
    out_packets->count = 1u;
    return 0;
}

static int mock_send(void *ctx, const adapter_tx_packet_t *packet)
{
    mock_adapter_context_t *mock = (mock_adapter_context_t *)ctx;

    if ((mock == 0) || (packet == 0) || (packet->data == 0) || (packet->len == 0u)) {
        return -1;
    }

    mock->send_count++;
    return mock->send_result;
}

static physical_interface_adapter_t g_mock_adapter = {
    "mock",
    (uint8_t)PUT_SHM_INTERFACE_ETHERNET,
    mock_get_mtu,
    0,
    0,
    mock_encapsulate,
    mock_fragment_tx,
    mock_send,
    0,
};

static void setup_worker_context(egress_worker_context_t *ctx,
                                 linux_shm_ipc_t *ipc,
                                 status_collector_t *collector,
                                 mock_adapter_context_t *mock,
                                 bool enabled)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->interface_id = PUT_SHM_INTERFACE_ETHERNET;
    ctx->status_module = STATUS_MODULE_ETHERNET;
    ctx->adapter = &g_mock_adapter;
    ctx->adapter_ctx = mock;
    ctx->ipc = ipc;
    ctx->collector = collector;
    ctx->linux_epoch = 77u;
    ctx->enabled = enabled;
    ctx->budget = 16u;
}

static int test_legal_tx_descriptor_sends_and_releases(void)
{
    linux_shm_ipc_t ipc;
    status_collector_t collector;
    egress_worker_context_t ctx;
    mock_adapter_context_t mock;
    put_shm_descriptor_t descriptor;
    uint32_t frame_id;
    egress_worker_drain_result_t result;

    CHECK(setup_ipc(&ipc) == 0);
    status_collector_init(&collector, NULL, false);
    status_collector_configure_module(&collector, STATUS_MODULE_ETHERNET,
                                      true, true, "test", "test");
    memset(&mock, 0, sizeof(mock));
    setup_worker_context(&ctx, &ipc, &collector, &mock, true);
    CHECK(prepare_tx_frame(&ipc,
                           PUT_SHM_INTERFACE_CAN,
                           PUT_SHM_INTERFACE_ETHERNET,
                           0x20u,
                           0x40u,
                           4u,
                           &frame_id,
                           &descriptor) == 0);
    publish_tx_direct(&g_region.tx_rings[PUT_SHM_INTERFACE_ETHERNET],
                      &g_region.tx_pending_bitmap,
                      &descriptor);

    CHECK(egress_worker_drain_once(&ctx, &result) == UNIFIED_OK);
    CHECK(result.dequeued == 1u);
    CHECK(result.tx_ok == 1u);
    CHECK(mock.send_count == 1u);
    CHECK(collector.modules[STATUS_MODULE_ETHERNET].tx_frames == 1u);
    CHECK(collector.modules[STATUS_MODULE_ETHERNET].tx_bytes == descriptor.frame_length);
    CHECK((ipc.allocation_bitmap & (uint64_t)(1ULL << frame_id)) == 0u);
    status_collector_destroy(&collector);
    return 0;
}

static int test_empty_tx_ring_is_normal(void)
{
    linux_shm_ipc_t ipc;
    egress_worker_context_t ctx;
    mock_adapter_context_t mock;
    egress_worker_drain_result_t result;

    CHECK(setup_ipc(&ipc) == 0);
    memset(&mock, 0, sizeof(mock));
    setup_worker_context(&ctx, &ipc, 0, &mock, true);
    CHECK(egress_worker_drain_once(&ctx, &result) == UNIFIED_OK);
    CHECK(result.dequeued == 0u);
    CHECK(result.ipc_errors == 0u);
    return 0;
}

static int test_invalid_anymsg_is_released_without_send(void)
{
    linux_shm_ipc_t ipc;
    status_collector_t collector;
    egress_worker_context_t ctx;
    mock_adapter_context_t mock;
    put_shm_descriptor_t descriptor;
    uint32_t frame_id;
    egress_worker_drain_result_t result;

    CHECK(setup_ipc(&ipc) == 0);
    status_collector_init(&collector, NULL, false);
    status_collector_configure_module(&collector, STATUS_MODULE_ETHERNET,
                                      true, true, "test", "test");
    memset(&mock, 0, sizeof(mock));
    setup_worker_context(&ctx, &ipc, &collector, &mock, true);
    CHECK(prepare_tx_frame(&ipc,
                           PUT_SHM_INTERFACE_CAN,
                           PUT_SHM_INTERFACE_ETHERNET,
                           0x20u,
                           0x40u,
                           0u,
                           &frame_id,
                           &descriptor) == 0);
    g_region.frame_pool[frame_id].bytes[ANYMSG_OFFSET_RESERVED] = 1u;
    publish_tx_direct(&g_region.tx_rings[PUT_SHM_INTERFACE_ETHERNET],
                      &g_region.tx_pending_bitmap,
                      &descriptor);

    CHECK(egress_worker_drain_once(&ctx, &result) == UNIFIED_OK);
    CHECK(result.validation_failed == 1u);
    CHECK(mock.send_count == 0u);
    CHECK(collector.modules[STATUS_MODULE_ETHERNET].error_count == 1u);
    CHECK((ipc.allocation_bitmap & (uint64_t)(1ULL << frame_id)) == 0u);
    status_collector_destroy(&collector);
    return 0;
}

static int test_send_failure_releases_frame(void)
{
    linux_shm_ipc_t ipc;
    status_collector_t collector;
    egress_worker_context_t ctx;
    mock_adapter_context_t mock;
    put_shm_descriptor_t descriptor;
    uint32_t frame_id;
    egress_worker_drain_result_t result;

    CHECK(setup_ipc(&ipc) == 0);
    status_collector_init(&collector, NULL, false);
    status_collector_configure_module(&collector, STATUS_MODULE_ETHERNET,
                                      true, true, "test", "test");
    memset(&mock, 0, sizeof(mock));
    mock.send_result = -1;
    setup_worker_context(&ctx, &ipc, &collector, &mock, true);
    CHECK(prepare_tx_frame(&ipc,
                           PUT_SHM_INTERFACE_CAN,
                           PUT_SHM_INTERFACE_ETHERNET,
                           0x20u,
                           0x40u,
                           0u,
                           &frame_id,
                           &descriptor) == 0);
    publish_tx_direct(&g_region.tx_rings[PUT_SHM_INTERFACE_ETHERNET],
                      &g_region.tx_pending_bitmap,
                      &descriptor);

    CHECK(egress_worker_drain_once(&ctx, &result) == UNIFIED_OK);
    CHECK(result.tx_failed == 1u);
    CHECK(mock.send_count == 1u);
    CHECK(collector.modules[STATUS_MODULE_ETHERNET].send_fail_count == 1u);
    CHECK((ipc.allocation_bitmap & (uint64_t)(1ULL << frame_id)) == 0u);
    status_collector_destroy(&collector);
    return 0;
}

static int test_disabled_and_missing_adapter_release_frame(void)
{
    linux_shm_ipc_t ipc;
    status_collector_t collector;
    egress_worker_context_t ctx;
    mock_adapter_context_t mock;
    put_shm_descriptor_t descriptor;
    uint32_t frame_id;
    egress_worker_drain_result_t result;

    CHECK(setup_ipc(&ipc) == 0);
    status_collector_init(&collector, NULL, false);
    status_collector_configure_module(&collector, STATUS_MODULE_ETHERNET,
                                      true, true, "test", "test");
    memset(&mock, 0, sizeof(mock));
    setup_worker_context(&ctx, &ipc, &collector, &mock, false);
    CHECK(prepare_tx_frame(&ipc,
                           PUT_SHM_INTERFACE_CAN,
                           PUT_SHM_INTERFACE_ETHERNET,
                           0x20u,
                           0x40u,
                           0u,
                           &frame_id,
                           &descriptor) == 0);
    publish_tx_direct(&g_region.tx_rings[PUT_SHM_INTERFACE_ETHERNET],
                      &g_region.tx_pending_bitmap,
                      &descriptor);
    CHECK(egress_worker_drain_once(&ctx, &result) == UNIFIED_OK);
    CHECK(result.tx_failed == 1u);
    CHECK((ipc.allocation_bitmap & (uint64_t)(1ULL << frame_id)) == 0u);

    CHECK(prepare_tx_frame(&ipc,
                           PUT_SHM_INTERFACE_CAN,
                           PUT_SHM_INTERFACE_ETHERNET,
                           0x20u,
                           0x40u,
                           0u,
                           &frame_id,
                           &descriptor) == 0);
    ctx.enabled = true;
    ctx.adapter = 0;
    publish_tx_direct(&g_region.tx_rings[PUT_SHM_INTERFACE_ETHERNET],
                      &g_region.tx_pending_bitmap,
                      &descriptor);
    CHECK(egress_worker_drain_once(&ctx, &result) == UNIFIED_OK);
    CHECK(result.tx_failed == 1u);
    CHECK((ipc.allocation_bitmap & (uint64_t)(1ULL << frame_id)) == 0u);
    CHECK(collector.modules[STATUS_MODULE_ETHERNET].interface_offline_count == 2u);
    status_collector_destroy(&collector);
    return 0;
}

static int test_reclaim_drain_releases_frame(void)
{
    linux_shm_ipc_t ipc;
    uint32_t frame_id;
    uint8_t *buffer;
    uint16_t capacity;
    put_shm_reclaim_descriptor_t reclaim;
    uint32_t reclaim_count;
    linux_shm_ipc_stats_t stats;

    CHECK(setup_ipc(&ipc) == 0);
    CHECK(linux_shm_frame_alloc(&ipc,
                                PUT_SHM_INTERFACE_CAN,
                                &frame_id,
                                &buffer,
                                &capacity) == UNIFIED_OK);
    (void)capacity;
    (void)make_anymsg(buffer, 0u, 0x20u, 0x40u);
    ipc.frames[frame_id].state = LINUX_SHM_FRAME_STATE_RX_QUEUED;
    ipc.frames[frame_id].target_interface = (uint8_t)PUT_SHM_INTERFACE_ETHERNET;

    memset(&reclaim, 0, sizeof(reclaim));
    reclaim.frame_id = frame_id;
    reclaim.reason = PUT_SHM_RECLAIM_REASON_NO_ROUTE;
    reclaim.source_interface = (uint8_t)PUT_SHM_INTERFACE_CAN;
    reclaim.target_interface = (uint8_t)PUT_SHM_INTERFACE_ETHERNET;
    reclaim.epoch = 77u;
    publish_reclaim_direct(&g_region, &reclaim);

    CHECK(egress_manager_drain_reclaim_once(&ipc, 0, &reclaim_count) == UNIFIED_OK);
    CHECK(reclaim_count == 1u);
    CHECK((ipc.allocation_bitmap & (uint64_t)(1ULL << frame_id)) == 0u);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.reclaim_ack_count == 1u);
    CHECK(stats.reclaim_reason_count[PUT_SHM_RECLAIM_REASON_NO_ROUTE] == 1u);
    return 0;
}

int main(void)
{
    CHECK(test_legal_tx_descriptor_sends_and_releases() == 0);
    CHECK(test_empty_tx_ring_is_normal() == 0);
    CHECK(test_invalid_anymsg_is_released_without_send() == 0);
    CHECK(test_send_failure_releases_frame() == 0);
    CHECK(test_disabled_and_missing_adapter_release_frame() == 0);
    CHECK(test_reclaim_drain_releases_frame() == 0);
    puts("egress_worker_test: OK");
    return 0;
}
