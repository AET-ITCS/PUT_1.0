#ifndef ETHERNET_ADAPTER_H
#define ETHERNET_ADAPTER_H

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

#define ETHERNET_ADAPTER_DEFAULT_BIND_ADDR "0.0.0.0"
#define ETHERNET_ADAPTER_DEFAULT_PORT 5000u
#define ETHERNET_ADAPTER_DEFAULT_TCP_BACKLOG 4u
#define ETHERNET_ADAPTER_DEFAULT_PRIORITY 2u
#define ETHERNET_ADAPTER_DEFAULT_TTL 8u

typedef struct {
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t linux_epoch;
} ethernet_rx_context_t;

typedef struct {
    bool enabled;
    char bind_addr[64];
    uint16_t port;
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t linux_epoch;
} ethernet_udp_config_t;

typedef struct {
    bool enabled;
    char bind_addr[64];
    uint16_t port;
    uint16_t listen_backlog;
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t linux_epoch;
} ethernet_tcp_config_t;

typedef struct {
    ethernet_rx_context_t rx_ctx;
    uint8_t buffer[PUT_SHM_FRAME_POOL_BLOCK_SIZE];
    size_t buffered_len;
} ethernet_tcp_stream_context_t;

typedef struct {
    uint16_t port;
    int udp_socket_fd;
} ethernet_tx_context_t;

extern physical_interface_adapter_t ethernet_adapter;

void ethernet_tx_context_init(ethernet_tx_context_t *ctx, uint16_t port);
void ethernet_tx_context_destroy(ethernet_tx_context_t *ctx);

unified_error_t ethernet_adapter_decode_datagram(const uint8_t *input,
                                                 size_t input_len,
                                                 adapter_rx_result_t *out);

unified_error_t ethernet_adapter_submit_to_ipc(linux_shm_ipc_t *ipc,
                                               const uint8_t *frame,
                                               const adapter_rx_result_t *rx,
                                               uint32_t linux_epoch);

unified_error_t ethernet_adapter_handle_datagram(ethernet_rx_context_t *ctx,
                                                 const uint8_t *input,
                                                 size_t input_len);

void ethernet_tcp_stream_init(ethernet_tcp_stream_context_t *stream_ctx,
                              const ethernet_rx_context_t *rx_ctx);
unified_error_t ethernet_adapter_handle_tcp_bytes(ethernet_tcp_stream_context_t *stream_ctx,
                                                  const uint8_t *input,
                                                  size_t input_len);

int ethernet_udp_server_start(const ethernet_udp_config_t *config);
void ethernet_udp_server_stop(void);
int ethernet_tcp_server_start(const ethernet_tcp_config_t *config);
void ethernet_tcp_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_ADAPTER_H */
