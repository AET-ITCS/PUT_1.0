#ifndef WIFI_ADAPTER_H
#define WIFI_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_code.h"
#include "linux_shm_ipc.h"
#include "physical_interface_adapter.h"
#include "status_collector.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_ADAPTER_DEFAULT_BIND_ADDR "0.0.0.0"
#define WIFI_ADAPTER_DEFAULT_PORT 5001u
#define WIFI_ADAPTER_DEFAULT_TCP_BACKLOG 4u
#define WIFI_ADAPTER_DEFAULT_PRIORITY 2u
#define WIFI_ADAPTER_DEFAULT_TTL 8u
#define WIFI_TX_PEER_MAX 16u

typedef struct {
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t linux_epoch;
} wifi_rx_context_t;

typedef struct {
    bool enabled;
    char bind_addr[64];
    uint16_t port;
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t linux_epoch;
} wifi_udp_config_t;

typedef struct {
    bool enabled;
    char bind_addr[64];
    uint16_t port;
    uint16_t listen_backlog;
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t linux_epoch;
} wifi_tcp_config_t;

typedef struct {
    wifi_rx_context_t rx_ctx;
    uint8_t buffer[PUT_SHM_FRAME_POOL_BLOCK_SIZE];
    size_t buffered_len;
} wifi_tcp_stream_context_t;

typedef struct {
    uint8_t destination_cid[ANYMSG_CID_LENGTH];
    uint32_t ipv4_addr_be;
    uint16_t port;
} wifi_tx_peer_t;

typedef struct {
    uint16_t port;
    int udp_socket_fd;
    bool default_peer_configured;
    uint32_t default_peer_addr_be;
    const char *last_tx_error_stage;
    unified_error_t last_tx_error;
    wifi_tx_peer_t peers[WIFI_TX_PEER_MAX];
    size_t peer_count;
} wifi_tx_context_t;

extern physical_interface_adapter_t wifi_adapter;

void wifi_tx_context_init(wifi_tx_context_t *ctx, uint16_t port);
unified_error_t wifi_tx_context_set_default_peer(wifi_tx_context_t *ctx,
                                                 const char *ipv4,
                                                 uint16_t port);
int wifi_tx_context_add_peer(wifi_tx_context_t *ctx, const wifi_tx_peer_t *peer);
void wifi_tx_context_destroy(wifi_tx_context_t *ctx);

unified_error_t wifi_adapter_decode_datagram(const uint8_t *input,
                                                 size_t input_len,
                                                 adapter_rx_result_t *out);

unified_error_t wifi_adapter_submit_to_ipc(linux_shm_ipc_t *ipc,
                                               const uint8_t *frame,
                                               const adapter_rx_result_t *rx,
                                               uint32_t linux_epoch);

unified_error_t wifi_adapter_handle_datagram(wifi_rx_context_t *ctx,
                                                 const uint8_t *input,
                                                 size_t input_len);

void wifi_tcp_stream_init(wifi_tcp_stream_context_t *stream_ctx,
                              const wifi_rx_context_t *rx_ctx);
unified_error_t wifi_adapter_handle_tcp_bytes(wifi_tcp_stream_context_t *stream_ctx,
                                                  const uint8_t *input,
                                                  size_t input_len);

int wifi_udp_server_start(const wifi_udp_config_t *config);
void wifi_udp_server_stop(void);
int wifi_tcp_server_start(const wifi_tcp_config_t *config);
void wifi_tcp_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_ADAPTER_H */
