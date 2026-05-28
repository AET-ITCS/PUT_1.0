#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "can_adapter.h"

#include <errno.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "anymsg_frame.h"
#include "crc16.h"
#include "shared_memory_ipc.h"

#define CAN_ADAPTER_SOCKET_TIMEOUT_US 100000
#define CAN_ADAPTER_RETRY_FIRST_MS 1000u
#define CAN_ADAPTER_RETRY_SECOND_MS 2000u
#define CAN_ADAPTER_RETRY_MAX_MS 5000u

typedef struct {
    pthread_mutex_t lock;
    int socket_fd;
    pthread_t thread;
    bool thread_started;
    bool stop_requested;
    can_adapter_config_t config;
    can_status_t status;
    can_reassembly_context_t reassembly;
} can_adapter_state_t;

static can_adapter_state_t g_can_state = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .socket_fd = -1,
};

static uint16_t read_le16(const uint8_t bytes[2])
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
}

static uint64_t now_monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }

    return ((uint64_t)ts.tv_sec * 1000ull) + ((uint64_t)ts.tv_nsec / 1000000ull);
}

static void sleep_ms(uint32_t duration_ms)
{
    struct timespec req;
    struct timespec rem;

    req.tv_sec = (time_t)(duration_ms / 1000u);
    req.tv_nsec = (long)((duration_ms % 1000u) * 1000000u);
    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) {
            break;
        }
        req = rem;
    }
}

static bool string_has(const char *text, const char *needle)
{
    return (text != 0) && (needle != 0) && (strstr(text, needle) != 0);
}

static void record_error(can_status_t *status,
                         status_collector_t *collector,
                         const char *stage,
                         unified_error_t err)
{
    uint64_t now_ms;

    now_ms = now_monotonic_ms();
    if (status != 0) {
        status->error_count++;
        status->last_error_ms = now_ms;
        status->updated_at_ms = now_ms;
        (void)snprintf(status->last_error_stage,
                       sizeof(status->last_error_stage),
                       "%s",
                       (stage == 0) ? "can_unknown" : stage);
        (void)snprintf(status->last_error_message,
                       sizeof(status->last_error_message),
                       "%s failed with error=%d",
                       (stage == 0) ? "can_unknown" : stage,
                       (int)err);

        if (string_has(stage, "decode") || string_has(stage, "parse")) {
            status->decode_error_count++;
        } else if (string_has(stage, "fragment")) {
            status->fragment_drop_count++;
        } else if (string_has(stage, "timeout")) {
            status->reassemble_timeout_count++;
        } else if (string_has(stage, "crc")) {
            status->crc_error_count++;
        } else if (string_has(stage, "send")) {
            status->send_fail_count++;
        } else if (string_has(stage, "socket") || string_has(stage, "bind")) {
            status->interface_offline_count++;
        }

        if (string_has(stage, "ipc")) {
            status->ipc_error_count++;
        }
        if (err == UNIFIED_ERR_IPC_FRAME_POOL_FULL) {
            status->shm_alloc_fail_count++;
        }
    }

    if (collector != 0) {
        status_collector_record_error(collector, STATUS_MODULE_CAN, stage, err);
    }
}

static void record_rx_ok(can_status_t *status,
                         status_collector_t *collector,
                         size_t bytes)
{
    uint64_t now_ms;

    now_ms = now_monotonic_ms();
    if (status != 0) {
        status->rx_frames++;
        status->rx_bytes += (uint64_t)bytes;
        status->last_rx_ms = now_ms;
        status->updated_at_ms = now_ms;
    }
    if (collector != 0) {
        status_collector_record_rx(collector, STATUS_MODULE_CAN, bytes);
    }
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
        return PUT_SHM_INTERFACE_CAN;
    }
}

static uint8_t expected_data_frame_count(uint16_t total_len)
{
    return (uint8_t)(((uint32_t)total_len + CAN_ADAPTER_DATA_PAYLOAD_SIZE - 1u) /
                     CAN_ADAPTER_DATA_PAYLOAD_SIZE);
}

static uint32_t normalize_source_can_id(canid_t can_id)
{
    if ((can_id & CAN_EFF_FLAG) != 0u) {
        return (uint32_t)((can_id & CAN_EFF_MASK) | CAN_EFF_FLAG);
    }

    return (uint32_t)(can_id & CAN_SFF_MASK);
}

static bool bitmap_get(const uint8_t *bitmap, uint8_t seq)
{
    return (bitmap[(uint32_t)seq / 8u] & (uint8_t)(1u << (seq % 8u))) != 0u;
}

