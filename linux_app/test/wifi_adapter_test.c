#include "wifi_adapter.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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
        buffer[ANYMSG_HEADER_SIZE + i] = (uint8_t)(0xA0u + (i & 0x0Fu));
    }
    return msg_length;
}

static int setup_ipc(linux_shm_ipc_t *ipc, uint32_t linux_epoch)
{
    linux_shm_ipc_init(ipc);
    return (linux_shm_ipc_format_region(ipc, &g_region, linux_epoch, 0u, NULL) == UNIFIED_OK) ? 0 : 1;
}

static int test_valid_udp_anymsg_enters_wifi_rx_ring(void)
{
    linux_shm_ipc_t ipc;
    wifi_rx_context_t ctx;
    linux_shm_ipc_stats_t stats;
    put_shm_descriptor_ring_t *ring;
    const put_shm_descriptor_t *descriptor;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    size_t frame_len;

    CHECK(setup_ipc(&ipc, 1234u) == 0);
    frame_len = make_anymsg(frame, 4u, 0x60u, 0x20u);

    memset(&ctx, 0, sizeof(ctx));
    ctx.ipc = &ipc;
    ctx.linux_epoch = 1234u;

    CHECK(wifi_adapter_handle_datagram(&ctx, frame, frame_len) == UNIFIED_OK);
    CHECK(memcmp(g_region.frame_pool[0].bytes, frame, frame_len) == 0);

    ring = &g_region.rx_rings[PUT_SHM_INTERFACE_WIFI];
    CHECK(ring->producer.write_seq == 1u);
    CHECK(g_region.rx_pending_bitmap.bits == (uint32_t)(1u << PUT_SHM_INTERFACE_WIFI));
    descriptor = &ring->descriptors[0];
    CHECK(descriptor->frame_id == 0u);
    CHECK(descriptor->frame_length == (uint16_t)frame_len);
    CHECK(descriptor->source_interface == (uint8_t)PUT_SHM_INTERFACE_WIFI);
    CHECK(descriptor->target_interface == (uint8_t)PUT_SHM_INTERFACE_CAN);
    CHECK(descriptor->source_cid[0] == 0x60u);
    CHECK(descriptor->destination_cid[0] == 0x20u);
    CHECK(descriptor->type == ANYMSG_TYPE_RAW_CAN);
    CHECK(descriptor->priority == WIFI_ADAPTER_DEFAULT_PRIORITY);
    CHECK(descriptor->ttl == WIFI_ADAPTER_DEFAULT_TTL);
    CHECK(descriptor->epoch == 1234u);
    CHECK((descriptor->flags & PUT_SHM_DESCRIPTOR_TRUST_FLAG_MASK) == 0u);

    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.frame_pool.used == 1u);
    CHECK(stats.frame_pool.high_watermark == 1u);
    CHECK(stats.rx_rings[PUT_SHM_INTERFACE_WIFI].used == 1u);
    return 0;
}

static int test_explicit_wifi_trust_flags_enter_descriptor(void)
{
    linux_shm_ipc_t ipc;
    adapter_rx_result_t rx;
    put_shm_descriptor_ring_t *ring;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    size_t frame_len;
    uint32_t trust_flags;

    CHECK(setup_ipc(&ipc, 1235u) == 0);
    frame_len = make_anymsg(frame, 4u, 0x60u, 0x20u);
    CHECK(wifi_adapter_decode_datagram(frame, frame_len, &rx) == UNIFIED_OK);
    trust_flags = PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK |
                  PUT_SHM_DESCRIPTOR_FLAG_INTEGRITY_OK |
                  PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK |
                  PUT_SHM_DESCRIPTOR_FLAG_CONTROL_ALLOWED;
    rx.trust_flags = trust_flags;

    CHECK(wifi_adapter_submit_to_ipc(&ipc, frame, &rx, 1235u) == UNIFIED_OK);
    ring = &g_region.rx_rings[PUT_SHM_INTERFACE_WIFI];
    CHECK((ring->descriptors[0].flags & PUT_SHM_DESCRIPTOR_TRUST_FLAG_MASK) ==
          trust_flags);
    return 0;
}

