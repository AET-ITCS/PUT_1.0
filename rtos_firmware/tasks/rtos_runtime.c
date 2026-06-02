/**
 * @file rtos_runtime.c
 * @brief RTOS 共享内存 IPC v2 cooperative runtime 实现。
 * @author Yukikaze
 */
#include "rtos_runtime.h"

#include <string.h>

#include "rtos_mailbox.h"

/** @brief 默认单轮 Router Scheduler budget。 */
#define RTOS_RUNTIME_DEFAULT_SCHEDULER_BUDGET 16u

/** @brief 默认单轮 Recovery/reclaim budget。 */
#define RTOS_RUNTIME_DEFAULT_RECOVERY_BUDGET 16u

/**
 * @brief 将 runtime trigger 映射为 IPC Event Task trigger。
 *
 * @param trigger runtime 触发来源。
 * @return IPC Event Task 触发来源。
 */
static rtos_ipc_event_trigger_t map_ipc_trigger(rtos_runtime_trigger_t trigger)
{
    if (trigger == RTOS_RUNTIME_TRIGGER_DOORBELL) {
        /* doorbell 触发时需要按 pending bitmap 快照过滤入口 ring。 */
        return RTOS_IPC_EVENT_TRIGGER_DOORBELL;
    }

    /* 周期兜底必须扫描所有 RX ring，避免 doorbell 或 pending bit 丢失。 */
    return RTOS_IPC_EVENT_TRIGGER_PERIODIC;
}

/**
 * @brief 判断当前状态是否需要优先补写 pending reclaim。
 *
 * @param state 当前 task 状态。
 * @return true 表示应先补写 pending reclaim。
 */
static bool state_needs_reclaim_retry_first(rtos_tasks_state_t state)
{
    return (state == RTOS_TASKS_STATE_RECLAIM_BLOCKED) ||
           (state == RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL);
}

/**
 * @brief 判断当前状态是否需要优先运行 recovery。
 *
 * @param state 当前 task 状态。
 * @return true 表示应先运行 recovery。
 */
static bool state_needs_recovery_first(rtos_tasks_state_t state)
{
    return state == RTOS_TASKS_STATE_RECOVERY;
}

/**
 * @brief 获取非 0 budget。
 *
 * @param configured_budget 配置值。
 * @param default_budget 默认值。
 * @return 实际 budget。
 */
static uint32_t resolve_budget(uint32_t configured_budget, uint32_t default_budget)
{
    if (configured_budget == 0u) {
        /* 0 表示调用方接受 runtime 默认预算。 */
        return default_budget;
    }

    return configured_budget;
}

/**
 * @brief 同步共享内存 region header。
 *
 * @param runtime runtime 上下文。
 * @return UNIFIED_OK 表示 header 可读取，否则返回公共错误码。
 */
