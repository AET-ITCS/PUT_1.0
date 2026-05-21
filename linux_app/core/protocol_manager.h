/* 协议管理接口：负责调度外部协议接收、解析、打包和发送流程。 */
#ifndef PROTOCOL_MANAGER_H
#define PROTOCOL_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 运行以太网 UDP 协议转换主循环。
 *
 * @param port UDP 监听端口。
 * @param max_packets 最大处理包数，0 表示一直运行。
 */
unified_error_t protocol_manager_run_udp(uint16_t port, uint32_t max_packets);

/**
 * @brief 运行以太网 UDP 协议转换主循环，并输出 Web 业务状态快照。
 *
 * @param port UDP 监听端口。
 * @param max_packets 最大处理包数，0 表示一直运行。
 * @param status_dir 快照目录；NULL 或空字符串表示 /run/put/status。
 * @param status_enabled 是否写出 modules/ipc/can/events 快照。
 */
unified_error_t protocol_manager_run_udp_with_status(uint16_t port,
                                                     uint32_t max_packets,
                                                     const char *status_dir,
                                                     bool status_enabled);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_MANAGER_H */