static void bitmap_set(uint8_t *bitmap, uint8_t seq)
{
    bitmap[(uint32_t)seq / 8u] =
        (uint8_t)(bitmap[(uint32_t)seq / 8u] | (uint8_t)(1u << (seq % 8u)));
}

static void clear_session(can_reassembly_session_t *session)
{
    if (session == 0) {
        return;
    }

    session->in_use = false;
    session->source_can_id = 0u;
    session->session_id = 0u;
    session->total_len = 0u;
    session->full_crc16 = 0u;
    session->data_frame_count = 0u;
    session->received_count = 0u;
    session->started_at_ms = 0u;
    session->updated_at_ms = 0u;
    memset(session->received_bitmap, 0, sizeof(session->received_bitmap));
}

static void clear_all_sessions(can_reassembly_context_t *ctx)
{
    if (ctx == 0) {
        return;
    }

    for (size_t i = 0u; i < CAN_ADAPTER_REASSEMBLY_MAX_SESSIONS; ++i) {
        clear_session(&ctx->sessions[i]);
    }
}

static can_reassembly_session_t *find_session(can_reassembly_context_t *ctx,
                                              uint32_t source_can_id,
                                              uint8_t session_id)
{
    if (ctx == 0) {
        return 0;
    }

    for (size_t i = 0u; i < CAN_ADAPTER_REASSEMBLY_MAX_SESSIONS; ++i) {
        can_reassembly_session_t *session = &ctx->sessions[i];
        if (session->in_use &&
            (session->source_can_id == source_can_id) &&
            (session->session_id == session_id)) {
            return session;
        }
    }

    return 0;
}

static can_reassembly_session_t *find_free_session(can_reassembly_context_t *ctx)
{
    if (ctx == 0) {
        return 0;
    }

    for (size_t i = 0u; i < CAN_ADAPTER_REASSEMBLY_MAX_SESSIONS; ++i) {
        if (!ctx->sessions[i].in_use) {
            return &ctx->sessions[i];
        }
    }

    return 0;
}

static can_reassembly_session_t *find_oldest_session(can_reassembly_context_t *ctx)
{
    can_reassembly_session_t *oldest = 0;

    if (ctx == 0) {
        return 0;
    }

    for (size_t i = 0u; i < CAN_ADAPTER_REASSEMBLY_MAX_SESSIONS; ++i) {
        can_reassembly_session_t *session = &ctx->sessions[i];
        if (!session->in_use) {
            continue;
        }
        if ((oldest == 0) || (session->started_at_ms < oldest->started_at_ms)) {
            oldest = session;
        }
    }

    return oldest;
}

void can_adapter_config_set_defaults(can_adapter_config_t *config)
{
    if (config == 0) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->enabled = false;
    (void)snprintf(config->ifname, sizeof(config->ifname), "%s", CAN_ADAPTER_DEFAULT_IFNAME);
    config->bitrate = CAN_ADAPTER_DEFAULT_BITRATE;
    config->tx_can_id = CAN_ADAPTER_DEFAULT_TX_CAN_ID;
    config->rx_filter_id = CAN_ADAPTER_DEFAULT_RX_FILTER_ID;
    config->rx_filter_mask = CAN_ADAPTER_DEFAULT_RX_FILTER_MASK;
    config->extended_id = false;
    config->reassembly_timeout_ms = CAN_ADAPTER_DEFAULT_REASSEMBLY_TIMEOUT_MS;
}

void can_tx_context_init(can_tx_context_t *ctx, uint32_t tx_can_id, bool extended_id)
{
    if (ctx == 0) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->tx_can_id = tx_can_id;
    ctx->extended_id = extended_id;
    ctx->socket_fd = -1;
}

void can_tx_context_set_socket(can_tx_context_t *ctx, int socket_fd)
{
    if (ctx == 0) {
        return;
    }

    ctx->socket_fd = socket_fd;
}

unified_error_t can_adapter_decode_anymsg(const uint8_t *input,
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
        ANYMSG_CID_SEGMENT_CAN) {
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

unified_error_t can_adapter_submit_to_ipc(linux_shm_ipc_t *ipc,
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
                                PUT_SHM_INTERFACE_CAN,
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
                                    PUT_SHM_INTERFACE_CAN,
                                    target_interface,
                                    rx->source_cid,
                                    rx->destination_cid,
                                    rx->type,
                                    CAN_ADAPTER_DEFAULT_PRIORITY,
                                    CAN_ADAPTER_DEFAULT_TTL,
                                    linux_epoch,
                                    0u);
    if (err != UNIFIED_OK) {
        (void)linux_shm_frame_release(ipc, frame_id, PUT_SHM_RECLAIM_REASON_QUEUE_FULL);
        return err;
    }

    return UNIFIED_OK;
}

