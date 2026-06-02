#include "bluetooth_adapter.h"
#include "physical_interface_adapter.h"
#include "status_collector.h"
#include "shared_memory_ipc.h"
#include "anymsg_frame.h"
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>

/* Fallback definitions for Linux Bluetooth sockets to avoid libbluetooth-dev dependency */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif

#ifndef BTPROTO_RFCOMM
#define BTPROTO_RFCOMM 3
#endif

typedef struct {
    uint8_t b[6];
} __attribute__((packed)) bdaddr_t;

struct sockaddr_rc {
    sa_family_t rc_family;
    bdaddr_t    rc_bdaddr;
    uint8_t     rc_channel;
};

static inline void bt_mac_to_str(const bdaddr_t *ba, char *str) {
    sprintf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
            ba->b[5], ba->b[4], ba->b[3], ba->b[2], ba->b[1], ba->b[0]);
}

/**
 * @brief 蓝牙模块内部上下文结构体
 */
typedef struct {
    int listen_fd;
    int client_fd;
    pthread_t thread;
    bool running;
    bluetooth_config_t config;
    unified_error_t last_tx_error;
    const char *last_tx_error_stage;
    uint64_t rx_count;
    uint64_t tx_count;
    uint64_t rx_bytes;
    uint64_t started_at_ms;
    uint64_t last_seen_ms;
    uint64_t last_tx_ms;
    char connected_client_addr[128];
} bt_ctx_t;

static bt_ctx_t g_bt_ctx = { .listen_fd = -1, .client_fd = -1, .running = false };

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

static uint16_t read_le16(const uint8_t bytes[2])
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
}

static void bt_tx_record_error(bt_ctx_t *ctx, const char *stage, unified_error_t err) {
    if (ctx) {
        ctx->last_tx_error_stage = stage;
        ctx->last_tx_error = err;
    }
}

/* ---------- 适配器回调接口实现 ---------- */

static size_t bluetooth_get_mtu(void *ctx) {
    (void)ctx;
    return PUT_SHM_FRAME_POOL_BLOCK_SIZE;
}

static int bluetooth_decode_rx(void *ctx, const uint8_t *input, size_t input_len, adapter_rx_result_t *out) {
    const anymsg_header_t *header;
    uint16_t msg_length;
    uint16_t payload_length;
    unified_error_t err;

    if ((input == 0) || (out == 0)) return -1;
    if (input_len < ANYMSG_HEADER_SIZE || input_len > PUT_SHM_FRAME_POOL_BLOCK_SIZE) return -1;

    header = (const anymsg_header_t *)input;
    msg_length = read_le16(header->msg_length);
    payload_length = read_le16(header->payload_length);

    err = anymsg_validate_normalized_lengths(msg_length, payload_length, input_len);
    if (err != UNIFIED_OK) return -1;

    err = anymsg_validate_header_static_fields(header);
    if (err != UNIFIED_OK) return -1;

    if (anymsg_cid_segment_from_first_byte(anymsg_source_cid_first_byte(header)) != ANYMSG_CID_SEGMENT_BLUETOOTH) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->data = input;
    out->len = input_len;
    out->msg_length = msg_length;
    out->payload_length = payload_length;
    memcpy(out->source_cid, header->source_cid, ANYMSG_CID_LENGTH);
    memcpy(out->destination_cid, header->destination_cid, ANYMSG_CID_LENGTH);
    out->type = header->type;

    (void)ctx;
    return 0;
}

static int bluetooth_reassemble(void *ctx, const adapter_fragment_t *fragment, anymsg_buffer_t *out_complete_msg) {
    adapter_rx_result_t rx;
    if ((fragment == 0) || (out_complete_msg == 0)) return -1;
    if (bluetooth_decode_rx(ctx, fragment->data, fragment->len, &rx) != 0) return -1;
    out_complete_msg->data = fragment->data;
    out_complete_msg->len = fragment->len;
    return 0;
}

static int bluetooth_encapsulate(void *ctx, const anymsg_buffer_t *msg, adapter_tx_packet_t *out_packet) {
    if ((msg == 0) || (out_packet == 0) || (msg->data == 0) || (msg->len == 0u)) return -1;
    (void)ctx;
    out_packet->data = msg->data;
    out_packet->len = msg->len;
    return 0;
}

