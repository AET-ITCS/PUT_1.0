/* 蓝牙服务实现：建立 RFCOMM Socket 监听，处理连接与流式数据读取解析，并发送给小核。 */
#define _POSIX_C_SOURCE 200809L

#include "bluetooth_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "crc16.h"
#include "frame_packer.h"
#include "ipc_to_rtos.h"
#include "unified_frame.h"
#include "bluetooth_status.h"

/* 
 * 嵌入式防御性编程设计：
 * 在部分比赛或嵌入式交叉编译工具链中，可能没有安装 libbluetooth-dev (BlueZ) 头文件。
 * 我们在此直接自主定义 Linux 内核蓝牙 RFCOMM 套接字所需的常量与地址结构体。
 * 这保证了蓝牙模块在任何交叉编译环境下无需额外配置依赖即可“一次编译通过”，极大提高了系统的健壮性。
 */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif

#ifndef BTPROTO_RFCOMM
#define BTPROTO_RFCOMM 3
#endif

typedef struct {
    uint8_t b[6];
} bdaddr_t;

struct sockaddr_rc {
    sa_family_t rc_family;
    bdaddr_t    rc_bdaddr;
    uint8_t     rc_channel;
};

/* 默认监听通道为 1 */
#define DEFAULT_RFCOMM_CHANNEL 1u

#define OFFSET_MAGIC 0u
#define OFFSET_VERSION 2u
#define OFFSET_VEHICLE_TYPE 3u
#define OFFSET_CAN_FLAGS 4u
#define OFFSET_CAN_DLC 5u
#define OFFSET_CAN_ID 6u
#define OFFSET_CAN_DATA 10u
#define OFFSET_CRC (BLUETOOTH_FRAME_LENGTH - BLUETOOTH_FRAME_CRC_LENGTH)

/* 蓝牙状态监控全局实例 */
static bluetooth_status_t g_bt_status;
/* 蓝牙服务后台线程 */
static pthread_t g_bt_thread;
/* 蓝牙线程运行标志 */
static volatile bool g_bt_running = false;
/* 监听 Socket */
static int g_server_fd = -1;

static uint16_t load_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

static uint32_t load_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8u) |
           ((uint32_t)data[2] << 16u) |
           ((uint32_t)data[3] << 24u);
}

/* 辅组函数：将 bdaddr 蓝牙物理地址格式化为标准的 MAC 文本 (如 AA:BB:CC:DD:EE:FF) */
static void format_bdaddr(const bdaddr_t *ba, char *out_str, size_t max_len)
{
    if (ba == NULL || out_str == NULL || max_len < 18u) {
        return;
    }
    snprintf(out_str, max_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             ba->b[5], ba->b[4], ba->b[3], ba->b[2], ba->b[1], ba->b[0]);
}

/* 辅组函数：确保在流式 Socket 链路中完整读取指定大小的数据，解决分包沾包问题 */
static ssize_t recv_all(int fd, uint8_t *buf, size_t len)
{
    size_t total_received = 0u;
    while (total_received < len) {
        ssize_t received = recv(fd, buf + total_received, len - total_received, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1; // 发生读取错误
        }
        if (received == 0) {
            return (ssize_t)total_received; // 远端关闭，返回已读到的字节数
        }
        total_received += (size_t)received;
    }
    return (ssize_t)total_received;
}