void can_reassembly_context_init(can_reassembly_context_t *ctx,
                                 uint32_t reassembly_timeout_ms,
                                 can_status_t *status,
                                 status_collector_t *collector)
{
    if (ctx == 0) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->reassembly_timeout_ms =
        (reassembly_timeout_ms == 0u) ? CAN_ADAPTER_DEFAULT_REASSEMBLY_TIMEOUT_MS :
                                        reassembly_timeout_ms;
    ctx->status = status;
    ctx->collector = collector;
}

void can_adapter_reassembly_scan_timeouts(can_reassembly_context_t *ctx, uint64_t now_ms)
{
    uint32_t timeout_ms;

    if (ctx == 0) {
        return;
    }

    timeout_ms = (ctx->reassembly_timeout_ms == 0u) ?
        CAN_ADAPTER_DEFAULT_REASSEMBLY_TIMEOUT_MS : ctx->reassembly_timeout_ms;
    for (size_t i = 0u; i < CAN_ADAPTER_REASSEMBLY_MAX_SESSIONS; ++i) {
        can_reassembly_session_t *session = &ctx->sessions[i];
        if (!session->in_use) {
            continue;
        }
        if ((now_ms >= session->updated_at_ms) &&
            ((now_ms - session->updated_at_ms) > (uint64_t)timeout_ms)) {
            record_error(ctx->status,
                         ctx->collector,
                         "can_reassemble_timeout",
                         UNIFIED_ERR_LENGTH);
            clear_session(session);
        }
    }
}

static unified_error_t handle_sof(can_reassembly_context_t *ctx,
                                  const struct can_frame *frame,
                                  uint32_t source_can_id,
                                  uint64_t now_ms)
{
    uint8_t session_id;
    uint16_t total_len;
    uint16_t full_crc16;
    uint8_t flags;
    uint8_t data_frame_count;
    uint8_t expected_count;
    can_reassembly_session_t *session;

    session_id = frame->data[1];
    total_len = read_le16(&frame->data[2]);
    full_crc16 = read_le16(&frame->data[4]);
    flags = frame->data[6];
    data_frame_count = frame->data[7];

    if ((total_len < ANYMSG_HEADER_SIZE) ||
        (total_len > PUT_SHM_FRAME_POOL_BLOCK_SIZE) ||
        ((flags & (uint8_t)~0x01u) != 0u)) {
        record_error(ctx->status, ctx->collector, "can_fragment_sof", UNIFIED_ERR_LENGTH);
        return UNIFIED_ERR_LENGTH;
    }

    expected_count = expected_data_frame_count(total_len);
    if ((data_frame_count == 0u) || (data_frame_count != expected_count)) {
        record_error(ctx->status, ctx->collector, "can_fragment_sof", UNIFIED_ERR_INVALID_ARG);
        return UNIFIED_ERR_INVALID_ARG;
    }

    session = find_session(ctx, source_can_id, session_id);
    if (session != 0) {
        if (ctx->status != 0) {
            ctx->status->session_conflict_count++;
        }
        record_error(ctx->status, ctx->collector, "can_session_conflict", UNIFIED_ERR_INVALID_ARG);
    } else {
        can_adapter_reassembly_scan_timeouts(ctx, now_ms);
        session = find_free_session(ctx);
        if (session == 0) {
            session = find_oldest_session(ctx);
            if (ctx->status != 0) {
                ctx->status->session_no_buffer_count++;
            }
            record_error(ctx->status, ctx->collector, "can_session_no_buffer", UNIFIED_ERR_IPC_QUEUE_FULL);
        }
    }

    if (session == 0) {
        return UNIFIED_ERR_IPC_QUEUE_FULL;
    }

    memset(session, 0, sizeof(*session));
    session->in_use = true;
    session->source_can_id = source_can_id;
    session->session_id = session_id;
    session->total_len = total_len;
    session->full_crc16 = full_crc16;
    session->data_frame_count = data_frame_count;
    session->started_at_ms = now_ms;
    session->updated_at_ms = now_ms;
    return UNIFIED_OK;
}