static int bluetooth_fragment_tx(void *ctx, const anymsg_buffer_t *msg, adapter_tx_packet_list_t *out_packets) {
    static adapter_tx_packet_t packet;
    if ((msg == 0) || (out_packets == 0) || (msg->data == 0) || (msg->len == 0u) || (msg->len > PUT_SHM_FRAME_POOL_BLOCK_SIZE)) return -1;
    (void)ctx;
    packet.data = msg->data;
    packet.len = msg->len;
    out_packets->packets = &packet;
    out_packets->count = 1u;
    return 0;
}

static int bluetooth_send(void *ctx, const adapter_tx_packet_t *packet) {
    bt_ctx_t *c = (ctx == 0) ? &g_bt_ctx : (bt_ctx_t *)ctx;
    
    if ((packet == 0) || (packet->data == 0) || (packet->len < ANYMSG_HEADER_SIZE) || (packet->len > PUT_SHM_FRAME_POOL_BLOCK_SIZE)) {
        bt_tx_record_error(c, "bt_send_packet", UNIFIED_ERR_INVALID_ARG);
        return -1;
    }

    if (c->client_fd < 0) {
        bt_tx_record_error(c, "bt_send_no_client", UNIFIED_ERR_IPC_OFFLINE);
        return -1;
    }
    
    ssize_t sent = send(c->client_fd, packet->data, packet->len, 0);
    if (sent == (ssize_t)packet->len) {
        c->tx_count++;
        c->last_tx_ms = now_ms();
        bt_tx_record_error(c, 0, UNIFIED_OK);
        return 0;
    }
    
    if (sent >= 0) {
        bt_tx_record_error(c, "bt_send_short", UNIFIED_ERR_LENGTH);
    } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == ENOBUFS)) {
        bt_tx_record_error(c, "bt_send_busy", UNIFIED_ERR_IPC_QUEUE_FULL);
    } else {
        bt_tx_record_error(c, "bt_send", UNIFIED_ERR_IPC_OFFLINE);
    }
    
    return -1;
}

static int bluetooth_get_tx_error(void *ctx, adapter_tx_error_t *out_error) {
    bt_ctx_t *c = (ctx == 0) ? &g_bt_ctx : (bt_ctx_t *)ctx;
    if (out_error == 0) return -1;
    if (c->last_tx_error_stage == 0 || c->last_tx_error_stage[0] == '\0') return -1;
    out_error->stage = c->last_tx_error_stage;
    out_error->err = c->last_tx_error;
    return 0;
}

static int bluetooth_status(void *ctx, void *out_status) {
    bt_ctx_t *c = (ctx == 0) ? &g_bt_ctx : (bt_ctx_t *)ctx;
    if (out_status == 0) return -1;
    adapter_status_t *status = (adapter_status_t *)out_status;
    memset(status, 0, sizeof(*status));
    status->name = "bluetooth";
    status->interface_id = (uint8_t)PUT_SHM_INTERFACE_BLUETOOTH;
    status->state = c->running ? (c->client_fd >= 0 ? "connected" : "listening") : "offline";
    return 0;
}

static put_shm_interface_t route_hint_from_destination_cid(const uint8_t cid[ANYMSG_CID_LENGTH])
{
    switch (anymsg_cid_segment_from_first_byte(cid[0])) {
    case ANYMSG_CID_SEGMENT_CAN: return PUT_SHM_INTERFACE_CAN;
    case ANYMSG_CID_SEGMENT_ETHERNET: return PUT_SHM_INTERFACE_ETHERNET;
    case ANYMSG_CID_SEGMENT_WIFI: return PUT_SHM_INTERFACE_WIFI;
    case ANYMSG_CID_SEGMENT_BLUETOOTH: return PUT_SHM_INTERFACE_BLUETOOTH;
    case ANYMSG_CID_SEGMENT_4G: return PUT_SHM_INTERFACE_4G;
    case ANYMSG_CID_SEGMENT_RS485: return PUT_SHM_INTERFACE_RS485;
    default: return PUT_SHM_INTERFACE_BLUETOOTH;
    }
}

