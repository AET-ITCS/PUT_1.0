/* 协议管理接口：负责调度外部协议接收、解析、打包和发送流程。 */
#ifndef PROTOCOL_MANAGER_H
#define PROTOCOL_MANAGER_H

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

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_MANAGER_H */