static unified_error_t handle_data(can_reassembly_context_t *ctx,
                                   const struct can_frame *frame,
                                   uint32_t source_can_id,
                                   uint64_t now_ms,
                                   anymsg_buffer_t *out_complete_msg)
{
    uint8_t session_id;
    uint8_t seq;
    size_t offset;
    size_t copy_len;
    uint16_t actual_crc;
    can_reassembly_session_t *session;

    session_id = frame->data[1];
    seq = frame->data[2];
    session = find_session(ctx, source_can_id, session_id);
    if (session == 0) {
        if (ctx->status != 0) {
            ctx->status->orphan_fragment_count++;
        }
        record_error(ctx->status, ctx->collector, "can_fragment_orphan", UNIFIED_ERR_INVALID_ARG);
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (seq >= session->data_frame_count) {
        record_error(ctx->status, ctx->collector, "can_fragment_seq", UNIFIED_ERR_INVALID_ARG);
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (bitmap_get(session->received_bitmap, seq)) {
        if (ctx->status != 0) {
            ctx->status->duplicate_fragment_count++;
        }
        record_error(ctx->status, ctx->collector, "can_fragment_duplicate", UNIFIED_ERR_INVALID_ARG);
        return UNIFIED_OK;
    }

    offset = (size_t)seq * CAN_ADAPTER_DATA_PAYLOAD_SIZE;
    copy_len = (size_t)session->total_len - offset;
    if (copy_len > CAN_ADAPTER_DATA_PAYLOAD_SIZE) {
        copy_len = CAN_ADAPTER_DATA_PAYLOAD_SIZE;
    }

    memcpy(session->buffer + offset, &frame->data[3], copy_len);
    bitmap_set(session->received_bitmap, seq);
    session->received_count++;
    session->updated_at_ms = now_ms;

    if (session->received_count < session->data_frame_count) {
        return UNIFIED_OK;
    }

    actual_crc = unified_crc16_ccitt_false(session->buffer, session->total_len);
    if (actual_crc != session->full_crc16) {
        record_error(ctx->status, ctx->collector, "can_crc", UNIFIED_ERR_CRC);
        clear_session(session);
        return UNIFIED_ERR_CRC;
    }

    memcpy(ctx->completed_buffer, session->buffer, session->total_len);
    ctx->completed_len = session->total_len;
    out_complete_msg->data = ctx->completed_buffer;
    out_complete_msg->len = ctx->completed_len;
    clear_session(session);
    return UNIFIED_OK;
}

unified_error_t can_adapter_reassemble_frame(can_reassembly_context_t *ctx,
                                             const struct can_frame *frame,
                                             uint64_t now_ms,
                                             anymsg_buffer_t *out_complete_msg)
{
    uint32_t source_can_id;

    if ((ctx == 0) || (frame == 0) || (out_complete_msg == 0)) {
        return UNIFIED_ERR_NULL;
    }

    out_complete_msg->data = 0;
    out_complete_msg->len = 0u;
    can_adapter_reassembly_scan_timeouts(ctx, now_ms);

    if ((frame->can_id & CAN_RTR_FLAG) != 0u) {
        record_error(ctx->status, ctx->collector, "can_fragment_rtr", UNIFIED_ERR_INVALID_ARG);
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (frame->can_dlc != CAN_ADAPTER_CLASSIC_DLC) {
        record_error(ctx->status, ctx->collector, "can_fragment_dlc", UNIFIED_ERR_CAN_DLC);
        return UNIFIED_ERR_CAN_DLC;
    }

    source_can_id = normalize_source_can_id(frame->can_id);
    switch (frame->data[0]) {
    case CAN_ADAPTER_FRAME_KIND_SOF:
        return handle_sof(ctx, frame, source_can_id, now_ms);
    case CAN_ADAPTER_FRAME_KIND_DATA:
        return handle_data(ctx, frame, source_can_id, now_ms, out_complete_msg);
    default:
        record_error(ctx->status, ctx->collector, "can_fragment_kind", UNIFIED_ERR_INVALID_ARG);
        return UNIFIED_ERR_INVALID_ARG;
    }
}

static size_t can_get_mtu(void *ctx)
{
    (void)ctx;
    return CAN_ADAPTER_DATA_PAYLOAD_SIZE;
}

static int can_decode_rx(void *ctx,
                         const uint8_t *input,
                         size_t input_len,
                         adapter_rx_result_t *out)
{
    (void)ctx;
    return (can_adapter_decode_anymsg(input, input_len, out) == UNIFIED_OK) ? 0 : -1;
}

static int can_reassemble(void *ctx,
                          const adapter_fragment_t *fragment,
                          anymsg_buffer_t *out_complete_msg)
{
    can_reassembly_context_t *reassembly_ctx;

    if ((ctx == 0) || (fragment == 0) || (fragment->data == 0) ||
        (fragment->len != sizeof(struct can_frame)) || (out_complete_msg == 0)) {
        return -1;
    }

    reassembly_ctx = (can_reassembly_context_t *)ctx;
    return (can_adapter_reassemble_frame(reassembly_ctx,
                                         (const struct can_frame *)fragment->data,
                                         now_monotonic_ms(),
                                         out_complete_msg) == UNIFIED_OK) ? 0 : -1;
}

static int can_encapsulate(void *ctx,
                           const anymsg_buffer_t *msg,
                           adapter_tx_packet_t *out_packet)
{
    (void)ctx;
    (void)msg;
    (void)out_packet;
    return -1;
}

static int can_fragment_tx(void *ctx,
                           const anymsg_buffer_t *msg,
                           adapter_tx_packet_list_t *out_packets)
{
    static can_tx_context_t fallback_ctx;
    static bool fallback_initialized = false;
    can_tx_context_t *tx_ctx;
    uint8_t data_frame_count;
    uint16_t crc;
    uint16_t msg_len;
    canid_t can_id;
    uint8_t session_id;

    if ((msg == 0) || (msg->data == 0) || (out_packets == 0) ||
        (msg->len < ANYMSG_HEADER_SIZE) ||
        (msg->len > PUT_SHM_FRAME_POOL_BLOCK_SIZE)) {
        return -1;
    }

    if (!fallback_initialized) {
        can_tx_context_init(&fallback_ctx, CAN_ADAPTER_DEFAULT_TX_CAN_ID, false);
        fallback_initialized = true;
    }

    tx_ctx = (ctx == 0) ? &fallback_ctx : (can_tx_context_t *)ctx;
    msg_len = (uint16_t)msg->len;
    data_frame_count = expected_data_frame_count(msg_len);
    if (((size_t)data_frame_count + 1u) > CAN_ADAPTER_TX_MAX_PACKETS) {
        return -1;
    }

    crc = unified_crc16_ccitt_false(msg->data, msg->len);
    session_id = tx_ctx->next_session_id++;
    can_id = (canid_t)(tx_ctx->tx_can_id & (tx_ctx->extended_id ? CAN_EFF_MASK : CAN_SFF_MASK));
    if (tx_ctx->extended_id) {
        can_id |= CAN_EFF_FLAG;
    }

    memset(tx_ctx->frames, 0, sizeof(tx_ctx->frames));
    memset(tx_ctx->packets, 0, sizeof(tx_ctx->packets));

    tx_ctx->frames[0].can_id = can_id;
    tx_ctx->frames[0].can_dlc = CAN_ADAPTER_CLASSIC_DLC;
    tx_ctx->frames[0].data[0] = CAN_ADAPTER_FRAME_KIND_SOF;
    tx_ctx->frames[0].data[1] = session_id;
    tx_ctx->frames[0].data[2] = (uint8_t)(msg_len & 0xFFu);
    tx_ctx->frames[0].data[3] = (uint8_t)((msg_len >> 8u) & 0xFFu);
    tx_ctx->frames[0].data[4] = (uint8_t)(crc & 0xFFu);
    tx_ctx->frames[0].data[5] = (uint8_t)((crc >> 8u) & 0xFFu);
    tx_ctx->frames[0].data[6] = ((msg_len % CAN_ADAPTER_DATA_PAYLOAD_SIZE) == 0u) ? 0u : 1u;
    tx_ctx->frames[0].data[7] = data_frame_count;

    tx_ctx->packets[0].data = (const uint8_t *)&tx_ctx->frames[0];
    tx_ctx->packets[0].len = sizeof(struct can_frame);

    for (uint8_t seq = 0u; seq < data_frame_count; ++seq) {
        size_t offset;
        size_t copy_len;
        struct can_frame *frame;
        adapter_tx_packet_t *packet;

        offset = (size_t)seq * CAN_ADAPTER_DATA_PAYLOAD_SIZE;
        copy_len = msg->len - offset;
        if (copy_len > CAN_ADAPTER_DATA_PAYLOAD_SIZE) {
            copy_len = CAN_ADAPTER_DATA_PAYLOAD_SIZE;
        }

        frame = &tx_ctx->frames[(size_t)seq + 1u];
        frame->can_id = can_id;
        frame->can_dlc = CAN_ADAPTER_CLASSIC_DLC;
        frame->data[0] = CAN_ADAPTER_FRAME_KIND_DATA;
        frame->data[1] = session_id;
        frame->data[2] = seq;
        memcpy(&frame->data[3], msg->data + offset, copy_len);

        packet = &tx_ctx->packets[(size_t)seq + 1u];
        packet->data = (const uint8_t *)frame;
        packet->len = sizeof(struct can_frame);
    }

    out_packets->packets = tx_ctx->packets;
    out_packets->count = (size_t)data_frame_count + 1u;
    return 0;
}

static int can_send(void *ctx, const adapter_tx_packet_t *packet)
{
    can_tx_context_t *tx_ctx;
    int fd;
    ssize_t sent;

    if ((packet == 0) || (packet->data == 0) || (packet->len != sizeof(struct can_frame))) {
        return -1;
    }

    tx_ctx = (can_tx_context_t *)ctx;
    fd = (tx_ctx == 0) ? -1 : tx_ctx->socket_fd;
    if (fd < 0) {
        (void)pthread_mutex_lock(&g_can_state.lock);
        fd = g_can_state.socket_fd;
        (void)pthread_mutex_unlock(&g_can_state.lock);
    }
    if (fd < 0) {
        record_error(&g_can_state.status, g_can_state.config.collector,
                     "can_send_offline", UNIFIED_ERR_IPC_OFFLINE);
        return -1;
    }

    sent = write(fd, packet->data, packet->len);
    if (sent != (ssize_t)packet->len) {
        record_error(&g_can_state.status, g_can_state.config.collector,
                     "can_send", UNIFIED_ERR_INVALID_ARG);
        return -1;
    }

    g_can_state.status.tx_frames++;
    g_can_state.status.tx_bytes += (uint64_t)packet->len;
    g_can_state.status.last_tx_ms = now_monotonic_ms();
    g_can_state.status.updated_at_ms = g_can_state.status.last_tx_ms;
    return 0;
}

static int can_status(void *ctx, void *out_status)
{
    can_status_t *status;

    if (out_status == 0) {
        return -1;
    }

    (void)ctx;
    status = (can_status_t *)out_status;
    *status = g_can_state.status;
    return 0;
}

static void set_socket_state(can_adapter_state_t *state,
                             int socket_fd,
                             bool socket_open,
                             bool interface_online)
{
    (void)pthread_mutex_lock(&state->lock);
    state->socket_fd = socket_fd;
    state->status.socket_open = socket_open;
    state->status.interface_online = interface_online;
    state->status.updated_at_ms = now_monotonic_ms();
    (void)pthread_mutex_unlock(&state->lock);
}

static int take_socket_fd(can_adapter_state_t *state)
{
    int socket_fd;

    (void)pthread_mutex_lock(&state->lock);
    socket_fd = state->socket_fd;
    state->socket_fd = -1;
    state->status.socket_open = false;
    state->status.interface_online = false;
    state->status.updated_at_ms = now_monotonic_ms();
    (void)pthread_mutex_unlock(&state->lock);
    return socket_fd;
}

static bool should_stop(can_adapter_state_t *state)
{
    bool stop_requested;

    (void)pthread_mutex_lock(&state->lock);
    stop_requested = state->stop_requested;
    (void)pthread_mutex_unlock(&state->lock);
    return stop_requested;
}

static int open_can_socket(const can_adapter_config_t *config,
                           can_status_t *status,
                           status_collector_t *collector)
{
    int fd;
    struct ifreq ifr;
    struct sockaddr_can addr;
    struct can_filter filter;
    can_err_mask_t err_mask;
    struct timeval timeout;

    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        record_error(status, collector, "can_socket", UNIFIED_ERR_IPC_NOT_READY);
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    if (strlen(config->ifname) >= sizeof(ifr.ifr_name)) {
        record_error(status, collector, "can_bind_ifname", UNIFIED_ERR_INVALID_ARG);
        (void)close(fd);
        return -1;
    }
    memcpy(ifr.ifr_name, config->ifname, strlen(config->ifname) + 1u);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) != 0) {
        record_error(status, collector, "can_bind_ifindex", UNIFIED_ERR_IPC_NOT_READY);
        (void)close(fd);
        return -1;
    }

    memset(&filter, 0, sizeof(filter));
    filter.can_id = (canid_t)(config->rx_filter_id & (config->extended_id ? CAN_EFF_MASK : CAN_SFF_MASK));
    filter.can_mask = (canid_t)(config->rx_filter_mask & (config->extended_id ? CAN_EFF_MASK : CAN_SFF_MASK));
    if (config->extended_id) {
        filter.can_id |= CAN_EFF_FLAG;
    }
    filter.can_mask |= CAN_EFF_FLAG | CAN_RTR_FLAG;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) != 0) {
        record_error(status, collector, "can_socket_filter", UNIFIED_ERR_INVALID_ARG);
        (void)close(fd);
        return -1;
    }

    err_mask = CAN_ERR_BUSOFF | CAN_ERR_CRTL | CAN_ERR_PROT | CAN_ERR_RESTARTED;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof(err_mask)) != 0) {
        record_error(status, collector, "can_socket_err_filter", UNIFIED_ERR_INVALID_ARG);
    }

    timeout.tv_sec = 0;
    timeout.tv_usec = CAN_ADAPTER_SOCKET_TIMEOUT_US;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        record_error(status, collector, "can_bind", UNIFIED_ERR_IPC_NOT_READY);
        (void)close(fd);
        return -1;
    }

    return fd;
}

