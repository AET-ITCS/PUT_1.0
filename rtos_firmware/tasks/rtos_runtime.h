/**
 * @file rtos_runtime.h
 * @brief RTOS 共享内存 IPC v2 cooperative runtime 接口。
 * @author Yukikaze
 */
#ifndef RTOS_RUNTIME_H
#define RTOS_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_tasks.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief runtime 单步触发来源。
 */
typedef enum {
    RTOS_RUNTIME_TRIGGER_DOORBELL = 0, /**< Linux doorbell 触发。 */
    RTOS_RUNTIME_TRIGGER_PERIODIC = 1, /**< 周期兜底触发。 */
} rtos_runtime_trigger_t;

/**
 * @brief runtime 初始化配置。
 */
typedef struct {
    put_shm_region_t *region;                         /**< 共享内存 v2 region 指针。 */
    const rtos_shm_platform_ops_t *platform_ops;       /**< 平台 cache/barrier/doorbell 操作。 */
    rtos_router_time_source_t time_source;             /**< runtime 时间源，单位毫秒。 */
    void *time_context;                                /**< 时间源用户上下文。 */
    uint32_t scheduler_budget;                         /**< 单轮 Router Scheduler budget，0 使用默认值。 */
    uint32_t recovery_budget;                          /**< 单轮 Recovery/reclaim budget，0 使用默认值。 */
} rtos_runtime_config_t;

/**
 * @brief runtime 统计快照。
 */
typedef struct {
    bool initialized;                                  /**< runtime 是否已完成初始化。 */
    rtos_tasks_state_t state;                          /**< 当前 task 编排状态。 */
    uint32_t run_count;                                /**< runtime 单步执行次数。 */
    uint32_t doorbell_run_count;                       /**< doorbell 触发执行次数。 */
    uint32_t periodic_run_count;                       /**< 周期触发执行次数。 */
    uint32_t mailbox_ack_fail_count;                   /**< mailbox acknowledge 失败次数。 */
    uint32_t last_processed_count;                     /**< 最近一轮处理动作数。 */
    rtos_tasks_statistics_t task_statistics;           /**< task/monitor 聚合统计。 */
} rtos_runtime_statistics_t;

/**
 * @brief runtime 上下文。
 */
typedef struct {
    rtos_shm_ipc_t ipc;                                /**< RTOS 共享内存 IPC 上下文。 */
    rtos_tasks_context_t tasks;                        /**< RTOS task 编排上下文。 */
    bool initialized;                                  /**< runtime 初始化完成标记。 */
    uint32_t scheduler_budget;                         /**< 当前 Router Scheduler budget。 */
    uint32_t recovery_budget;                          /**< 当前 Recovery/reclaim budget。 */
    uint32_t run_count;                                /**< runtime 单步执行次数。 */
    uint32_t doorbell_run_count;                       /**< doorbell 触发执行次数。 */
    uint32_t periodic_run_count;                       /**< 周期触发执行次数。 */
    uint32_t mailbox_ack_fail_count;                   /**< mailbox acknowledge 失败次数。 */
    uint32_t last_processed_count;                     /**< 最近一轮处理动作数。 */
} rtos_runtime_context_t;

/**
 * @brief 初始化 RTOS runtime。
 *
 * @param runtime runtime 上下文。
 * @param config runtime 初始化配置。
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_runtime_init(rtos_runtime_context_t *runtime,
                                  const rtos_runtime_config_t *config);

/**
 * @brief 单步运行 RTOS runtime。
 *
 * @param runtime runtime 上下文。
 * @param trigger 本轮触发来源。
 * @return 本轮处理动作数量。
 */
uint32_t rtos_runtime_run_once(rtos_runtime_context_t *runtime,
                               rtos_runtime_trigger_t trigger);

/**
 * @brief 获取 runtime 统计快照。
 *
 * @param runtime runtime 上下文。
 * @param out_statistics 输出统计快照。
 */
void rtos_runtime_get_statistics(const rtos_runtime_context_t *runtime,
                                 rtos_runtime_statistics_t *out_statistics);

/**
 * @brief 获取 runtime 当前 task 状态。
 *
 * @param runtime runtime 上下文。
 * @return 当前 task 状态，未初始化时返回 DEGRADED。
 */
rtos_tasks_state_t rtos_runtime_get_state(const rtos_runtime_context_t *runtime);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_RUNTIME_H */
