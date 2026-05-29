/**
 * @file rtos_router.h
 * @brief rtos_firmware P1 路由核心接口。
 * @author Yukikaze
 */
#ifndef RTOS_ROUTER_H
#define RTOS_ROUTER_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_endpoint_heartbeat.h"
#include "rtos_priority_queue.h"
#include "rtos_shm_ipc.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief P1 路由输入可信状态。
 */
typedef enum {
    RTOS_ROUTE_TRUST_AUTH_OK = 0,
    RTOS_ROUTE_TRUST_INTERNAL_TRUSTED = 1,
    RTOS_ROUTE_TRUST_AUTH_FAILED = 2,
    RTOS_ROUTE_TRUST_INTEGRITY_FAILED = 3,
    RTOS_ROUTE_TRUST_REPLAY_DROPPED = 4,
} rtos_route_trust_t;

/**
 * @brief P1 路由输出类型。
 */
typedef enum {
    RTOS_ROUTE_OUTPUT_NONE = 0,
    RTOS_ROUTE_OUTPUT_TX = 1,
    RTOS_ROUTE_OUTPUT_RECLAIM = 2,
} rtos_route_output_kind_t;

/**
 * @brief 已与 put_shm_descriptor_t 解耦的可信路由输入。
 */
typedef struct {
    uint32_t frame_id;                      /**< 可信 Frame Pool block ID。 */
    put_shm_interface_t source_interface;   /**< 来源 RX 接口。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH];  /**< anyMSG 源 CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< anyMSG 目的 CID。 */
    uint8_t type;                           /**< anyMSG type。 */
    uint8_t priority;                       /**< 调度优先级。 */
    uint8_t ttl;                            /**< TTL 毫秒值，0 表示禁用过期检查。 */
    rtos_route_trust_t trust;               /**< P1 可信状态。 */
    uint32_t epoch;                         /**< 输入携带的 Linux epoch。 */
    uint32_t flags;                         /**< 路由输入标志。 */
    uint32_t receive_time_ms;               /**< P1 接收该输入的时间。 */
    uint32_t frame_local_time;              /**< anyMSG local_time 快照。 */
    bool anymsg_header_valid;               /**< 为 false 时模拟 anyMSG 静态头校验失败。 */
} rtos_route_input_t;

/**
 * @brief 模拟 TX 和 reclaim sink 使用的路由输出。
 */
typedef struct {
    rtos_route_output_kind_t kind;          /**< TX 或 reclaim 输出。 */
    uint32_t frame_id;                      /**< 帧引用。 */
    put_shm_interface_t source_interface;   /**< 原始来源接口。 */
    put_shm_interface_t target_interface;   /**< 目标接口或最近一次路由目标。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH];  /**< 源 CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< 目的 CID。 */
    uint8_t type;                           /**< anyMSG type。 */
    uint8_t priority;                       /**< 优先级。 */
    uint8_t retry_count;                    /**< TX 重试次数。 */
    uint32_t epoch;                         /**< Linux epoch。 */
    uint32_t flags;                         /**< 输出标志。 */
    uint32_t latency_ms;                    /**< 从 receive_time_ms 到输出的延迟。 */
    put_shm_reclaim_reason_t reclaim_reason; /**< kind 为 reclaim 时的回收原因。 */
} rtos_route_output_t;

/**
 * @brief P1 固定 CID 路由使用的路由表快照。
 */
typedef struct {
    uint32_t route_version;                 /**< 路由表版本。 */
    uint32_t active_route_epoch;            /**< 当前生效的路由 epoch。 */
    bool valid;                             /**< 路由表有效标记。 */
    put_shm_interface_t cid_segment_targets[ANYMSG_CID_SEGMENT_RESERVED_HIGH + 1u];
    uint16_t crc16;                         /**< 预留给后续控制区 CRC。 */
} rtos_route_table_snapshot_t;

/**
 * @brief P1 路由统计快照。
 */
