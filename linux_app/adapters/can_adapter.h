#ifndef CAN_ADAPTER_H
#define CAN_ADAPTER_H

#include <linux/can.h>
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

#define CAN_ADAPTER_IFNAME_MAX 32u
#define CAN_ADAPTER_DEFAULT_IFNAME "can0"
#define CAN_ADAPTER_DEFAULT_BITRATE 500000u
#define CAN_ADAPTER_DEFAULT_TX_CAN_ID 0x321u
#define CAN_ADAPTER_DEFAULT_RX_FILTER_ID 0x320u
#define CAN_ADAPTER_DEFAULT_RX_FILTER_MASK 0x7FFu
#define CAN_ADAPTER_DEFAULT_REASSEMBLY_TIMEOUT_MS 500u
#define CAN_ADAPTER_DEFAULT_PRIORITY 2u
#define CAN_ADAPTER_DEFAULT_TTL 8u

#define CAN_ADAPTER_FRAME_KIND_SOF 0xA0u
#define CAN_ADAPTER_FRAME_KIND_DATA 0xD0u
#define CAN_ADAPTER_CLASSIC_DLC 8u
#define CAN_ADAPTER_DATA_PAYLOAD_SIZE 5u
#define CAN_ADAPTER_TX_MAX_PACKETS \
    (1u + ((PUT_SHM_FRAME_POOL_BLOCK_SIZE + CAN_ADAPTER_DATA_PAYLOAD_SIZE - 1u) / \
           CAN_ADAPTER_DATA_PAYLOAD_SIZE))
#define CAN_ADAPTER_REASSEMBLY_MAX_SESSIONS 8u
#define CAN_ADAPTER_REASSEMBLY_BITMAP_SIZE 16u

typedef struct {
    bool enabled;
    char ifname[CAN_ADAPTER_IFNAME_MAX];
    uint32_t bitrate; /* Expected externally preconfigured SocketCAN bitrate. */
    uint32_t tx_can_id;
    uint32_t rx_filter_id;
    uint32_t rx_filter_mask;
    bool extended_id;
    uint32_t reassembly_timeout_ms;
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t linux_epoch;
} can_adapter_config_t;

typedef struct {
    bool enabled;
    bool running;
    bool socket_open;
    bool interface_online;
    bool bus_off;
    char ifname[CAN_ADAPTER_IFNAME_MAX];

    uint64_t rx_frames;
    uint64_t tx_frames;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t error_count;
    uint64_t decode_error_count;
    uint64_t fragment_drop_count;
    uint64_t duplicate_fragment_count;
    uint64_t orphan_fragment_count;
    uint64_t reassemble_timeout_count;
    uint64_t crc_error_count;
    uint64_t session_conflict_count;
    uint64_t session_no_buffer_count;
    uint64_t shm_alloc_fail_count;
    uint64_t ipc_error_count;
    uint64_t send_fail_count;
    uint64_t interface_offline_count;

    uint64_t started_at_ms;
    uint64_t updated_at_ms;
    uint64_t last_rx_ms;
    uint64_t last_tx_ms;
    uint64_t last_error_ms;
    char last_error_stage[128];
    char last_error_message[128];
} can_status_t;

typedef struct {
    bool in_use;
    uint32_t source_can_id;
    uint8_t session_id;
    uint16_t total_len;
    uint16_t full_crc16;
    uint8_t data_frame_count;
    uint8_t received_count;
    uint64_t started_at_ms;
    uint64_t updated_at_ms;
    uint8_t buffer[PUT_SHM_FRAME_POOL_BLOCK_SIZE];
    uint8_t received_bitmap[CAN_ADAPTER_REASSEMBLY_BITMAP_SIZE];
} can_reassembly_session_t;

typedef struct {
    uint32_t reassembly_timeout_ms;
    can_status_t *status;
    status_collector_t *collector;
    uint8_t completed_buffer[PUT_SHM_FRAME_POOL_BLOCK_SIZE];
    size_t completed_len;
    can_reassembly_session_t sessions[CAN_ADAPTER_REASSEMBLY_MAX_SESSIONS];
} can_reassembly_context_t;

typedef struct {
    uint32_t tx_can_id;
    bool extended_id;
    uint8_t next_session_id;
    int socket_fd;
    struct can_frame frames[CAN_ADAPTER_TX_MAX_PACKETS];
    adapter_tx_packet_t packets[CAN_ADAPTER_TX_MAX_PACKETS];
} can_tx_context_t;

extern physical_interface_adapter_t can_adapter;

void can_adapter_config_set_defaults(can_adapter_config_t *config);
void can_tx_context_init(can_tx_context_t *ctx, uint32_t tx_can_id, bool extended_id);
void can_tx_context_set_socket(can_tx_context_t *ctx, int socket_fd);
unified_error_t can_adapter_decode_anymsg(const uint8_t *input,
                                          size_t input_len,
                                          adapter_rx_result_t *out);
unified_error_t can_adapter_submit_to_ipc(linux_shm_ipc_t *ipc,
                                          const uint8_t *frame,
                                          const adapter_rx_result_t *rx,
                                          uint32_t linux_epoch);
int can_adapter_start(const can_adapter_config_t *config);
void can_adapter_stop(void);

void can_reassembly_context_init(can_reassembly_context_t *ctx,
                                 uint32_t reassembly_timeout_ms,
                                 can_status_t *status,
                                 status_collector_t *collector);
void can_adapter_reassembly_scan_timeouts(can_reassembly_context_t *ctx, uint64_t now_ms);
unified_error_t can_adapter_reassemble_frame(can_reassembly_context_t *ctx,
                                             const struct can_frame *frame,
                                             uint64_t now_ms,
                                             anymsg_buffer_t *out_complete_msg);

#ifdef __cplusplus
}
#endif

#endif /* CAN_ADAPTER_H */