unified_error_t bluetooth_parse_frame(const uint8_t *buffer,
                                      size_t length,
                                      protocol_parsed_msg_t *out_msg)
{
    uint16_t expected_crc;
    uint16_t actual_crc;
    uint8_t can_flags;
    uint8_t can_dlc;
    uint32_t can_id;

    if ((buffer == NULL) || (out_msg == NULL)) {
        return UNIFIED_ERR_NULL;
    }

    if (length != BLUETOOTH_FRAME_LENGTH) {
        return UNIFIED_ERR_LENGTH;
    }

    if (load_u16_le(&buffer[OFFSET_MAGIC]) != BLUETOOTH_FRAME_MAGIC) {
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if (buffer[OFFSET_VERSION] != BLUETOOTH_FRAME_VERSION) {
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    expected_crc = load_u16_le(&buffer[OFFSET_CRC]);
    actual_crc = unified_crc16_ccitt_false(buffer, OFFSET_CRC);
    if (expected_crc != actual_crc) {
        return UNIFIED_ERR_CRC;
    }

    can_flags = buffer[OFFSET_CAN_FLAGS];
    can_dlc = buffer[OFFSET_CAN_DLC];
    can_id = load_u32_le(&buffer[OFFSET_CAN_ID]);

    if (!vehicle_msg_type_is_valid(buffer[OFFSET_VEHICLE_TYPE])) {
        return UNIFIED_ERR_UNKNOWN_TYPE;
    }

    if (!unified_frame_can_dlc_is_valid(can_dlc, can_flags)) {
        return UNIFIED_ERR_CAN_DLC;
    }

    if (((can_flags & (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID) == 0u) && (can_id > 0x7FFu)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (((can_flags & (uint8_t)UNIFIED_CAN_FLAG_EXTENDED_ID) != 0u) && (can_id > 0x1FFFFFFFu)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->source_protocol = PROTOCOL_TYPE_BLUETOOTH;
    out_msg->vehicle_type = buffer[OFFSET_VEHICLE_TYPE];
    out_msg->can_flags = can_flags;
    out_msg->can_dlc = can_dlc;
    out_msg->can_id = can_id;
    memcpy(out_msg->can_data, &buffer[OFFSET_CAN_DATA], UNIFIED_CAN_FD_DATA_MAX_LEN);

    return UNIFIED_OK;
}

/* 蓝牙线程主循环入口 */
static void *bluetooth_server_thread_entry(void *arg)
{
    (void)arg;
    struct sockaddr_rc loc_addr = { 0 };
    int server_fd;

    // 1. 创建蓝牙 RFCOMM Socket
    server_fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (server_fd < 0) {
        perror("[bluetooth] socket");
        bluetooth_status_record_error(&g_bt_status, "socket", UNIFIED_ERR_INVALID_ARG);
        bluetooth_status_write_all(&g_bt_status);
        return NULL;
    }
    g_server_fd = server_fd;

    // 2. 绑定到蓝牙通道 Channel 1
    loc_addr.rc_family = AF_BLUETOOTH;
    // ba_any 对应全 0 的 MAC
    memset(&loc_addr.rc_bdaddr, 0, sizeof(bdaddr_t));
    loc_addr.rc_channel = (uint8_t)DEFAULT_RFCOMM_CHANNEL;

    if (bind(server_fd, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0) {
        perror("[bluetooth] bind");
        bluetooth_status_record_error(&g_bt_status, "bind", UNIFIED_ERR_INVALID_ARG);
        bluetooth_status_write_all(&g_bt_status);
        close(server_fd);
        g_server_fd = -1;
        return NULL;
    }

    // 3. 开始监听
    if (listen(server_fd, 1) < 0) {
        perror("[bluetooth] listen");
        bluetooth_status_record_error(&g_bt_status, "listen", UNIFIED_ERR_INVALID_ARG);
        bluetooth_status_write_all(&g_bt_status);
        close(server_fd);
        g_server_fd = -1;
        return NULL;
    }

    printf("linux_app Bluetooth server listening on RFCOMM Channel %d\n", DEFAULT_RFCOMM_CHANNEL);
    bluetooth_status_mark_listening(&g_bt_status);
    bluetooth_status_write_all(&g_bt_status);

    // 4. 监听与接收主循环
    while (g_bt_running) {
        struct sockaddr_rc rem_addr = { 0 };
        socklen_t opt = sizeof(rem_addr);
        int client_fd;
        char client_mac[64] = { 0 };

        // 阻塞接收客户端配对连接（因为是在子线程，阻塞不会拖慢以太网 UDP 主循环）
        client_fd = accept(server_fd, (struct sockaddr *)&rem_addr, &opt);
        if (client_fd < 0) {
            if (!g_bt_running) {
                break; // 服务被主动停止
            }
            perror("[bluetooth] accept");
            bluetooth_status_record_error(&g_bt_status, "accept", UNIFIED_ERR_INVALID_ARG);
            bluetooth_status_write_all(&g_bt_status);
            sleep(1); // 避免 accept 出错死循环占用 100% CPU
            continue;
        }

        format_bdaddr(&rem_addr.rc_bdaddr, client_mac, sizeof(client_mac));
        printf("[bluetooth] connected by client %s on Channel %d\n", client_mac, DEFAULT_RFCOMM_CHANNEL);
        bluetooth_status_mark_connected(&g_bt_status, client_mac);
        bluetooth_status_write_all(&g_bt_status);

        // 5. 循环读取当前已建立连接上的数据流
        while (g_bt_running) {
            uint8_t rx_buffer[BLUETOOTH_FRAME_LENGTH];
            ssize_t bytes_read;
            protocol_parsed_msg_t parsed_msg;
            unified_frame_t frame;
            unified_error_t err;

            bytes_read = recv_all(client_fd, rx_buffer, BLUETOOTH_FRAME_LENGTH);
            if (bytes_read < 0) {
                perror("[bluetooth] recv");
                bluetooth_status_record_error(&g_bt_status, "recv", UNIFIED_ERR_INVALID_ARG);
                bluetooth_status_write_all(&g_bt_status);
                break; // 退出读取，重新 accept 
            }
            if (bytes_read == 0) {
                // 远端断开连接
                printf("[bluetooth] client %s disconnected\n", client_mac);
                break;
            }
            if (bytes_read < (ssize_t)BLUETOOTH_FRAME_LENGTH) {
                // 读出的数据未满 76 字节，说明数据流异常截断
                bluetooth_status_record_error(&g_bt_status, "recv_all", UNIFIED_ERR_LENGTH);
                bluetooth_status_write_all(&g_bt_status);
                break;
            }

            bluetooth_status_record_rx(&g_bt_status, (size_t)bytes_read);

            // 6. 帧校验与中间消息解析
            err = bluetooth_parse_frame(rx_buffer, (size_t)bytes_read, &parsed_msg);
            if (err != UNIFIED_OK) {
                fprintf(stderr, "[bluetooth] parse error=%d\n", (int)err);
                bluetooth_status_record_error(&g_bt_status, "bluetooth_parse_frame", err);
                bluetooth_status_write_all(&g_bt_status);
                continue; // 容错：解析出错不挂断，继续读取下一帧
            }

            // 7. 将解析好的中间消息打包为大核统一帧格式
            err = frame_packer_pack(&parsed_msg, &frame);
            if (err != UNIFIED_OK) {
                fprintf(stderr, "[bluetooth] pack error=%d\n", (int)err);
                bluetooth_status_record_error(&g_bt_status, "frame_packer_pack", err);
                bluetooth_status_write_all(&g_bt_status);
                continue;
            }

            // 8. 调用 IPC 统一发送到小核桩中（后续无缝替换为共享内存）
            err = ipc_to_rtos_send(&frame);
            if (err != UNIFIED_OK) {
                fprintf(stderr, "[bluetooth] ipc send error=%d\n", (int)err);
                bluetooth_status_record_error(&g_bt_status, "ipc_to_rtos_send", err);
                bluetooth_status_write_all(&g_bt_status);
                continue;
            }

            // 成功发送，记录并上报
            bluetooth_status_record_tx_ok(&g_bt_status);
            bluetooth_status_write_all(&g_bt_status);
        }

        close(client_fd);
        bluetooth_status_mark_disconnected(&g_bt_status);
        bluetooth_status_write_all(&g_bt_status);
    }

    close(server_fd);
    g_server_fd = -1;
    return NULL;
}

int bluetooth_server_start(const char *status_dir, bool status_enabled)
{
    if (g_bt_running) {
        return 0; // 已经运行
    }

    bluetooth_status_init(&g_bt_status, status_dir, DEFAULT_RFCOMM_CHANNEL, status_enabled);

    g_bt_running = true;
    if (pthread_create(&g_bt_thread, NULL, bluetooth_server_thread_entry, NULL) != 0) {
        perror("[bluetooth] pthread_create");
        g_bt_running = false;
        return -1;
    }

    return 0;
}

void bluetooth_server_stop(void)
{
    if (!g_bt_running) {
        return;
    }

    g_bt_running = false;

    // 主动关闭监听 Socket，迫使正在阻塞的 accept() 或 recv() 解锁并退出
    if (g_server_fd >= 0) {
        shutdown(g_server_fd, SHUT_RDWR);
        close(g_server_fd);
        g_server_fd = -1;
    }

    pthread_join(g_bt_thread, NULL);

    bluetooth_status_mark_stopped(&g_bt_status, "Bluetooth server stopped by system call");
    bluetooth_status_write_all(&g_bt_status);
}