static unified_error_t invalidate_region_header(const rtos_runtime_context_t *runtime)
{
    const put_shm_region_t *region; /**< 当前绑定的共享内存 region。 */

    if ((runtime == 0) || (runtime->ipc.region == 0)) {
        /* region 不可访问时无法安全读取共享 header。 */
        return UNIFIED_ERR_NULL;
    }

    region = runtime->ipc.region;
    if (runtime->ipc.platform_ops.cache_invalidate == 0) {
        /* 缺少 invalidate 操作时不能在非一致 cache 平台上信任 header。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    return runtime->ipc.platform_ops.cache_invalidate(&region->header,
                                                      sizeof(region->header),
                                                      runtime->ipc.platform_ops.user_context);
}

/**
 * @brief 判断共享内存 region header 是否需要在 RX drain 前重新检查。
 *
 * @param runtime runtime 上下文。
 * @param out_needs_preflight 输出是否需要 monitor/recovery preflight。
 * @return UNIFIED_OK 表示判断成功，否则返回公共错误码。
 */
static unified_error_t region_needs_preflight(const rtos_runtime_context_t *runtime,
                                              bool *out_needs_preflight)
{
    const put_shm_region_t *region; /**< 当前绑定的共享内存 region。 */
    unified_error_t result;         /**< cache 同步结果。 */

    if (out_needs_preflight == 0) {
        /* 输出为空时不能返回判断结果。 */
        return UNIFIED_ERR_NULL;
    }

    *out_needs_preflight = true;
    if ((runtime == 0) || (runtime->ipc.region == 0)) {
        /* region 不可访问时需要阻断后续 RX drain。 */
        return UNIFIED_ERR_NULL;
    }

    result = invalidate_region_header(runtime);
    if (result != UNIFIED_OK) {
        /* header 未同步成功时不能继续比较或 drain RX。 */
        return result;
    }

    region = runtime->ipc.region;
    if ((region->header.magic != PUT_SHM_REGION_MAGIC) ||
        (region->header.version != PUT_SHM_IPC_VERSION) ||
        (region->header.header_size != (uint16_t)sizeof(put_shm_region_header_t)) ||
        (region->header.region_size != PUT_SHM_REGION_SIZE)) {
        /* header 基础 ABI 异常时必须让 monitor 先进入降级/recovery。 */
        *out_needs_preflight = true;
        return UNIFIED_OK;
    }

    if (region->header.linux_epoch != runtime->tasks.last_linux_epoch) {
        /* Linux 可能已经重建 region，新 epoch frame 不能用旧 router epoch drain。 */
        *out_needs_preflight = true;
        return UNIFIED_OK;
    }

    *out_needs_preflight = false;
    return UNIFIED_OK;
}

/**
 * @brief 在任何 RX drain 前执行 region/epoch preflight。
 *
 * @param runtime runtime 上下文。
 * @param out_rx_drain_allowed 输出本轮是否允许继续 RX drain。
 * @return 本次 preflight 产生的状态动作数量。
 */
static uint32_t preflight_before_rx_drain(rtos_runtime_context_t *runtime,
                                          bool *out_rx_drain_allowed)
{
    bool needs_preflight;   /**< 是否需要运行 monitor/recovery preflight。 */
    unified_error_t result; /**< header cache 同步和判断结果。 */

    if (out_rx_drain_allowed != 0) {
        *out_rx_drain_allowed = false;
    }

    if ((runtime == 0) || !runtime->initialized) {
        /* runtime 未初始化时没有可检查的 region。 */
        return 0u;
    }

    if (out_rx_drain_allowed == 0) {
        /* 输出为空时调用方无法判断能否 drain RX。 */
        return 0u;
    }

    result = region_needs_preflight(runtime, &needs_preflight);
    if (result != UNIFIED_OK) {
        /* header 未 invalidate 成功时禁止本轮 RX drain，等待下一轮重试。 */
        return 0u;
    }

    *out_rx_drain_allowed = true;
    if (!needs_preflight) {
        /* header 和 epoch 未变化时跳过 monitor，保持 doorbell 快路径轻量。 */
        return 0u;
    }

    /* 复用现有 error monitor 逻辑统一处理 header 异常和 Linux epoch recovery。 */
    return rtos_error_monitor_task_run_once(&runtime->tasks);
}

/**
 * @brief 初始化 RTOS runtime。
 *
 * @param runtime runtime 上下文。
 * @param config runtime 初始化配置。
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_runtime_init(rtos_runtime_context_t *runtime,
                                  const rtos_runtime_config_t *config)
{
    unified_error_t result; /**< 当前初始化阶段结果。 */

    if ((runtime == 0) || (config == 0)) {
        /* runtime 或配置为空时无法建立运行闭环。 */
        return UNIFIED_ERR_NULL;
    }

    if (config->region == 0) {
        /* region 是 v2 IPC attach 的必要输入。 */
        return UNIFIED_ERR_NULL;
    }

    (void)memset(runtime, 0, sizeof(*runtime));
    runtime->scheduler_budget =
        resolve_budget(config->scheduler_budget, RTOS_RUNTIME_DEFAULT_SCHEDULER_BUDGET);
    runtime->recovery_budget =
        resolve_budget(config->recovery_budget, RTOS_RUNTIME_DEFAULT_RECOVERY_BUDGET);

    result = rtos_shm_ipc_attach(&runtime->ipc,
                                 config->region,
                                 config->platform_ops);
    if (result != UNIFIED_OK) {
        /* IPC attach 失败时不能初始化 task context，避免持有无效 ring。 */
        return result;
    }

    rtos_tasks_context_init(&runtime->tasks,
                            &runtime->ipc,
                            config->time_source,
                            config->time_context);
    runtime->initialized = true;
    runtime->last_processed_count = 0u;
    return UNIFIED_OK;
}

/**
 * @brief 单步运行 RTOS runtime。
 *
 * @param runtime runtime 上下文。
 * @param trigger 本轮触发来源。
 * @return 本轮处理动作数量。
 */
uint32_t rtos_runtime_run_once(rtos_runtime_context_t *runtime,
                               rtos_runtime_trigger_t trigger)
{
    uint32_t processed_count;       /**< 本轮累计处理动作数。 */
    rtos_tasks_state_t state;       /**< 当前 task 状态。 */
    unified_error_t ack_result;     /**< mailbox acknowledge 结果。 */
    bool rx_drain_allowed;          /**< 本轮 preflight 后是否允许 drain RX。 */

    if ((runtime == 0) || !runtime->initialized) {
        /* runtime 未初始化时不能访问 IPC 或 task context。 */
        return 0u;
    }

    processed_count = 0u;
    runtime->run_count = runtime->run_count + 1u;
    if (trigger == RTOS_RUNTIME_TRIGGER_DOORBELL) {
        runtime->doorbell_run_count = runtime->doorbell_run_count + 1u;
        ack_result = rtos_mailbox_isr_acknowledge();
        if (ack_result != UNIFIED_OK) {
            /* acknowledge 失败不阻止 drain，ring/pending 才是可信数据状态。 */
            runtime->mailbox_ack_fail_count = runtime->mailbox_ack_fail_count + 1u;
        }
    } else {
        runtime->periodic_run_count = runtime->periodic_run_count + 1u;
    }

    rx_drain_allowed = false;
    processed_count = processed_count +
        preflight_before_rx_drain(runtime, &rx_drain_allowed);

    state = rtos_tasks_get_state(&runtime->tasks);
    if (state_needs_reclaim_retry_first(state)) {
        /* reclaim 阻塞状态优先补写，避免 Frame Pool block 长期被 RTOS 持有。 */
        processed_count = processed_count +
            rtos_tasks_retry_pending_reclaims(&runtime->tasks, runtime->recovery_budget);
    }

    state = rtos_tasks_get_state(&runtime->tasks);
    if (state_needs_recovery_first(state)) {
        /* recovery 状态优先清理本地引用，避免 Frame Pool 长期占用。 */
        processed_count = processed_count +
            rtos_recovery_task_run_once(&runtime->tasks, runtime->recovery_budget);
    }

    state = rtos_tasks_get_state(&runtime->tasks);
    if (rx_drain_allowed && (state != RTOS_TASKS_STATE_RECLAIM_BLOCKED)) {
        processed_count = processed_count +
            rtos_ipc_event_task_run_once(&runtime->tasks, map_ipc_trigger(trigger));
    }

    processed_count = processed_count +
        rtos_router_scheduler_task_run_once(&runtime->tasks, runtime->scheduler_budget);

    if (trigger == RTOS_RUNTIME_TRIGGER_PERIODIC) {
        /* 周期路径负责心跳、异常检测和 recovery 状态推进。 */
        processed_count = processed_count + rtos_heartbeat_task_run_once(&runtime->tasks);
        processed_count = processed_count + rtos_error_monitor_task_run_once(&runtime->tasks);
        if (rtos_tasks_get_state(&runtime->tasks) == RTOS_TASKS_STATE_RECOVERY) {
            processed_count = processed_count +
                rtos_recovery_task_run_once(&runtime->tasks, runtime->recovery_budget);
        }
    }

    (void)rtos_statistics_task_run_once(&runtime->tasks);
    runtime->last_processed_count = processed_count;
    return processed_count;
}

/**
 * @brief 获取 runtime 统计快照。
 *
 * @param runtime runtime 上下文。
 * @param out_statistics 输出统计快照。
 */
void rtos_runtime_get_statistics(const rtos_runtime_context_t *runtime,
                                 rtos_runtime_statistics_t *out_statistics)
{
    if (out_statistics == 0) {
        /* 输出为空时没有可写位置。 */
        return;
    }

    (void)memset(out_statistics, 0, sizeof(*out_statistics));
    if (runtime == 0) {
        /* runtime 为空时返回清零快照。 */
        return;
    }

    out_statistics->initialized = runtime->initialized;
    out_statistics->state = runtime->initialized ?
        rtos_tasks_get_state(&runtime->tasks) : RTOS_TASKS_STATE_DEGRADED;
    out_statistics->run_count = runtime->run_count;
    out_statistics->doorbell_run_count = runtime->doorbell_run_count;
    out_statistics->periodic_run_count = runtime->periodic_run_count;
    out_statistics->mailbox_ack_fail_count = runtime->mailbox_ack_fail_count;
    out_statistics->last_processed_count = runtime->last_processed_count;
    rtos_tasks_get_statistics(&runtime->tasks, &out_statistics->task_statistics);
}

/**
 * @brief 获取 runtime 当前 task 状态。
 *
 * @param runtime runtime 上下文。
 * @return 当前 task 状态，未初始化时返回 DEGRADED。
 */
rtos_tasks_state_t rtos_runtime_get_state(const rtos_runtime_context_t *runtime)
{
    if ((runtime == 0) || !runtime->initialized) {
        /* 未初始化 runtime 不能视为正常态。 */
        return RTOS_TASKS_STATE_DEGRADED;
    }

    return rtos_tasks_get_state(&runtime->tasks);
}