static int test_decode_rejects_invalid_udp_anymsg(void)
{
    adapter_rx_result_t rx;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    size_t frame_len;
    anymsg_header_t *header;

    frame_len = make_anymsg(frame, 4u, 0x60u, 0x20u);
    header = (anymsg_header_t *)frame;

    CHECK(wifi_adapter_decode_datagram(frame, ANYMSG_HEADER_SIZE - 1u, &rx) != UNIFIED_OK);
    CHECK(wifi_adapter_decode_datagram(frame, PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u, &rx) != UNIFIED_OK);

    frame_len = make_anymsg(frame, 4u, 0x60u, 0x20u);
    write_le16(header->msg_length, (uint16_t)(frame_len + 1u));
    CHECK(wifi_adapter_decode_datagram(frame, frame_len, &rx) != UNIFIED_OK);

    frame_len = make_anymsg(frame, 4u, 0x60u, 0x20u);
    write_le16(header->payload_length, 3u);
    CHECK(wifi_adapter_decode_datagram(frame, frame_len, &rx) != UNIFIED_OK);

    frame_len = make_anymsg(frame, 4u, 0x60u, 0x20u);
    header->reserved = 1u;
    CHECK(wifi_adapter_decode_datagram(frame, frame_len, &rx) != UNIFIED_OK);

    frame_len = make_anymsg(frame, 4u, 0x40u, 0x60u);
    CHECK(wifi_adapter_decode_datagram(frame, frame_len, &rx) != UNIFIED_OK);

    return 0;
}

static int test_decode_errors_are_counted(void)
{
    linux_shm_ipc_t ipc;
    status_collector_t collector;
    wifi_rx_context_t ctx;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    size_t frame_len;

    CHECK(setup_ipc(&ipc, 2000u) == 0);
    status_collector_init(&collector, NULL, false);
    status_collector_configure_module(&collector,
                                      STATUS_MODULE_WIFI,
                                      true,
                                      true,
                                      "Wi-Fi UDP/TCP raw",
                                      "test");
    memset(&ctx, 0, sizeof(ctx));
    ctx.ipc = &ipc;
    ctx.collector = &collector;
    ctx.linux_epoch = 2000u;

    frame_len = make_anymsg(frame, 4u, 0x40u, 0x60u);
    CHECK(wifi_adapter_handle_datagram(&ctx, frame, frame_len) != UNIFIED_OK);
    CHECK(collector.modules[STATUS_MODULE_WIFI].decode_error_count == 1u);
    CHECK(collector.modules[STATUS_MODULE_WIFI].error_count == 1u);
    status_collector_destroy(&collector);
    return 0;
}

static int test_frame_pool_full_has_no_leak(void)
{
    linux_shm_ipc_t ipc;
    wifi_rx_context_t ctx;
    linux_shm_ipc_stats_t stats;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    size_t frame_len;

    CHECK(setup_ipc(&ipc, 3000u) == 0);
    CHECK(linux_shm_ipc_set_interface_quota(&ipc, PUT_SHM_INTERFACE_WIFI, 0u) == UNIFIED_OK);
    frame_len = make_anymsg(frame, 0u, 0x60u, 0x20u);

    memset(&ctx, 0, sizeof(ctx));
    ctx.ipc = &ipc;
    ctx.linux_epoch = 3000u;

    CHECK(wifi_adapter_handle_datagram(&ctx, frame, frame_len) == UNIFIED_ERR_IPC_FRAME_POOL_FULL);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.frame_pool.used == 0u);
    CHECK(stats.frame_pool.full_count == 1u);
    CHECK(stats.rx_rings[PUT_SHM_INTERFACE_WIFI].used == 0u);
    return 0;
}

