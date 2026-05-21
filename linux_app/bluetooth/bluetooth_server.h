/* 蓝牙服务接口头文件：定义蓝牙服务线程启动/停止及帧解析器接口。 */
#ifndef BLUETOOTH_SERVER_H
#define BLUETOOTH_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_code.h"
#include "protocol_parsed_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 蓝牙物理链路上约定的原始调试数据帧配置，与以太网 UDP 调试帧保持格式一致 */
#define BLUETOOTH_FRAME_MAGIC 0x55AAu
#define BLUETOOTH_FRAME_VERSION 0x01u
#define BLUETOOTH_FRAME_HEADER_LENGTH 10u
#define BLUETOOTH_FRAME_CRC_LENGTH 2u
#define BLUETOOTH_FRAME_LENGTH \
    (BLUETOOTH_FRAME_HEADER_LENGTH + UNIFIED_CAN_FD_DATA_MAX_LEN + BLUETOOTH_FRAME_CRC_LENGTH)

/**
 * @brief 启动大核蓝牙服务监听子线程。
 *
 * @param status_dir 状态快照输出目录，NULL 采用默认目录
 * @param status_enabled 是否启用快照输出
 * @return int 0 启动成功，-1 启动失败
 */
int bluetooth_server_start(const char *status_dir, bool status_enabled);

/**
 * @brief 停止蓝牙监听服务，释放套接字并销毁子线程。
 */
void bluetooth_server_stop(void);

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