static const char *ipc_stage_from_result(unified_error_t err)
{
    switch (err) {
    case UNIFIED_ERR_IPC_FRAME_POOL_FULL:
        return "can_ipc_frame_pool_full";
    case UNIFIED_ERR_IPC_QUEUE_FULL:
        return "can_ipc_rx_ring_full";
    default:
        return "can_ipc_commit";
    }
}

static unified_error_t handle_complete_msg(can_adapter_state_t *state,
                                           const uint8_t *frame,
                                           size_t frame_len)
{
    adapter_rx_result_t rx;
    unified_error_t err;

    err = can_adapter_decode_anymsg(frame, frame_len, &rx);
    if (err != UNIFIED_OK) {
        record_error(&state->status, state->config.collector, "can_decode", err);
        return err;
    }

    err = can_adapter_submit_to_ipc(state->config.ipc, frame, &rx, state->config.linux_epoch);
    if (err != UNIFIED_OK) {
        record_error(&state->status, state->config.collector, ipc_stage_from_result(err), err);
        return err;
    }

    record_rx_ok(&state->status, state->config.collector, frame_len);
    return UNIFIED_OK;
}

static bool handle_error_frame(can_adapter_state_t *state, const struct can_frame *frame)
{
    if ((frame->can_id & CAN_ERR_BUSOFF) != 0u) {
        state->status.bus_off = true;
        state->status.interface_online = false;
        clear_all_sessions(&state->reassembly);
        record_error(&state->status,
                     state->config.collector,
                     "can_socket_bus_off",
                     UNIFIED_ERR_IPC_OFFLINE);
        return true;
    }

    record_error(&state->status,
                 state->config.collector,
                 "can_socket_error_frame",
                 UNIFIED_ERR_IPC_OFFLINE);
    return false;
}