static int test_rx_ring_full_releases_unpublished_frame(void)
{
    linux_shm_ipc_t ipc;
    wifi_rx_context_t ctx;
    linux_shm_ipc_stats_t stats;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    size_t frame_len;

    CHECK(setup_ipc(&ipc, 4000u) == 0);
    frame_len = make_anymsg(frame, 0u, 0x60u, 0x20u);
    memset(&ctx, 0, sizeof(ctx));
    ctx.ipc = &ipc;
    ctx.linux_epoch = 4000u;

    for (uint32_t i = 0u; i < PUT_SHM_DESCRIPTOR_RING_DEPTH; ++i) {
        frame[ANYMSG_OFFSET_LOCAL_TIME] = (uint8_t)i;
        CHECK(wifi_adapter_handle_datagram(&ctx, frame, frame_len) == UNIFIED_OK);
    }

    CHECK(wifi_adapter_handle_datagram(&ctx, frame, frame_len) == UNIFIED_ERR_IPC_QUEUE_FULL);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.frame_pool.used == PUT_SHM_DESCRIPTOR_RING_DEPTH);
    CHECK(stats.frame_pool.allocated == PUT_SHM_DESCRIPTOR_RING_DEPTH + 1u);
    CHECK(stats.frame_pool.released == 1u);
    CHECK(stats.rx_rings[PUT_SHM_INTERFACE_WIFI].used == PUT_SHM_DESCRIPTOR_RING_DEPTH);
    CHECK(stats.rx_rings[PUT_SHM_INTERFACE_WIFI].full_count == 1u);
    return 0;
}


static int test_tcp_stream_partial_frame_enters_wifi_rx_ring(void)
{
    linux_shm_ipc_t ipc;
    wifi_rx_context_t rx_ctx;
    wifi_tcp_stream_context_t stream_ctx;
    linux_shm_ipc_stats_t stats;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    size_t frame_len;

    CHECK(setup_ipc(&ipc, 5000u) == 0);
    frame_len = make_anymsg(frame, 8u, 0x60u, 0x20u);
    memset(&rx_ctx, 0, sizeof(rx_ctx));
    rx_ctx.ipc = &ipc;
    rx_ctx.linux_epoch = 5000u;
    wifi_tcp_stream_init(&stream_ctx, &rx_ctx);

    CHECK(wifi_adapter_handle_tcp_bytes(&stream_ctx, frame, 7u) == UNIFIED_OK);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.frame_pool.used == 0u);
    CHECK(stream_ctx.buffered_len == 7u);

    CHECK(wifi_adapter_handle_tcp_bytes(&stream_ctx, frame + 7u, frame_len - 7u) == UNIFIED_OK);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.frame_pool.used == 1u);
    CHECK(stats.rx_rings[PUT_SHM_INTERFACE_WIFI].used == 1u);
    CHECK(stream_ctx.buffered_len == 0u);
    CHECK(memcmp(g_region.frame_pool[0].bytes, frame, frame_len) == 0);
    return 0;
}

static int test_tcp_stream_multiple_frames_in_one_chunk(void)
{
    linux_shm_ipc_t ipc;
    wifi_rx_context_t rx_ctx;
    wifi_tcp_stream_context_t stream_ctx;
    linux_shm_ipc_stats_t stats;
    uint8_t frame_a[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    uint8_t frame_b[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    uint8_t stream[PUT_SHM_FRAME_POOL_BLOCK_SIZE * 2u];
    size_t len_a;
    size_t len_b;

    CHECK(setup_ipc(&ipc, 5001u) == 0);
    len_a = make_anymsg(frame_a, 0u, 0x60u, 0x20u);
    len_b = make_anymsg(frame_b, 3u, 0x61u, 0x40u);
    memcpy(stream, frame_a, len_a);
    memcpy(stream + len_a, frame_b, len_b);

    memset(&rx_ctx, 0, sizeof(rx_ctx));
    rx_ctx.ipc = &ipc;
    rx_ctx.linux_epoch = 5001u;
    wifi_tcp_stream_init(&stream_ctx, &rx_ctx);

    CHECK(wifi_adapter_handle_tcp_bytes(&stream_ctx, stream, len_a + len_b) == UNIFIED_OK);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.frame_pool.used == 2u);
    CHECK(stats.rx_rings[PUT_SHM_INTERFACE_WIFI].used == 2u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_WIFI].descriptors[0].target_interface ==
          (uint8_t)PUT_SHM_INTERFACE_CAN);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_WIFI].descriptors[1].target_interface ==
          (uint8_t)PUT_SHM_INTERFACE_ETHERNET);
    CHECK(stream_ctx.buffered_len == 0u);
    return 0;
}

