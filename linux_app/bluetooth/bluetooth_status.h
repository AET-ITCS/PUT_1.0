/* 蓝牙 Web 状态快照头文件：维护 RFCOMM 状态、收发统计并以线程安全方式写入 JSON。 */
#ifndef BLUETOOTH_STATUS_H
#define BLUETOOTH_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLUETOOTH_STATUS_DEFAULT_DIR "/run/put/status"
#define BLUETOOTH_STATUS_PATH_MAX 256u
#define BLUETOOTH_STATUS_TEXT_MAX 128u

typedef struct {
    bool enabled;
    bool listening;
    bool connected;
    bool stopped;
    uint8_t rfcomm_channel;
    char status_dir[BLUETOOTH_STATUS_PATH_MAX];

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
    char last_error_stage[BLUETOOTH_STATUS_TEXT_MAX];
    char last_error_message[BLUETOOTH_STATUS_TEXT_MAX];
    char last_ipc_error_message[BLUETOOTH_STATUS_TEXT_MAX];
    char connected_client_addr[BLUETOOTH_STATUS_TEXT_MAX]; /**< 当前连接的客户端 MAC 地址 */
} bluetooth_status_t;

/**
 * @brief 初始化蓝牙状态结构体
 */
void bluetooth_status_init(bluetooth_status_t *status,
                           const char *status_dir,
                           uint8_t rfcomm_channel,
                           bool enabled);

/**
 * @brief 标记蓝牙正在监听
 */
void bluetooth_status_mark_listening(bluetooth_status_t *status);

/**
 * @brief 标记蓝牙已成功建立连接
 *
 * @param client_addr 客户端 MAC 地址（如 "AA:BB:CC:DD:EE:FF"）
 */
void bluetooth_status_mark_connected(bluetooth_status_t *status, const char *client_addr);

/**
 * @brief 标记蓝牙客户端已断开连接
 */
void bluetooth_status_mark_disconnected(bluetooth_status_t *status);

/**
 * @brief 标记蓝牙监听服务已停止
 *
 * @param reason 停止原因
 */
void bluetooth_status_mark_stopped(bluetooth_status_t *status, const char *reason);

/**
 * @brief 记录蓝牙成功接收到原始包
 *
 * @param bytes 接收到的字节数
 */
void bluetooth_status_record_rx(bluetooth_status_t *status, size_t bytes);

/**
 * @brief 记录蓝牙成功解析并把统一帧发送给小核
 */
void bluetooth_status_record_tx_ok(bluetooth_status_t *status);

/**
 * @brief 记录蓝牙接收/解析/发送管道中的异常
 *
 * @param stage 发生错误的阶段
 * @param err 统一错误码
 */
void bluetooth_status_record_error(bluetooth_status_t *status,
                                   const char *stage,
                                   unified_error_t err);

/**
 * @brief 集中式以太网与蓝牙状态联合输出接口（由互斥锁保护安全）
 *
 * 该接口将同时获取以太网和蓝牙的最新统计，拼接后原子写出到 modules.json。
 * 如果其中某个模块未启动（传入 NULL），则会在 modules.json 中该模块的状态以 "unknown" 显示。
 */
int gateway_status_write_modules_json(const char *status_dir,
                                     bool status_enabled,
                                     const void *eth_status_raw,
                                     const bluetooth_status_t *bt_status);

/**
 * @brief 触发蓝牙快照状态与事件写出。
 *
 * 内部会调用全局状态同步方法，并写出 modules.json 及追加 events.jsonl。
 * @return 0 成功，-1 失败
 */
int bluetooth_status_write_all(bluetooth_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* BLUETOOTH_STATUS_H */
