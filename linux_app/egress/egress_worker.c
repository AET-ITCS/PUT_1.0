#define _POSIX_C_SOURCE 200809L

#include "egress_worker.h"

#include <string.h>

#define EGRESS_DIRECT_PACKET_COUNT 1u

static uint16_t read_le16(const uint8_t bytes[2])
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
}

static put_shm_interface_t interface_from_cid_segment(anymsg_cid_segment_t segment)
{
    switch (segment) {
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
        return PUT_SHM_INTERFACE_COUNT;
    }
}

static bool descriptor_reserved_fields_are_zero(const put_shm_descriptor_t *descriptor)
{
    if ((descriptor == 0) || (descriptor->reserved0 != 0u)) {
        return false;
    }

    for (size_t i = 0u; i < sizeof(descriptor->reserved1); ++i) {
        if (descriptor->reserved1[i] != 0u) {
            return false;
        }
    }

    return true;
}

static unified_error_t validate_anymsg_for_egress(const egress_worker_context_t *ctx,
                                                  const put_shm_descriptor_t *descriptor,
                                                  const uint8_t *frame,
                                                  uint16_t frame_length)
{
    const anymsg_header_t *header;
    uint16_t msg_length;
    uint16_t payload_length;
    anymsg_cid_segment_t source_segment;
    anymsg_cid_segment_t destination_segment;
    unified_error_t err;

    if ((ctx == 0) || (descriptor == 0) || (frame == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if ((descriptor->target_interface != (uint8_t)ctx->interface_id) ||
        (descriptor->frame_length != frame_length) ||
        !descriptor_reserved_fields_are_zero(descriptor)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if ((frame_length < ANYMSG_HEADER_SIZE) ||
        (frame_length > PUT_SHM_FRAME_POOL_BLOCK_SIZE)) {
        return UNIFIED_ERR_LENGTH;
    }

    header = (const anymsg_header_t *)frame;
    msg_length = read_le16(header->msg_length);
    payload_length = read_le16(header->payload_length);
    err = anymsg_validate_normalized_lengths(msg_length, payload_length, frame_length);
    if (err != UNIFIED_OK) {
        return err;
    }

    err = anymsg_validate_header_static_fields(header);
    if (err != UNIFIED_OK) {
        return err;
    }

    if ((memcmp(descriptor->source_cid, header->source_cid, ANYMSG_CID_LENGTH) != 0) ||
        (memcmp(descriptor->destination_cid, header->destination_cid, ANYMSG_CID_LENGTH) != 0) ||
        (descriptor->type != header->type)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    source_segment = anymsg_cid_segment_from_first_byte(header->source_cid[0]);
    destination_segment = anymsg_cid_segment_from_first_byte(header->destination_cid[0]);
    if ((source_segment == ANYMSG_CID_SEGMENT_RESERVED_LOW) ||
        (source_segment == ANYMSG_CID_SEGMENT_RESERVED_HIGH) ||
        (destination_segment == ANYMSG_CID_SEGMENT_RESERVED_LOW) ||
        (destination_segment == ANYMSG_CID_SEGMENT_RESERVED_HIGH)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (interface_from_cid_segment(destination_segment) != ctx->interface_id) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

static void record_error(const egress_worker_context_t *ctx,
                         const char *stage,
                         unified_error_t err)
{
    if ((ctx != 0) && (ctx->collector != 0)) {
        status_collector_record_error(ctx->collector, ctx->status_module, stage, err);
    }
}

static bool adapter_is_usable(const egress_worker_context_t *ctx)
{
    return (ctx != 0) && ctx->enabled && (ctx->adapter != 0) &&
           (ctx->adapter->send != 0);
}

static bool send_packet_list(const egress_worker_context_t *ctx,
                             const adapter_tx_packet_list_t *packets)
{
    if ((ctx == 0) || (ctx->adapter == 0) || (ctx->adapter->send == 0) ||
        (packets == 0) || (packets->packets == 0) || (packets->count == 0u)) {
        return false;
    }

    for (size_t i = 0u; i < packets->count; ++i) {
        if (ctx->adapter->send(ctx->adapter_ctx, &packets->packets[i]) != 0) {
            return false;
        }
    }

    return true;
}

static bool send_by_adapter(const egress_worker_context_t *ctx,
                            const uint8_t *frame,
                            uint16_t frame_length)
{
    anymsg_buffer_t msg;
    adapter_tx_packet_t packet;
    adapter_tx_packet_list_t packets;
    size_t mtu;

    if (!adapter_is_usable(ctx)) {
        return false;
    }

    msg.data = frame;
    msg.len = frame_length;
    mtu = (ctx->adapter->get_mtu == 0) ? 0u : ctx->adapter->get_mtu(ctx->adapter_ctx);

    if ((mtu >= msg.len) && (ctx->adapter->encapsulate != 0)) {
        memset(&packet, 0, sizeof(packet));
        if (ctx->adapter->encapsulate(ctx->adapter_ctx, &msg, &packet) == 0) {
            packets.packets = &packet;
            packets.count = EGRESS_DIRECT_PACKET_COUNT;
            return send_packet_list(ctx, &packets);
        }
    }

    if (ctx->adapter->fragment_tx == 0) {
        return false;
    }

    memset(&packets, 0, sizeof(packets));
    if (ctx->adapter->fragment_tx(ctx->adapter_ctx, &msg, &packets) != 0) {
        return false;
    }

    return send_packet_list(ctx, &packets);
}

static void record_send_failure(const egress_worker_context_t *ctx)
{
    adapter_tx_error_t tx_error;

    if ((ctx != 0) && (ctx->adapter != 0) && (ctx->adapter->get_tx_error != 0) &&
        (ctx->adapter->get_tx_error(ctx->adapter_ctx, &tx_error) == 0) &&
        (tx_error.stage != 0) && (tx_error.stage[0] != '\0')) {
        record_error(ctx, tx_error.stage, tx_error.err);
        return;
    }

    record_error(ctx, "send", UNIFIED_ERR_IPC_OFFLINE);
}

static void release_frame(const egress_worker_context_t *ctx,
                          const put_shm_descriptor_t *descriptor,
                          egress_worker_drain_result_t *result)
{
    unified_error_t err;

    if ((ctx == 0) || (descriptor == 0)) {
        return;
    }

    err = linux_shm_frame_release(ctx->ipc,
                                  descriptor->frame_id,
                                  PUT_SHM_RECLAIM_REASON_NONE);
    if (err != UNIFIED_OK) {
        if (result != 0) {
            result->release_errors++;
        }
        record_error(ctx, "egress_ipc_release", err);
    }
}

unified_error_t egress_worker_drain_once(egress_worker_context_t *ctx,
                                         egress_worker_drain_result_t *out_result)
{
    egress_worker_drain_result_t result;
    uint32_t budget;

    if ((ctx == 0) || (ctx->ipc == 0) || (ctx->interface_id >= PUT_SHM_INTERFACE_COUNT)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(&result, 0, sizeof(result));
    budget = (ctx->budget == 0u) ? EGRESS_WORKER_DEFAULT_BUDGET : ctx->budget;

    for (uint32_t i = 0u; i < budget; ++i) {
        put_shm_descriptor_t descriptor;
        const uint8_t *frame;
        uint16_t frame_length;
        unified_error_t err;
        bool send_ok;

        memset(&descriptor, 0, sizeof(descriptor));
        frame = 0;
        frame_length = 0u;
        err = linux_shm_dequeue_tx_descriptor(ctx->ipc,
                                              ctx->interface_id,
                                              &descriptor,
                                              &frame,
                                              &frame_length);
        if (err == UNIFIED_ERR_IPC_QUEUE_EMPTY) {
            break;
        }
        if (err != UNIFIED_OK) {
            result.ipc_errors++;
            record_error(ctx, "egress_ipc_dequeue", err);
            continue;
        }

        result.dequeued++;
        err = validate_anymsg_for_egress(ctx, &descriptor, frame, frame_length);
        if (err != UNIFIED_OK) {
            result.validation_failed++;
            record_error(ctx, "egress_validate", err);
            release_frame(ctx, &descriptor, &result);
            continue;
        }

        if (!ctx->enabled) {
            result.tx_failed++;
            record_error(ctx, "interface_disabled", UNIFIED_ERR_IPC_OFFLINE);
            release_frame(ctx, &descriptor, &result);
            continue;
        }
        if (ctx->adapter == 0) {
            result.tx_failed++;
            record_error(ctx, "adapter_missing", UNIFIED_ERR_IPC_OFFLINE);
            release_frame(ctx, &descriptor, &result);
            continue;
        }

        send_ok = send_by_adapter(ctx, frame, frame_length);
        if (send_ok) {
            result.tx_ok++;
            if (ctx->collector != 0) {
                status_collector_record_tx_ok(ctx->collector,
                                              ctx->status_module,
                                              frame_length);
            }
        } else {
            result.tx_failed++;
            record_send_failure(ctx);
        }

        release_frame(ctx, &descriptor, &result);
    }

    if (out_result != 0) {
        *out_result = result;
    }
    return UNIFIED_OK;
}