static int test_tcp_stream_invalid_length_is_counted(void)
{
    linux_shm_ipc_t ipc;
    status_collector_t collector;
    wifi_rx_context_t rx_ctx;
    wifi_tcp_stream_context_t stream_ctx;
    uint8_t bad_length[2] = {0x01u, 0x00u};

    CHECK(setup_ipc(&ipc, 5002u) == 0);
    status_collector_init(&collector, NULL, false);
    status_collector_configure_module(&collector,
                                      STATUS_MODULE_WIFI,
                                      true,
                                      true,
                                      "Wi-Fi UDP/TCP raw",
                                      "test");
    memset(&rx_ctx, 0, sizeof(rx_ctx));
    rx_ctx.ipc = &ipc;
    rx_ctx.collector = &collector;
    rx_ctx.linux_epoch = 5002u;
    wifi_tcp_stream_init(&stream_ctx, &rx_ctx);

    CHECK(wifi_adapter_handle_tcp_bytes(&stream_ctx, bad_length, sizeof(bad_length)) ==
          UNIFIED_ERR_LENGTH);
    CHECK(stream_ctx.buffered_len == 0u);
    CHECK(collector.modules[STATUS_MODULE_WIFI].decode_error_count == 1u);
    CHECK(collector.modules[STATUS_MODULE_WIFI].error_count == 1u);
    status_collector_destroy(&collector);
    return 0;
}

static bool udp_loopback_is_unavailable(void)
{
    return (errno == EPERM) || (errno == EACCES) || (errno == EAFNOSUPPORT);
}

