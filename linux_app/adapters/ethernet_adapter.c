#define _POSIX_C_SOURCE 200809L

#include "ethernet_adapter.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "anymsg_frame.h"
#include "shared_memory_ipc.h"

#define ETHERNET_RX_BUFFER_SIZE 2048u

typedef struct {
    int socket_fd;
    pthread_t thread;
    bool running;
    ethernet_udp_config_t config;
} ethernet_server_state_t;

static ethernet_server_state_t g_ethernet_server = {
    .socket_fd = -1,
    .running = false,
};

static uint16_t read_le16(const uint8_t bytes[2])
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
}

static put_shm_interface_t route_hint_from_destination_cid(const uint8_t cid[ANYMSG_CID_LENGTH])
{
    switch (anymsg_cid_segment_from_first_byte(cid[0])) {
    case ANYMSG_CID_SEGMENT_CAN:
        return PUT_SHM_INTERFACE_CAN;
    case ANYMSG_CID_SEGMENT_ETHERNET:
        return PUT_SHM_INTERFACE_ETHERNET;
    case ANYMSG_CID_SEGMENT_WIFI:
        return PUT_SHM_INTERFACE_WIFI;
    case ANYMSG_CID_SEGMENT_BLUETOOTH:
        return PUT_SHM_INTERFACE_BLUETOOTH;
    case ANYMSG_CID_SEGMENT_4G:
        return PUT_SHM_INTERFACE_4G;
    case ANYMSG_CID_SEGMENT_RS485:
        return PUT_SHM_INTERFACE_RS485;
    case ANYMSG_CID_SEGMENT_RESERVED_LOW:
    case ANYMSG_CID_SEGMENT_RESERVED_HIGH:
    default:
        return PUT_SHM_INTERFACE_ETHERNET;
    }
}

static const char *error_stage_from_result(unified_error_t err)
{
    switch (err) {
    case UNIFIED_ERR_NULL:
    case UNIFIED_ERR_LENGTH:
    case UNIFIED_ERR_PAYLOAD_LENGTH:
    case UNIFIED_ERR_PROTOCOL_HEADER:
    case UNIFIED_ERR_INVALID_ARG:
        return "ethernet_decode";
    case UNIFIED_ERR_IPC_FRAME_POOL_FULL:
        return "ethernet_ipc_frame_pool_full";
    case UNIFIED_ERR_IPC_QUEUE_FULL:
        return "ethernet_ipc_rx_ring_full";
    default:
        return "ethernet_ipc_to_rtos_send";
    }
}

static size_t ethernet_get_mtu(void *ctx)
{
    (void)ctx;
    return PUT_SHM_FRAME_POOL_BLOCK_SIZE;
}

static int ethernet_decode_rx(void *ctx,
                              const uint8_t *input,
                              size_t input_len,
                              adapter_rx_result_t *out)
{
    (void)ctx;
    return (ethernet_adapter_decode_datagram(input, input_len, out) == UNIFIED_OK) ? 0 : -1;
}

static int ethernet_reassemble(void *ctx,
                               const adapter_fragment_t *fragment,
                               anymsg_buffer_t *out_complete_msg)
{
    adapter_rx_result_t rx;

    if ((fragment == 0) || (out_complete_msg == 0)) {
        return -1;
    }

    if (ethernet_adapter_decode_datagram(fragment->data, fragment->len, &rx) != UNIFIED_OK) {
        return -1;
    }

    (void)ctx;
    out_complete_msg->data = fragment->data;
    out_complete_msg->len = fragment->len;
    return 0;
}

static int ethernet_encapsulate(void *ctx,
                                const anymsg_buffer_t *msg,
                                adapter_tx_packet_t *out_packet)
{
    if ((msg == 0) || (out_packet == 0) || (msg->data == 0) || (msg->len == 0u)) {
        return -1;
    }

    (void)ctx;
    out_packet->data = msg->data;
    out_packet->len = msg->len;
    return 0;
}

static int ethernet_fragment_tx(void *ctx,
                                const anymsg_buffer_t *msg,
                                adapter_tx_packet_list_t *out_packets)
{
    static adapter_tx_packet_t packet;

    if ((msg == 0) || (out_packets == 0) || (msg->data == 0) ||
        (msg->len == 0u) || (msg->len > PUT_SHM_FRAME_POOL_BLOCK_SIZE)) {
        return -1;
    }

    (void)ctx;
    packet.data = msg->data;
    packet.len = msg->len;
    out_packets->packets = &packet;
    out_packets->count = 1u;
    return 0;
}

static int ethernet_send(void *ctx, const adapter_tx_packet_t *packet)
{
    (void)ctx;
    (void)packet;
    return -1;
}

static int ethernet_status(void *ctx, void *out_status)
{
    adapter_status_t *status;

    if (out_status == 0) {
        return -1;
    }

    (void)ctx;
    status = (adapter_status_t *)out_status;
    memset(status, 0, sizeof(*status));
    status->name = "ethernet";
    status->interface_id = (uint8_t)PUT_SHM_INTERFACE_ETHERNET;
    status->state = g_ethernet_server.running ? "online" : "offline";
    return 0;
}