static bool handle_socket_frame(can_adapter_state_t *state, const struct can_frame *frame)
{
    anymsg_buffer_t complete_msg;
    unified_error_t err;

    if ((frame->can_id & CAN_ERR_FLAG) != 0u) {
        return handle_error_frame(state, frame);
    }

    err = can_adapter_reassemble_frame(&state->reassembly,
                                       frame,
                                       now_monotonic_ms(),
                                       &complete_msg);
    if ((err == UNIFIED_OK) && (complete_msg.data != 0) && (complete_msg.len != 0u)) {
        (void)handle_complete_msg(state, complete_msg.data, complete_msg.len);
    }

    return false;
}

static uint32_t next_retry_delay(uint32_t current_ms)
{
    if (current_ms < CAN_ADAPTER_RETRY_SECOND_MS) {
        return CAN_ADAPTER_RETRY_SECOND_MS;
    }
    return CAN_ADAPTER_RETRY_MAX_MS;
}

static void *can_rx_thread(void *arg)
{
    can_adapter_state_t *state;
    uint32_t retry_delay_ms;

    state = (can_adapter_state_t *)arg;
    retry_delay_ms = CAN_ADAPTER_RETRY_FIRST_MS;

    state->status.running = true;
    state->status.updated_at_ms = now_monotonic_ms();
    if (state->config.collector != 0) {
        status_collector_mark_running(state->config.collector, STATUS_MODULE_CAN);
    }

    while (!should_stop(state)) {
        int fd;

        fd = open_can_socket(&state->config, &state->status, state->config.collector);
        if (fd < 0) {
            set_socket_state(state, -1, false, false);
            sleep_ms(retry_delay_ms);
            retry_delay_ms = next_retry_delay(retry_delay_ms);
            continue;
        }

        state->status.bus_off = false;
        set_socket_state(state, fd, true, true);
        retry_delay_ms = CAN_ADAPTER_RETRY_FIRST_MS;

        while (!should_stop(state)) {
            struct can_frame frame;
            ssize_t received;

            memset(&frame, 0, sizeof(frame));
            received = read(fd, &frame, sizeof(frame));
            if (received < 0) {
                if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) {
                    can_adapter_reassembly_scan_timeouts(&state->reassembly, now_monotonic_ms());
                    continue;
                }
                if ((errno == ENETDOWN) || (errno == ENODEV) || (errno == EBADF)) {
                    state->status.interface_online = false;
                    record_error(&state->status,
                                 state->config.collector,
                                 "can_socket_offline",
                                 UNIFIED_ERR_IPC_OFFLINE);
                    break;
                }
                record_error(&state->status,
                             state->config.collector,
                             "can_socket_read",
                             UNIFIED_ERR_INVALID_ARG);
                continue;
            }

            if (received != (ssize_t)sizeof(frame)) {
                record_error(&state->status,
                             state->config.collector,
                             "can_fragment_short_read",
                             UNIFIED_ERR_LENGTH);
                continue;
            }

            if (handle_socket_frame(state, &frame)) {
                break;
            }
        }

        fd = take_socket_fd(state);
        if (fd >= 0) {
            (void)close(fd);
        }
        if (!should_stop(state)) {
            sleep_ms(retry_delay_ms);
            retry_delay_ms = next_retry_delay(retry_delay_ms);
        }
    }

    state->status.running = false;
    state->status.socket_open = false;
    state->status.interface_online = false;
    state->status.updated_at_ms = now_monotonic_ms();
    if (state->config.collector != 0) {
        status_collector_mark_stopped(state->config.collector, STATUS_MODULE_CAN, "can rx stopped");
    }
    return 0;
}