static int test_udp_tx_uses_default_peer(void)
{
    wifi_tx_context_t tx_ctx;
    anymsg_buffer_t msg;
    adapter_tx_packet_t packet;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    uint8_t received[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    anymsg_header_t *header;
    struct sockaddr_in bind_addr;
    struct timeval timeout;
    socklen_t bind_len;
    ssize_t received_len;
    int recv_fd;
    uint16_t port;
    size_t frame_len;

    recv_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if ((recv_fd < 0) && udp_loopback_is_unavailable()) {
        puts("wifi_adapter_test: SKIP udp tx default peer (AF_INET unavailable)");
        return 0;
    }
    CHECK(recv_fd >= 0);

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    CHECK(setsockopt(recv_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = 0;
    CHECK(inet_pton(AF_INET, "127.0.0.1", &bind_addr.sin_addr) == 1);
    CHECK(bind(recv_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0);
    bind_len = (socklen_t)sizeof(bind_addr);
    CHECK(getsockname(recv_fd, (struct sockaddr *)&bind_addr, &bind_len) == 0);
    port = ntohs(bind_addr.sin_port);

    frame_len = make_anymsg(frame, 4u, 0x60u, 0x60u);
    header = (anymsg_header_t *)frame;
    header->destination_cid[1] = 0x01u;
    msg.data = frame;
    msg.len = frame_len;
    memset(&packet, 0, sizeof(packet));
    wifi_tx_context_init(&tx_ctx, port);
    CHECK(wifi_tx_context_set_default_peer(&tx_ctx, "127.0.0.1", port) == UNIFIED_OK);
    CHECK(wifi_adapter.encapsulate(&tx_ctx, &msg, &packet) == 0);
    CHECK(wifi_adapter.send(&tx_ctx, &packet) == 0);

    received_len = recvfrom(recv_fd, received, sizeof(received), 0, 0, 0);
    CHECK(received_len == (ssize_t)frame_len);
    CHECK(memcmp(received, frame, frame_len) == 0);

    wifi_tx_context_destroy(&tx_ctx);
    (void)close(recv_fd);
    return 0;
}

static int test_udp_tx_uses_configured_destination_peer(void)
{
    wifi_tx_context_t tx_ctx;
    wifi_tx_peer_t peer;
    anymsg_buffer_t msg;
    adapter_tx_packet_t packet;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    uint8_t received[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    anymsg_header_t *header;
    struct sockaddr_in bind_addr;
    struct in_addr peer_addr;
    struct timeval timeout;
    socklen_t bind_len;
    ssize_t received_len;
    int recv_fd;
    uint16_t port;
    size_t frame_len;

    recv_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if ((recv_fd < 0) && udp_loopback_is_unavailable()) {
        puts("wifi_adapter_test: SKIP udp tx configured peer (AF_INET unavailable)");
        return 0;
    }
    CHECK(recv_fd >= 0);

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    CHECK(setsockopt(recv_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = 0;
    CHECK(inet_pton(AF_INET, "127.0.0.1", &bind_addr.sin_addr) == 1);
    CHECK(bind(recv_fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0);
    bind_len = (socklen_t)sizeof(bind_addr);
    CHECK(getsockname(recv_fd, (struct sockaddr *)&bind_addr, &bind_len) == 0);
    port = ntohs(bind_addr.sin_port);

    memset(&peer, 0, sizeof(peer));
    peer.destination_cid[0] = 0x7Fu;
    peer.destination_cid[1] = 0x00u;
    peer.destination_cid[2] = 0x00u;
    peer.destination_cid[3] = 0x01u;
    CHECK(inet_pton(AF_INET, "127.0.0.1", &peer_addr) == 1);
    peer.ipv4_addr_be = peer_addr.s_addr;
    peer.port = port;

    frame_len = make_anymsg(frame, 4u, 0x60u, 0x7Fu);
    header = (anymsg_header_t *)frame;
    memcpy(header->destination_cid, peer.destination_cid, ANYMSG_CID_LENGTH);
    msg.data = frame;
    msg.len = frame_len;
    memset(&packet, 0, sizeof(packet));
    wifi_tx_context_init(&tx_ctx, port);
    CHECK(wifi_tx_context_add_peer(&tx_ctx, &peer) == 0);
    CHECK(wifi_adapter.encapsulate(&tx_ctx, &msg, &packet) == 0);
    CHECK(wifi_adapter.send(&tx_ctx, &packet) == 0);

    received_len = recvfrom(recv_fd, received, sizeof(received), 0, 0, 0);
    CHECK(received_len == (ssize_t)frame_len);
    CHECK(memcmp(received, frame, frame_len) == 0);

    wifi_tx_context_destroy(&tx_ctx);
    (void)close(recv_fd);
    return 0;
}

static int test_udp_tx_fails_without_configured_peer(void)
{
    wifi_tx_context_t tx_ctx;
    anymsg_buffer_t msg;
    adapter_tx_packet_t packet;
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u];
    anymsg_header_t *header;
    size_t frame_len;

    frame_len = make_anymsg(frame, 4u, 0x60u, 0x7Fu);
    header = (anymsg_header_t *)frame;
    header->destination_cid[0] = 0x7Fu;
    header->destination_cid[1] = 0u;
    header->destination_cid[2] = 0u;
    header->destination_cid[3] = 1u;

    msg.data = frame;
    msg.len = frame_len;
    memset(&packet, 0, sizeof(packet));
    wifi_tx_context_init(&tx_ctx, WIFI_ADAPTER_DEFAULT_PORT);
    CHECK(wifi_adapter.encapsulate(&tx_ctx, &msg, &packet) == 0);
    CHECK(wifi_adapter.send(&tx_ctx, &packet) != 0);
    wifi_tx_context_destroy(&tx_ctx);
    return 0;
}

int main(void)
{
    if (test_valid_udp_anymsg_enters_wifi_rx_ring() != 0) {
        return 1;
    }
    if (test_decode_rejects_invalid_udp_anymsg() != 0) {
        return 1;
    }
    if (test_explicit_wifi_trust_flags_enter_descriptor() != 0) {
        return 1;
    }
    if (test_decode_errors_are_counted() != 0) {
        return 1;
    }
    if (test_frame_pool_full_has_no_leak() != 0) {
        return 1;
    }
    if (test_rx_ring_full_releases_unpublished_frame() != 0) {
        return 1;
    }
    if (test_tcp_stream_partial_frame_enters_wifi_rx_ring() != 0) {
        return 1;
    }
    if (test_tcp_stream_multiple_frames_in_one_chunk() != 0) {
        return 1;
    }
    if (test_tcp_stream_invalid_length_is_counted() != 0) {
        return 1;
    }
    if (test_udp_tx_uses_default_peer() != 0) {
        return 1;
    }
    if (test_udp_tx_uses_configured_destination_peer() != 0) {
        return 1;
    }
    if (test_udp_tx_fails_without_configured_peer() != 0) {
        return 1;
    }

    puts("wifi_adapter_test: OK");
    return 0;
}