typedef struct {
    uint32_t submitted_count;
    uint32_t enqueued_count;
    uint32_t routed_count;
    uint32_t reclaimed_count;
    uint32_t heartbeat_consumed_count;
    uint32_t queue_evicted_count;
    uint32_t tx_retry_count;
    uint32_t drop_reason_count[PUT_SHM_RECLAIM_REASON_QUEUE_FULL + 1u];
    uint32_t routed_by_interface[PUT_SHM_INTERFACE_COUNT];
    uint32_t routed_by_priority[RTOS_FIRMWARE_PRIORITY_COUNT];
    uint32_t auth_failed_count;
    uint32_t integrity_failed_count;
    uint32_t replay_dropped_count;
    uint32_t invalid_priority_count;
    uint32_t invalid_type_count;
    uint32_t invalid_header_count;
    uint32_t route_table_crc_error_count;
} rtos_router_statistics_t;

/**
 * @brief 路由器时间源回调。
 */
typedef uint32_t (*rtos_router_time_source_t)(void *user_context);

/**
 * @brief 可替换的 TX sink 回调。
 */
typedef unified_error_t (*rtos_router_tx_sink_t)(const rtos_route_output_t *output,
                                                 void *user_context);

/**
 * @brief 可替换的 reclaim sink 回调。
 */
typedef unified_error_t (*rtos_router_reclaim_sink_t)(const rtos_route_output_t *output,
                                                      void *user_context);

/**
 * @brief P1 路由 sink 回调集合。
 */
typedef struct {
    rtos_router_tx_sink_t tx_sink;
    rtos_router_reclaim_sink_t reclaim_sink;
    void *user_context;
} rtos_router_sinks_t;

/**
 * @brief P1 路由上下文。
 */
typedef struct {
    rtos_priority_queue_t queue;
    rtos_endpoint_heartbeat_table_t endpoint_heartbeat;
    rtos_route_table_snapshot_t route_table;
    rtos_router_statistics_t statistics;
    rtos_router_sinks_t sinks;
    rtos_router_time_source_t time_source;
    void *time_context;
    uint32_t current_linux_epoch;
} rtos_router_context_t;

void rtos_router_init(rtos_router_context_t *router,
                      const rtos_router_sinks_t *sinks,
                      rtos_router_time_source_t time_source,
                      void *time_context);

void rtos_router_set_linux_epoch(rtos_router_context_t *router, uint32_t linux_epoch);

unified_error_t rtos_router_submit(rtos_router_context_t *router,
                                   const rtos_route_input_t *input);

unified_error_t rtos_router_schedule_once(rtos_router_context_t *router,
                                          rtos_route_output_t *out_output);

uint32_t rtos_router_drain(rtos_router_context_t *router, uint32_t budget);

uint32_t rtos_router_reclaim_queued(rtos_router_context_t *router,
                                    put_shm_reclaim_reason_t reason,
                                    uint32_t budget);

uint32_t rtos_router_get_queued_count(const rtos_router_context_t *router);

void rtos_router_get_route_table(const rtos_router_context_t *router,
                                 rtos_route_table_snapshot_t *out_table);

unified_error_t rtos_router_set_route_table(rtos_router_context_t *router,
                                            const rtos_route_table_snapshot_t *table);

void rtos_router_route_table_default(rtos_route_table_snapshot_t *out_table);

uint16_t rtos_router_route_table_calculate_crc(
    const rtos_route_table_snapshot_t *table);

bool rtos_router_route_table_crc_is_valid(const rtos_route_table_snapshot_t *table);

unified_error_t rtos_router_route_table_lookup(
    const rtos_route_table_snapshot_t *table,
    uint8_t destination_cid_first_byte,
    put_shm_interface_t *out_interface);

void rtos_router_get_statistics(const rtos_router_context_t *router,
                                rtos_router_statistics_t *out_statistics);

bool rtos_router_type_is_valid(uint8_t type);

bool rtos_router_trust_is_routable(rtos_route_trust_t trust);

bool rtos_router_ttl_is_expired(uint32_t now_ms,
                                uint32_t receive_time_ms,
                                uint8_t ttl_ms);

unified_error_t rtos_router_adapter_p1_boundary_check(void);

unified_error_t rtos_router_adapter_descriptor_to_input(
    const rtos_shm_ipc_t *ipc,
    const put_shm_descriptor_t *descriptor,
    uint32_t now_ms,
    rtos_route_input_t *out_input);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_ROUTER_H */
