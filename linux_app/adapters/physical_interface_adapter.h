/**
 * @file physical_interface_adapter.h
 * @brief Linux 物理接口适配层公共入口结果与 TX 抽象。
 * @author Yukikaze
 */
#ifndef PHYSICAL_INTERFACE_ADAPTER_H
#define PHYSICAL_INTERFACE_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "anymsg_frame.h"
#include "error_code.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data;                             /* 完整 anyMSG 起始地址。 */
    size_t len;                                      /* 完整 anyMSG 字节数。 */
    uint16_t msg_length;                             /* anyMSG msg_length 字段。 */
    uint16_t payload_length;                         /* anyMSG payload_length 字段。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH];           /* anyMSG source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH];      /* anyMSG destination CID。 */
    uint8_t type;                                    /* anyMSG payload type。 */
    uint32_t trust_flags;                            /* descriptor trust flags，见 PUT_SHM_DESCRIPTOR_FLAG_*。 */
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
    const char *stage;
    unified_error_t err;
} adapter_tx_error_t;

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
    int (*get_tx_error)(void *ctx, adapter_tx_error_t *out_error);
} physical_interface_adapter_t;

#ifdef __cplusplus
}
#endif

#endif /* PHYSICAL_INTERFACE_ADAPTER_H */