static unified_error_t bluetooth_adapter_submit_to_ipc(bt_ctx_t *ctx,
                                                const uint8_t *frame,
                                                const adapter_rx_result_t *rx)
{
    uint32_t frame_id;
    uint8_t *frame_buffer;
    uint16_t frame_capacity;
    unified_error_t err;
    put_shm_interface_t target_interface;
    uint32_t descriptor_flags; /* 写入 RX descriptor 的 trust flags。 */

    if ((ctx == 0) || (ctx->config.ipc == 0) || (frame == 0) || (rx == 0)) {
        return UNIFIED_ERR_NULL;
    }

    err = linux_shm_frame_alloc(ctx->config.ipc,
                                PUT_SHM_INTERFACE_BLUETOOTH,
                                &frame_id,
                                &frame_buffer,
                                &frame_capacity);
    if (err != UNIFIED_OK) {
        return err;
    }

    if (rx->len > frame_capacity) {
        (void)linux_shm_frame_release(ctx->config.ipc, frame_id, PUT_SHM_RECLAIM_REASON_INVALID_FRAME);
        return UNIFIED_ERR_LENGTH;
    }

    memcpy(frame_buffer, frame, rx->len);
    target_interface = route_hint_from_destination_cid(rx->destination_cid);
    descriptor_flags = rx->trust_flags & PUT_SHM_DESCRIPTOR_TRUST_FLAG_MASK;
    err = linux_shm_frame_commit_rx(ctx->config.ipc,
                                    frame_id,
                                    (uint16_t)rx->len,
                                    PUT_SHM_INTERFACE_BLUETOOTH,
                                    target_interface,
                                    rx->source_cid,
                                    rx->destination_cid,
                                    rx->type,
                                    1u, // priority normal
                                    255u, // TTL
                                    ctx->config.linux_epoch,
                                    descriptor_flags);
    if (err != UNIFIED_OK) {
        (void)linux_shm_frame_release(ctx->config.ipc, frame_id, PUT_SHM_RECLAIM_REASON_QUEUE_FULL);
        return err;
    }

    return UNIFIED_OK;
}

/* ---------- 蓝牙服务后台接收线程 ---------- */

static void *bluetooth_thread(void *arg) {
    bt_ctx_t *c = (bt_ctx_t *)arg;
    
    if (c->config.collector != 0) {
        status_collector_mark_running(c->config.collector, STATUS_MODULE_BLUETOOTH);
    }

    while (c->running) {
        uint8_t len_bytes[2];
        // 阶段一：读取 2 字节的消息长度字段
        ssize_t r = recv(c->client_fd, len_bytes, 2, MSG_WAITALL);
        if (r != 2) { goto reconnect; }
        
        // 字节序转换（小端转主机字节序）
        uint16_t msg_len = read_le16(len_bytes);
        
        // 消息长度合法性校验，避免脏报文影响或内存越界
        if (msg_len < ANYMSG_HEADER_SIZE || msg_len > PUT_SHM_FRAME_POOL_BLOCK_SIZE) { goto reconnect; }
        
        uint8_t buf[PUT_SHM_FRAME_POOL_BLOCK_SIZE];
        buf[0] = len_bytes[0];
        buf[1] = len_bytes[1];
        
        // 阶段二：精准接收剩余的 bytes
        r = recv(c->client_fd, buf + 2, msg_len - 2, MSG_WAITALL);
        if (r != (ssize_t)(msg_len - 2)) { goto reconnect; }
        
        // 完整包解析与校验
        adapter_rx_result_t rx_res;
        if (bluetooth_decode_rx(c, buf, msg_len, &rx_res) == 0) {
            unified_error_t ipc_err = bluetooth_adapter_submit_to_ipc(c, buf, &rx_res);
            if (ipc_err == UNIFIED_OK) {
                c->rx_count++;
                c->rx_bytes += msg_len;
                c->last_seen_ms = now_ms();
                if (c->config.collector) {
                    status_collector_record_rx(c->config.collector, STATUS_MODULE_BLUETOOTH, msg_len);
                }
            } else if (c->config.collector) {
                status_collector_record_error(c->config.collector, STATUS_MODULE_BLUETOOTH, "bt_ipc_submit", ipc_err);
            }
        } else if (c->config.collector) {
            status_collector_record_error(c->config.collector, STATUS_MODULE_BLUETOOTH, "bt_decode", UNIFIED_ERR_INVALID_ARG);
        }
        continue;

    reconnect:
        // 当物理链路断开、读取失败或流对齐损坏时进入重连处理
        close(c->client_fd);
        c->client_fd = -1;
        
        // 阻塞等待外部蓝牙调试终端发起连接
        struct sockaddr_rc rem_addr;
        socklen_t opt = sizeof(rem_addr);
        c->client_fd = accept(c->listen_fd, (struct sockaddr *)&rem_addr, &opt);
        if (c->client_fd < 0) { sleep(1); continue; }
        
        // 将配准客户端的 MAC 地址转换为可读字符串
        bt_mac_to_str(&rem_addr.rc_bdaddr, c->connected_client_addr);
    }
    
    if (c->config.collector != 0) {
        status_collector_mark_stopped(c->config.collector, STATUS_MODULE_BLUETOOTH, "bluetooth stopped");
    }
    
    return NULL;
}