unified_error_t ethernet_adapter_decode_datagram(const uint8_t *input,
                                                 size_t input_len,
                                                 adapter_rx_result_t *out)
{
    const anymsg_header_t *header;
    uint16_t msg_length;
    uint16_t payload_length;
    unified_error_t err;

    if ((input == 0) || (out == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if ((input_len < ANYMSG_HEADER_SIZE) || (input_len > PUT_SHM_FRAME_POOL_BLOCK_SIZE)) {
        return UNIFIED_ERR_LENGTH;
    }

    header = (const anymsg_header_t *)input;
    msg_length = read_le16(header->msg_length);
    payload_length = read_le16(header->payload_length);

    err = anymsg_validate_normalized_lengths(msg_length, payload_length, input_len);
    if (err != UNIFIED_OK) {
        return err;
    }

    err = anymsg_validate_header_static_fields(header);
    if (err != UNIFIED_OK) {
        return err;
    }

    if (anymsg_cid_segment_from_first_byte(anymsg_source_cid_first_byte(header)) !=
        ANYMSG_CID_SEGMENT_ETHERNET) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->data = input;
    out->len = input_len;
    out->msg_length = msg_length;
    out->payload_length = payload_length;
    memcpy(out->source_cid, header->source_cid, ANYMSG_CID_LENGTH);
    memcpy(out->destination_cid, header->destination_cid, ANYMSG_CID_LENGTH);
    out->type = header->type;
    return UNIFIED_OK;
}

unified_error_t ethernet_adapter_submit_to_ipc(linux_shm_ipc_t *ipc,
                                               const uint8_t *frame,
                                               const adapter_rx_result_t *rx,
                                               uint32_t linux_epoch)
{
    uint32_t frame_id;
    uint8_t *frame_buffer;
    uint16_t frame_capacity;
    unified_error_t err;
    put_shm_interface_t target_interface;

    if ((ipc == 0) || (frame == 0) || (rx == 0)) {
        return UNIFIED_ERR_NULL;
    }

    err = linux_shm_frame_alloc(ipc,
                                PUT_SHM_INTERFACE_ETHERNET,
                                &frame_id,
                                &frame_buffer,
                                &frame_capacity);
    if (err != UNIFIED_OK) {
        return err;
    }

    if (rx->len > frame_capacity) {
        (void)linux_shm_frame_release(ipc, frame_id, PUT_SHM_RECLAIM_REASON_INVALID_FRAME);
        return UNIFIED_ERR_LENGTH;
    }

    memcpy(frame_buffer, frame, rx->len);
    target_interface = route_hint_from_destination_cid(rx->destination_cid);
    err = linux_shm_frame_commit_rx(ipc,
                                    frame_id,
                                    (uint16_t)rx->len,
                                    PUT_SHM_INTERFACE_ETHERNET,
                                    target_interface,
                                    rx->source_cid,
                                    rx->destination_cid,
                                    rx->type,
                                    ETHERNET_ADAPTER_DEFAULT_PRIORITY,
                                    ETHERNET_ADAPTER_DEFAULT_TTL,
                                    linux_epoch,
                                    0u);
    if (err != UNIFIED_OK) {
        (void)linux_shm_frame_release(ipc, frame_id, PUT_SHM_RECLAIM_REASON_QUEUE_FULL);
        return err;
    }

    return UNIFIED_OK;
}

unified_error_t ethernet_adapter_handle_datagram(ethernet_rx_context_t *ctx,
                                                 const uint8_t *input,
                                                 size_t input_len)
{
    adapter_rx_result_t rx;
    unified_error_t err;

    if ((ctx == 0) || (ctx->ipc == 0)) {
        return UNIFIED_ERR_NULL;
    }

    err = ethernet_adapter_decode_datagram(input, input_len, &rx);
    if (err != UNIFIED_OK) {
        if (ctx->collector != 0) {
            status_collector_record_error(ctx->collector,
                                          STATUS_MODULE_ETHERNET,
                                          error_stage_from_result(err),
                                          err);
        }
        return err;
    }

    err = ethernet_adapter_submit_to_ipc(ctx->ipc, input, &rx, ctx->linux_epoch);
    if (err != UNIFIED_OK) {
        if (ctx->collector != 0) {
            status_collector_record_error(ctx->collector,
                                          STATUS_MODULE_ETHERNET,
                                          error_stage_from_result(err),
                                          err);
        }
        return err;
    }

    if (ctx->collector != 0) {
        status_collector_record_rx(ctx->collector, STATUS_MODULE_ETHERNET, input_len);
    }
    return UNIFIED_OK;
}

static void *ethernet_udp_thread(void *arg)
{
    ethernet_server_state_t *server;
    ethernet_rx_context_t rx_ctx;
    uint8_t buffer[ETHERNET_RX_BUFFER_SIZE];

    server = (ethernet_server_state_t *)arg;
    rx_ctx.ipc = server->config.ipc;
    rx_ctx.collector = server->config.collector;
    rx_ctx.linux_epoch = server->config.linux_epoch;

    if (rx_ctx.collector != 0) {
        status_collector_mark_running(rx_ctx.collector, STATUS_MODULE_ETHERNET);
    }

    while (server->running) {
        ssize_t received;
        struct sockaddr_in peer_addr;
        socklen_t peer_len;

        peer_len = (socklen_t)sizeof(peer_addr);
        memset(&peer_addr, 0, sizeof(peer_addr));
        received = recvfrom(server->socket_fd,
                            buffer,
                            sizeof(buffer),
                            0,
                            (struct sockaddr *)&peer_addr,
                            &peer_len);
        if (received < 0) {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) {
                continue;
            }
            if (server->running && (rx_ctx.collector != 0)) {
                status_collector_record_error(rx_ctx.collector,
                                              STATUS_MODULE_ETHERNET,
                                              "ethernet_recvfrom",
                                              UNIFIED_ERR_INVALID_ARG);
            }
            continue;
        }

        (void)ethernet_adapter_handle_datagram(&rx_ctx, buffer, (size_t)received);
    }

    if (rx_ctx.collector != 0) {
        status_collector_mark_stopped(rx_ctx.collector, STATUS_MODULE_ETHERNET, "ethernet udp stopped");
    }

    return 0;
}

int ethernet_udp_server_start(const ethernet_udp_config_t *config)
{
    struct sockaddr_in bind_addr;
    struct timeval timeout;
    int reuse;

    if ((config == 0) || (config->ipc == 0)) {
        return -1;
    }

    if (g_ethernet_server.running) {
        return 0;
    }

    memset(&g_ethernet_server, 0, sizeof(g_ethernet_server));
    g_ethernet_server.socket_fd = -1;
    g_ethernet_server.config = *config;

    g_ethernet_server.socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_ethernet_server.socket_fd < 0) {
        if (config->collector != 0) {
            status_collector_record_error(config->collector,
                                          STATUS_MODULE_ETHERNET,
                                          "ethernet_socket",
                                          UNIFIED_ERR_IPC_NOT_READY);
        }
        return -1;
    }

    reuse = 1;
    (void)setsockopt(g_ethernet_server.socket_fd,
                     SOL_SOCKET,
                     SO_REUSEADDR,
                     &reuse,
                     sizeof(reuse));

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    (void)setsockopt(g_ethernet_server.socket_fd,
                     SOL_SOCKET,
                     SO_RCVTIMEO,
                     &timeout,
                     sizeof(timeout));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(config->port);
    if (inet_pton(AF_INET, config->bind_addr, &bind_addr.sin_addr) != 1) {
        (void)close(g_ethernet_server.socket_fd);
        g_ethernet_server.socket_fd = -1;
        if (config->collector != 0) {
            status_collector_record_error(config->collector,
                                          STATUS_MODULE_ETHERNET,
                                          "ethernet_bind_addr",
                                          UNIFIED_ERR_INVALID_ARG);
        }
        return -1;
    }

    if (bind(g_ethernet_server.socket_fd,
             (const struct sockaddr *)&bind_addr,
             sizeof(bind_addr)) != 0) {
        (void)close(g_ethernet_server.socket_fd);
        g_ethernet_server.socket_fd = -1;
        if (config->collector != 0) {
            status_collector_record_error(config->collector,
                                          STATUS_MODULE_ETHERNET,
                                          "ethernet_bind",
                                          UNIFIED_ERR_IPC_NOT_READY);
        }
        return -1;
    }

    g_ethernet_server.running = true;
    if (pthread_create(&g_ethernet_server.thread,
                       0,
                       ethernet_udp_thread,
                       &g_ethernet_server) != 0) {
        g_ethernet_server.running = false;
        (void)close(g_ethernet_server.socket_fd);
        g_ethernet_server.socket_fd = -1;
        if (config->collector != 0) {
            status_collector_record_error(config->collector,
                                          STATUS_MODULE_ETHERNET,
                                          "ethernet_pthread_create",
                                          UNIFIED_ERR_IPC_NOT_READY);
        }
        return -1;
    }

    return 0;
}

void ethernet_udp_server_stop(void)
{
    if (!g_ethernet_server.running) {
        return;
    }

    g_ethernet_server.running = false;
    if (g_ethernet_server.socket_fd >= 0) {
        (void)close(g_ethernet_server.socket_fd);
    }
    (void)pthread_join(g_ethernet_server.thread, 0);
    g_ethernet_server.socket_fd = -1;
}

physical_interface_adapter_t ethernet_adapter = {
    "ethernet",
    (uint8_t)PUT_SHM_INTERFACE_ETHERNET,
    ethernet_get_mtu,
    ethernet_decode_rx,
    ethernet_reassemble,
    ethernet_encapsulate,
    ethernet_fragment_tx,
    ethernet_send,
    ethernet_status,
};
