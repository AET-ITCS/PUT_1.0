/* 以太网 Web 状态快照：维护 UDP 接收/转发统计并写入状态 JSON 文件。 */
#ifndef ETHERNET_STATUS_H
#define ETHERNET_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ETHERNET_STATUS_DEFAULT_DIR "/run/put/status"
#define ETHERNET_STATUS_PATH_MAX 256u
#define ETHERNET_STATUS_TEXT_MAX 128u

typedef struct {
    bool enabled;
    bool listening;
    bool stopped;
    uint16_t listen_port;
    char status_dir[ETHERNET_STATUS_PATH_MAX];

    uint64_t rx_count;
    uint64_t tx_count;
    uint64_t rx_bytes;
    uint64_t error_count;
    uint64_t parse_error_count;
    uint64_t pack_error_count;
    uint64_t ipc_error_count;
    uint64_t consecutive_error_count;

    uint64_t started_at_ms;
    uint64_t updated_at_ms;
    uint64_t last_seen_ms;
    uint64_t last_tx_ms;
    uint64_t last_error_ms;
    uint64_t last_ipc_error_ms;

    unified_error_t last_error_code;
    unified_error_t last_ipc_error_code;
    char last_error_stage[ETHERNET_STATUS_TEXT_MAX];
    char last_error_message[ETHERNET_STATUS_TEXT_MAX];
    char last_ipc_error_message[ETHERNET_STATUS_TEXT_MAX];
} ethernet_status_t;

void ethernet_status_init(ethernet_status_t *status,
                          const char *status_dir,
                          uint16_t listen_port,
                          bool enabled);
void ethernet_status_mark_listening(ethernet_status_t *status);
void ethernet_status_mark_stopped(ethernet_status_t *status, const char *reason);
void ethernet_status_record_rx(ethernet_status_t *status, size_t bytes);
void ethernet_status_record_tx_ok(ethernet_status_t *status);
void ethernet_status_record_error(ethernet_status_t *status,
                                  const char *stage,
                                  unified_error_t err);

/**
 * @brief 写出 Web 后端可读取的业务快照。
 *
 * 写入文件：
 * - modules.json：以太网 UDP 模块真实统计，其它协议第一版填 unknown；
 * - ipc_status.json：大核到小核发送统计；
 * - can_status.json：小核 CAN 回传暂未接入时填 unknown；
 * - events.jsonl：仅在 record_error() 时追加事件。
 *
 * 返回 0 表示全部快照写入成功；返回 -1 表示至少一个文件写入失败。
 * 写入失败不应中断协议主链路，调用方通常只打印一次告警。
 */
int ethernet_status_write_all(ethernet_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_STATUS_H */
