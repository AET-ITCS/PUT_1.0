/* 协议管理接口：负责多协议接收任务、统一打包和 IPC 发送调度。 */
#ifndef PROTOCOL_MANAGER_H
#define PROTOCOL_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

unified_error_t protocol_manager_run(const app_config_t *config);

/* 兼容旧的单 UDP 调试入口。 */
unified_error_t protocol_manager_run_udp(uint16_t port, uint32_t max_packets);
unified_error_t protocol_manager_run_udp_with_status(uint16_t port,
                                                     uint32_t max_packets,
                                                     const char *status_dir,
                                                     bool status_enabled);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_MANAGER_H */
