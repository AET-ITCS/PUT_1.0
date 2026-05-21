/* 蓝牙协议解析与低级网络接口 */
#ifndef BLUETOOTH_SERVER_H
#define BLUETOOTH_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef _WIN32
#include <basetsd.h>
typedef intptr_t ssize_t;
typedef unsigned short sa_family_t;
#endif

#include "error_code.h"
#include "protocol_parsed_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLUETOOTH_FRAME_MAGIC 0x55AAu
#define BLUETOOTH_FRAME_VERSION 0x01u
#define BLUETOOTH_FRAME_HEADER_LENGTH 10u
#define BLUETOOTH_FRAME_CRC_LENGTH 2u
#define BLUETOOTH_FRAME_LENGTH \
    (BLUETOOTH_FRAME_HEADER_LENGTH + UNIFIED_CAN_FD_DATA_MAX_LEN + BLUETOOTH_FRAME_CRC_LENGTH)

/* 
 * 嵌入式防御性编程设计：
 * 自主定义 Linux 内核蓝牙 RFCOMM 套接字所需的常量与地址结构体。
 * 保证蓝牙模块在任何交叉编译环境下无需额外配置依赖即可“一次编译通过”。
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

/**
 * @brief 将 bdaddr 蓝牙物理地址格式化为标准的 MAC 文本 (如 AA:BB:CC:DD:EE:FF)
 */
void format_bdaddr(const bdaddr_t *ba, char *out_str, size_t max_len);

/**
 * @brief 确保在流式 Socket 链路中完整读取指定大小的数据，解决分包沾包问题
 */
ssize_t recv_all(int fd, uint8_t *buf, size_t len);

/**
 * @brief 解析大核蓝牙接收到的原始二进制帧。
 *
 * 固定 76 字节，小端字段：
 * magic(2), version(1), vehicle_type(1), can_flags(1), can_dlc(1),
 * can_id(4), can_data(64), crc16(2)。
 *
 * @param buffer 输入缓冲区
 * @param length 数据长度
 * @param out_msg 输出的协议解析中间消息结构
 * @return unified_error_t 错误码，UNIFIED_OK 表示解析成功
 */
unified_error_t bluetooth_parse_frame(const uint8_t *buffer,
                                      size_t length,
                                      protocol_parsed_msg_t *out_msg);

#ifdef __cplusplus
}
#endif

#endif /* BLUETOOTH_SERVER_H */
