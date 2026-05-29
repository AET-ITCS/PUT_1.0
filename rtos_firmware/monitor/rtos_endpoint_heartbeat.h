/**
 * @file rtos_endpoint_heartbeat.h
 * @brief P1 路由主机端测试使用的端心跳表。
 * @author Yukikaze
 */
#ifndef RTOS_ENDPOINT_HEARTBEAT_H
#define RTOS_ENDPOINT_HEARTBEAT_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_firmware_config.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 端心跳状态。
 */
typedef enum {
    RTOS_ENDPOINT_HEARTBEAT_STATE_EMPTY = 0,
    RTOS_ENDPOINT_HEARTBEAT_STATE_ONLINE = 1,
} rtos_endpoint_heartbeat_state_t;

/**
 * @brief 单个端心跳记录。
 */
typedef struct {
    bool in_use;                                  /**< 记录已分配。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH];       /**< 端设备源 CID。 */
    put_shm_interface_t last_rx_interface;       /**< 最近一次心跳来源接口。 */
    uint32_t last_rtos_time_ms;                  /**< 心跳到达时的 RTOS 时间。 */
    uint32_t last_frame_local_time;              /**< anyMSG local_time 快照。 */
    uint32_t rx_count;                           /**< 已消费心跳数量。 */
    rtos_endpoint_heartbeat_state_t state;       /**< P1 心跳状态。 */
} rtos_endpoint_heartbeat_entry_t;

/**
 * @brief 只读心跳快照。
 */
typedef struct {
    bool gateway_configured;                     /**< gateway CID 已配置。 */
    uint8_t gateway_cid[ANYMSG_CID_LENGTH];      /**< 当前配置的 gateway CID。 */
    uint32_t entry_count;                        /**< 已分配记录数量。 */
    rtos_endpoint_heartbeat_entry_t entries[RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY];
} rtos_endpoint_heartbeat_snapshot_t;

/**
 * @brief 端心跳表上下文。
 */
typedef struct {
    bool gateway_configured;
    uint8_t gateway_cid[ANYMSG_CID_LENGTH];
    rtos_endpoint_heartbeat_entry_t entries[RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY];
} rtos_endpoint_heartbeat_table_t;

void rtos_endpoint_heartbeat_init(rtos_endpoint_heartbeat_table_t *table);

unified_error_t rtos_endpoint_heartbeat_set_gateway(
    rtos_endpoint_heartbeat_table_t *table,
    const uint8_t gateway_cid[ANYMSG_CID_LENGTH]);

void rtos_endpoint_heartbeat_clear_gateway(rtos_endpoint_heartbeat_table_t *table);

unified_error_t rtos_endpoint_heartbeat_consume(
    rtos_endpoint_heartbeat_table_t *table,
    const uint8_t source_cid[ANYMSG_CID_LENGTH],
    const uint8_t destination_cid[ANYMSG_CID_LENGTH],
    put_shm_interface_t source_interface,
    uint32_t now_ms,
    uint32_t frame_local_time);

void rtos_endpoint_heartbeat_get_snapshot(
    const rtos_endpoint_heartbeat_table_t *table,
    rtos_endpoint_heartbeat_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_ENDPOINT_HEARTBEAT_H */