/* ---------- 外部生命周期管理 API ---------- */

int bluetooth_server_start(const bluetooth_config_t *config) {
    if (g_bt_ctx.running) return 0;
    if (config == 0) return -1;
    
    // 创建经典蓝牙 RFCOMM 协议族的套接字
    int s = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (s < 0) {
        fprintf(stderr, "bluetooth socket() failed: %s\n", strerror(errno));
        return -1;
    }
    
    struct sockaddr_rc loc_addr;
    memset(&loc_addr, 0, sizeof(loc_addr));
    loc_addr.rc_family = AF_BLUETOOTH;
    loc_addr.rc_channel = config->channel > 0 ? config->channel : 1; 
    
    if (bind(s, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0) { 
        fprintf(stderr, "bluetooth bind() on channel %d failed: %s\n", loc_addr.rc_channel, strerror(errno));
        close(s); 
        return -1; 
    }
    if (listen(s, 1) < 0) { 
        fprintf(stderr, "bluetooth listen() failed: %s\n", strerror(errno));
        close(s); 
        return -1; 
    }
    
    memset(&g_bt_ctx, 0, sizeof(g_bt_ctx));
    g_bt_ctx.config = *config;
    g_bt_ctx.listen_fd = s;
    g_bt_ctx.client_fd = -1;
    g_bt_ctx.running = true;
    g_bt_ctx.started_at_ms = now_ms();
    
    // 创建异步后台服务接收线程，保证数据切帧逻辑不阻塞主业务线程
    pthread_create(&g_bt_ctx.thread, NULL, bluetooth_thread, &g_bt_ctx);
    return 0;
}

void bluetooth_server_stop(void) {
    if (!g_bt_ctx.running) return;
    g_bt_ctx.running = false;
    
    if (g_bt_ctx.client_fd >= 0) {
        shutdown(g_bt_ctx.client_fd, SHUT_RDWR);
        close(g_bt_ctx.client_fd);
    }
    if (g_bt_ctx.listen_fd >= 0) {
        shutdown(g_bt_ctx.listen_fd, SHUT_RDWR);
        close(g_bt_ctx.listen_fd);
    }
    
    pthread_join(g_bt_ctx.thread, NULL);
}

/**
 * @brief 统一物理接口适配器全局实例 (Bluetooth)
 */
physical_interface_adapter_t bluetooth_adapter = {
    .name = "Bluetooth",
    .interface_id = (uint8_t)PUT_SHM_INTERFACE_BLUETOOTH,
    .get_mtu = bluetooth_get_mtu,
    .decode_rx = bluetooth_decode_rx,
    .reassemble = bluetooth_reassemble,
    .encapsulate = bluetooth_encapsulate,
    .fragment_tx = bluetooth_fragment_tx,
    .send = bluetooth_send,
    .get_tx_error = bluetooth_get_tx_error,
    .status = bluetooth_status
};
