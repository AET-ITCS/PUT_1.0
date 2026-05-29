/**
 * @file rtos_tasks.h
 * @brief P2 task 编排和 host 单步调度接口。
 * @author Yukikaze
 */
#ifndef RTOS_TASKS_H
#define RTOS_TASKS_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_firmware_config.h"
#include "rtos_router.h"
#include "rtos_shm_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief P2 小核运行状态。
 */
typedef enum {
    RTOS_TASKS_STATE_NORMAL = 0,
    RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL = 1,
} rtos_tasks_state_t;

/**
 * @brief IPC Event Task 唤醒来源。
 */
typedef enum {
    RTOS_IPC_EVENT_TRIGGER_DOORBELL = 0,
    RTOS_IPC_EVENT_TRIGGER_PERIODIC = 1,
} rtos_ipc_event_trigger_t;

/**
 * @brief P2 已从 Linux RX ring 接收、尚未完成 TX/reclaim 的帧引用。
 */
typedef struct {
    bool in_use;                       /**< 该 frame_id 当前由 RTOS 本地持有。 */
    put_shm_descriptor_t descriptor;   /**< 原始可信 RX descriptor 元数据。 */
} rtos_tasks_frame_reference_t;

/**
 * @brief P2 task 编排统计。
 */
typedef struct {
    uint32_t rx_pending_snapshot_count;
    uint32_t rx_drain_count;
    uint32_t rx_descriptor_count;
    uint32_t rx_dequeue_error_count;
    uint32_t invalid_descriptor_no_reclaim_count;
    uint32_t adapter_error_count;
    uint32_t route_submit_count;
    uint32_t tx_enqueue_count;
    uint32_t reclaim_enqueue_count;
    uint32_t reclaim_full_count;
    uint32_t pending_reclaim_retry_count;
    uint32_t blocked_rx_drain_count;
    uint32_t pending_reclaim_count;
} rtos_tasks_statistics_t;

/**
 * @brief P2 task 编排上下文。
 */
typedef struct {
    rtos_shm_ipc_t *ipc;
    rtos_router_context_t router;
    rtos_tasks_state_t state;
    rtos_tasks_statistics_t statistics;
    rtos_tasks_frame_reference_t frame_references[PUT_SHM_FRAME_POOL_BLOCK_COUNT];
    rtos_route_output_t pending_reclaims[RTOS_FIRMWARE_PENDING_RECLAIM_CAPACITY];
    uint32_t pending_reclaim_count;
    rtos_router_time_source_t time_source;
    void *time_context;
} rtos_tasks_context_t;

void rtos_tasks_context_init(rtos_tasks_context_t *context,
                             rtos_shm_ipc_t *ipc,
                             rtos_router_time_source_t time_source,
                             void *time_context);

uint32_t rtos_ipc_event_task_run_once(rtos_tasks_context_t *context,
                                      rtos_ipc_event_trigger_t trigger);

uint32_t rtos_router_scheduler_task_run_once(rtos_tasks_context_t *context,
                                             uint32_t budget);

uint32_t rtos_tasks_retry_pending_reclaims(rtos_tasks_context_t *context,
                                           uint32_t budget);

rtos_tasks_state_t rtos_tasks_get_state(const rtos_tasks_context_t *context);

void rtos_tasks_get_statistics(const rtos_tasks_context_t *context,
                               rtos_tasks_statistics_t *out_statistics);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_TASKS_H */