static bool can_id_is_valid(uint32_t can_id, bool extended_id)
{
    return extended_id ? (can_id <= CAN_EFF_MASK) : (can_id <= CAN_SFF_MASK);
}

static int validate_config(const can_adapter_config_t *config)
{
    if ((config == 0) || (config->ipc == 0) || (config->linux_epoch == 0u) ||
        (config->ifname[0] == '\0') || (config->bitrate == 0u)) {
        return -1;
    }

    if (!can_id_is_valid(config->tx_can_id, config->extended_id) ||
        !can_id_is_valid(config->rx_filter_id, config->extended_id) ||
        !can_id_is_valid(config->rx_filter_mask, config->extended_id)) {
        return -1;
    }

    if ((config->reassembly_timeout_ms < 100u) ||
        (config->reassembly_timeout_ms > 5000u)) {
        return -1;
    }

    return 0;
}

int can_adapter_start(const can_adapter_config_t *config)
{
    uint64_t now_ms;

    if (validate_config(config) != 0) {
        return -1;
    }

    (void)pthread_mutex_lock(&g_can_state.lock);
    if (g_can_state.thread_started) {
        (void)pthread_mutex_unlock(&g_can_state.lock);
        return 0;
    }

    g_can_state.config = *config;
    g_can_state.stop_requested = false;
    g_can_state.socket_fd = -1;
    memset(&g_can_state.status, 0, sizeof(g_can_state.status));
    now_ms = now_monotonic_ms();
    g_can_state.status.enabled = config->enabled;
    g_can_state.status.started_at_ms = now_ms;
    g_can_state.status.updated_at_ms = now_ms;
    (void)snprintf(g_can_state.status.ifname,
                   sizeof(g_can_state.status.ifname),
                   "%s",
                   config->ifname);
    can_reassembly_context_init(&g_can_state.reassembly,
                                config->reassembly_timeout_ms,
                                &g_can_state.status,
                                config->collector);
    (void)pthread_mutex_unlock(&g_can_state.lock);

    if (pthread_create(&g_can_state.thread, 0, can_rx_thread, &g_can_state) != 0) {
        record_error(&g_can_state.status,
                     config->collector,
                     "can_pthread_create",
                     UNIFIED_ERR_IPC_NOT_READY);
        return -1;
    }

    (void)pthread_mutex_lock(&g_can_state.lock);
    g_can_state.thread_started = true;
    (void)pthread_mutex_unlock(&g_can_state.lock);
    return 0;
}

void can_adapter_stop(void)
{
    int fd;
    bool thread_started;

    (void)pthread_mutex_lock(&g_can_state.lock);
    thread_started = g_can_state.thread_started;
    g_can_state.stop_requested = true;
    (void)pthread_mutex_unlock(&g_can_state.lock);

    fd = take_socket_fd(&g_can_state);
    if (fd >= 0) {
        (void)close(fd);
    }

    if (thread_started) {
        (void)pthread_join(g_can_state.thread, 0);
    }

    (void)pthread_mutex_lock(&g_can_state.lock);
    g_can_state.thread_started = false;
    (void)pthread_mutex_unlock(&g_can_state.lock);
}

physical_interface_adapter_t can_adapter = {
    "can",
    (uint8_t)PUT_SHM_INTERFACE_CAN,
    can_get_mtu,
    can_decode_rx,
    can_reassemble,
    can_encapsulate,
    can_fragment_tx,
    can_send,
    can_status,
};
