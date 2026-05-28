#ifndef PHYSICAL_INTERFACE_ADAPTER_H
#define PHYSICAL_INTERFACE_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "anymsg_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data;
    size_t len;
    uint16_t msg_length;
    uint16_t payload_length;
    uint8_t source_cid[ANYMSG_CID_LENGTH];
    uint8_t destination_cid[ANYMSG_CID_LENGTH];
    uint8_t type;
} adapter_rx_result_t;

typedef struct {
    const uint8_t *data;
    size_t len;
} anymsg_buffer_t;

typedef struct {
    const uint8_t *data;
    size_t len;
} adapter_fragment_t;

typedef struct {
    const uint8_t *data;
    size_t len;
} adapter_tx_packet_t;

typedef struct {
    adapter_tx_packet_t *packets;
    size_t count;
} adapter_tx_packet_list_t;

typedef struct {
    const char *name;
    uint8_t interface_id;
    uint64_t rx_frames;
    uint64_t tx_frames;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t error_count;
    const char *state;
} adapter_status_t;

typedef struct {
    const char *name;
    uint8_t interface_id;

    size_t (*get_mtu)(void *ctx);
    int (*decode_rx)(void *ctx,
                     const uint8_t *input,
                     size_t input_len,
                     adapter_rx_result_t *out);
    int (*reassemble)(void *ctx,
                      const adapter_fragment_t *fragment,
                      anymsg_buffer_t *out_complete_msg);
    int (*encapsulate)(void *ctx,
                       const anymsg_buffer_t *msg,
                       adapter_tx_packet_t *out_packet);
    int (*fragment_tx)(void *ctx,
                       const anymsg_buffer_t *msg,
                       adapter_tx_packet_list_t *out_packets);
    int (*send)(void *ctx, const adapter_tx_packet_t *packet);
    int (*status)(void *ctx, void *out_status);
} physical_interface_adapter_t;

#ifdef __cplusplus
}
#endif

#endif /* PHYSICAL_INTERFACE_ADAPTER_H */
