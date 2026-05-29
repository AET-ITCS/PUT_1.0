/**
 * @file rtos_tasks.c
 * @brief P2 IPC Event、Router Scheduler 和 TX/reclaim sink 编排。
 * @author Yukikaze
 */
#include "rtos_tasks.h"

#include <string.h>

/**
 * @brief 未注入时间源时使用的默认时间。
 *
 * @param user_context 用户上下文，当前未使用。
 * @return 默认时间 0。
 */
static uint32_t default_time_source(void *user_context)
{
    (void)user_context;
    return 0u;
}

/**
 * @brief 读取 P2 当前时间。
 *
 * @param context P2 task 上下文。
 * @return 当前时间，单位毫秒。
 */
static uint32_t tasks_now(const rtos_tasks_context_t *context)
{
    if ((context == 0) || (context->time_source == 0)) {
        return 0u;
    }

    return context->time_source(context->time_context);
}

/**
 * @brief 获取 frame_id 对应的本地引用。
 *
 * @param context P2 task 上下文。
 * @param frame_id Frame Pool block ID。
 * @return 引用指针，无效 frame_id 时返回 NULL。
 */
static rtos_tasks_frame_reference_t *frame_reference_at(rtos_tasks_context_t *context,
                                                       uint32_t frame_id)
{
    if ((context == 0) || (frame_id >= PUT_SHM_FRAME_POOL_BLOCK_COUNT)) {
        return 0;
    }

    return &context->frame_references[frame_id];
}

/**
 * @brief 注册一个已由 RTOS 从 RX ring 消费的可信 frame reference。
 *
 * @param context P2 task 上下文。
 * @param descriptor 可信 RX descriptor。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t register_frame_reference(
    rtos_tasks_context_t *context,
    const put_shm_descriptor_t *descriptor)
{
    rtos_tasks_frame_reference_t *reference; /**< 本地引用槽。 */

    if ((context == 0) || (descriptor == 0)) {
        return UNIFIED_ERR_NULL;
    }

    reference = frame_reference_at(context, descriptor->frame_id);
    if (reference == 0) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (reference->in_use) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    reference->in_use = true;
    reference->descriptor = *descriptor;
    return UNIFIED_OK;
}

/**
 * @brief 清除一个已完成 TX 或 reclaim 的本地 frame reference。
 *
 * @param context P2 task 上下文。
 * @param frame_id Frame Pool block ID。
 */
static void clear_frame_reference(rtos_tasks_context_t *context, uint32_t frame_id)
{
    rtos_tasks_frame_reference_t *reference; /**< 本地引用槽。 */

    reference = frame_reference_at(context, frame_id);
    if (reference != 0) {
        (void)memset(reference, 0, sizeof(*reference));
    }
}

/**
 * @brief 判断指定 frame 是否已有 pending reclaim。
 *
 * @param context P2 task 上下文。
 * @param frame_id Frame Pool block ID。
 * @return true 表示已经冻结，false 表示尚未冻结。
 */
static bool pending_reclaim_exists(const rtos_tasks_context_t *context, uint32_t frame_id)
{
    uint32_t i; /**< pending reclaim 扫描下标。 */

    if (context == 0) {
        return false;
    }

    for (i = 0u; i < context->pending_reclaim_count; ++i) {
        if (context->pending_reclaims[i].frame_id == frame_id) {
            return true;
        }
    }

    return false;
}

/**
 * @brief 将 reclaim 输出冻结到本地 pending 队列。
 *
 * @param context P2 task 上下文。
 * @param output reclaim 输出。
 * @return UNIFIED_OK 表示已冻结，否则返回公共错误码。
 */
static unified_error_t store_pending_reclaim(rtos_tasks_context_t *context,
                                             const rtos_route_output_t *output)
{
    if ((context == 0) || (output == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (pending_reclaim_exists(context, output->frame_id)) {
        context->state = RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL;
        return UNIFIED_OK;
    }

    if (context->pending_reclaim_count >= RTOS_FIRMWARE_PENDING_RECLAIM_CAPACITY) {
        context->state = RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL;
        return UNIFIED_ERR_IPC_RECLAIM_FULL;
    }

    context->pending_reclaims[context->pending_reclaim_count] = *output;
    context->pending_reclaim_count = context->pending_reclaim_count + 1u;
    context->statistics.pending_reclaim_count = context->pending_reclaim_count;
    context->state = RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL;
    return UNIFIED_OK;
}

/**
 * @brief 移除一个 pending reclaim。
 *
 * @param context P2 task 上下文。
 * @param index 待移除下标。
 */
static void remove_pending_reclaim(rtos_tasks_context_t *context, uint32_t index)
{
    uint32_t i; /**< 移动下标。 */

    if ((context == 0) || (index >= context->pending_reclaim_count)) {
        return;
    }

    for (i = index; (i + 1u) < context->pending_reclaim_count; ++i) {
        context->pending_reclaims[i] = context->pending_reclaims[i + 1u];
    }

    context->pending_reclaim_count = context->pending_reclaim_count - 1u;
    context->statistics.pending_reclaim_count = context->pending_reclaim_count;
}

/**
 * @brief 根据原始 RX descriptor 和路由输出构造 TX descriptor。
 *
 * @param source_descriptor 原始 RX descriptor。
 * @param output 路由 TX 输出。
 * @param out_descriptor 输出 TX descriptor。
 */
static void fill_tx_descriptor(const put_shm_descriptor_t *source_descriptor,
                               const rtos_route_output_t *output,
                               put_shm_descriptor_t *out_descriptor)
{
    (void)memset(out_descriptor, 0, sizeof(*out_descriptor));
    out_descriptor->frame_id = output->frame_id;
    out_descriptor->frame_offset = source_descriptor->frame_offset;
    out_descriptor->frame_length = source_descriptor->frame_length;
    out_descriptor->source_interface = (uint8_t)output->source_interface;
    out_descriptor->target_interface = (uint8_t)output->target_interface;
    (void)memcpy(out_descriptor->source_cid, output->source_cid, ANYMSG_CID_LENGTH);
    (void)memcpy(out_descriptor->destination_cid,
                 output->destination_cid,
                 ANYMSG_CID_LENGTH);
    out_descriptor->type = output->type;
    out_descriptor->priority = output->priority;
    out_descriptor->ttl = source_descriptor->ttl;
    out_descriptor->epoch = output->epoch;
    out_descriptor->flags = output->flags;
}

/**
 * @brief P2 真实 TX sink：写入共享内存 TX ring。
 *
 * @param output 路由输出。
 * @param user_context P2 task 上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t rtos_tasks_tx_sink(const rtos_route_output_t *output,
                                          void *user_context)
{
    rtos_tasks_context_t *context;             /**< P2 task 上下文。 */
    rtos_tasks_frame_reference_t *reference;   /**< 本地 frame 引用。 */
    put_shm_descriptor_t tx_descriptor;        /**< 待写入 TX ring 的 descriptor。 */
    unified_error_t result;                    /**< IPC 写入结果。 */

    context = (rtos_tasks_context_t *)user_context;
    if ((context == 0) || (output == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if ((output->kind != RTOS_ROUTE_OUTPUT_TX) || (context->ipc == 0)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    reference = frame_reference_at(context, output->frame_id);
    if ((reference == 0) || !reference->in_use) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    fill_tx_descriptor(&reference->descriptor, output, &tx_descriptor);
    result = rtos_shm_ipc_enqueue_tx_descriptor(context->ipc,
                                                output->target_interface,
                                                &tx_descriptor);
    if (result == UNIFIED_OK) {
        context->statistics.tx_enqueue_count =
            context->statistics.tx_enqueue_count + 1u;
        clear_frame_reference(context, output->frame_id);
    }

    return result;
}

/**
 * @brief P2 真实 reclaim sink：写入共享内存 reclaim ring。
 *
 * @param output 路由输出。
 * @param user_context P2 task 上下文。
 * @return UNIFIED_OK 表示 reclaim 已写入或已冻结待补写。
 */
static unified_error_t rtos_tasks_reclaim_sink(const rtos_route_output_t *output,
                                               void *user_context)
{
    rtos_tasks_context_t *context;           /**< P2 task 上下文。 */
    rtos_tasks_frame_reference_t *reference; /**< 本地 frame 引用。 */
    unified_error_t result;                  /**< reclaim 写入结果。 */

    context = (rtos_tasks_context_t *)user_context;
    if ((context == 0) || (output == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if ((output->kind != RTOS_ROUTE_OUTPUT_RECLAIM) || (context->ipc == 0)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    reference = frame_reference_at(context, output->frame_id);
    if ((reference == 0) || !reference->in_use) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    result = rtos_shm_ipc_reclaim_frame(context->ipc,
                                        output->frame_id,
                                        output->reclaim_reason,
                                        output->source_interface,
                                        output->target_interface,
                                        output->epoch,
                                        output->flags);
    if (result == UNIFIED_OK) {
        context->statistics.reclaim_enqueue_count =
            context->statistics.reclaim_enqueue_count + 1u;
        clear_frame_reference(context, output->frame_id);
        return UNIFIED_OK;
    }

    if (result == UNIFIED_ERR_IPC_QUEUE_FULL) {
        context->statistics.reclaim_full_count =
            context->statistics.reclaim_full_count + 1u;
        return store_pending_reclaim(context, output);
    }

    return result;
}

/**
 * @brief drain 单个 RX 接口。
 *
 * @param context P2 task 上下文。
 * @param interface_id 物理接口 ID。
 * @param budget 本接口最多消费 descriptor 数。
 * @return 本次实际消费或隔离的 descriptor 数。
 */
static uint32_t drain_rx_interface(rtos_tasks_context_t *context,
                                   put_shm_interface_t interface_id,
                                   uint32_t budget)
{
    uint32_t processed;                 /**< 已处理 descriptor 数。 */
    put_shm_descriptor_t descriptor;    /**< 已出队 descriptor。 */
    rtos_route_input_t input;           /**< 适配后的路由输入。 */
    unified_error_t result;             /**< 当前操作结果。 */

    processed = 0u;
    while ((processed < budget) &&
           (context->state == RTOS_TASKS_STATE_NORMAL)) {
        result = rtos_shm_ipc_dequeue_rx_descriptor(context->ipc,
                                                    interface_id,
                                                    &descriptor);
        if (result == UNIFIED_ERR_IPC_QUEUE_EMPTY) {
            break;
        }

        if (result != UNIFIED_OK) {
            context->statistics.rx_dequeue_error_count =
                context->statistics.rx_dequeue_error_count + 1u;
            context->statistics.invalid_descriptor_no_reclaim_count =
                context->statistics.invalid_descriptor_no_reclaim_count + 1u;
            processed = processed + 1u;
            continue;
        }

        context->statistics.rx_descriptor_count =
            context->statistics.rx_descriptor_count + 1u;
        result = register_frame_reference(context, &descriptor);
        if (result != UNIFIED_OK) {
            context->statistics.invalid_descriptor_no_reclaim_count =
                context->statistics.invalid_descriptor_no_reclaim_count + 1u;
            processed = processed + 1u;
            continue;
        }

        result = rtos_router_adapter_descriptor_to_input(context->ipc,
                                                         &descriptor,
                                                         tasks_now(context),
                                                         &input);
        if (result != UNIFIED_OK) {
            context->statistics.adapter_error_count =
                context->statistics.adapter_error_count + 1u;
            clear_frame_reference(context, descriptor.frame_id);
            processed = processed + 1u;
            continue;
        }

        result = rtos_router_submit(&context->router, &input);
        if (result != UNIFIED_OK) {
            processed = processed + 1u;
            break;
        }

        context->statistics.route_submit_count =
            context->statistics.route_submit_count + 1u;
        processed = processed + 1u;
    }

    context->statistics.rx_drain_count = context->statistics.rx_drain_count + processed;
    return processed;
}

/**
 * @brief 初始化 P2 task 编排上下文。
 *
 * @param context P2 task 上下文。
 * @param ipc 已 attach 的 RTOS IPC 上下文。
 * @param time_source 时间源，可为 NULL。
 * @param time_context 时间源用户上下文。
 */
void rtos_tasks_context_init(rtos_tasks_context_t *context,
                             rtos_shm_ipc_t *ipc,
                             rtos_router_time_source_t time_source,
                             void *time_context)
{
    rtos_router_sinks_t sinks; /**< P2 真实 sink 集合。 */
    uint32_t linux_epoch;      /**< 当前 Linux epoch。 */

    if (context == 0) {
        return;
    }

    (void)memset(context, 0, sizeof(*context));
    context->ipc = ipc;
    context->state = RTOS_TASKS_STATE_NORMAL;
    context->time_source = (time_source != 0) ? time_source : default_time_source;
    context->time_context = time_context;

    (void)memset(&sinks, 0, sizeof(sinks));
    sinks.tx_sink = rtos_tasks_tx_sink;
    sinks.reclaim_sink = rtos_tasks_reclaim_sink;
    sinks.user_context = context;
    rtos_router_init(&context->router, &sinks, context->time_source, context->time_context);

    linux_epoch = 1u;
    if ((ipc != 0) && ipc->initialized && (ipc->region != 0)) {
        linux_epoch = ipc->region->header.linux_epoch;
    }
    rtos_router_set_linux_epoch(&context->router, linux_epoch);
}

/**
 * @brief 单步执行 IPC Event Task。
 *
 * @param context P2 task 上下文。
 * @param trigger 唤醒来源。
 * @return 本轮处理的 descriptor 数，或 reclaim full 补写数。
 */
uint32_t rtos_ipc_event_task_run_once(rtos_tasks_context_t *context,
                                      rtos_ipc_event_trigger_t trigger)
{
    uint32_t pending_bits;       /**< RX pending bitmap 快照。 */
    uint32_t processed;          /**< 本轮处理总数。 */
    uint32_t interface_index;    /**< 接口扫描下标。 */
    uint32_t remaining_budget;   /**< 剩余总 budget。 */
    uint32_t interface_budget;   /**< 本接口 budget。 */
    unified_error_t result;      /**< pending snapshot 读取结果。 */

    if ((context == 0) || (context->ipc == 0)) {
        return 0u;
    }

    if (context->state != RTOS_TASKS_STATE_NORMAL) {
        context->statistics.blocked_rx_drain_count =
            context->statistics.blocked_rx_drain_count + 1u;
        return rtos_tasks_retry_pending_reclaims(
            context,
            RTOS_FIRMWARE_PENDING_RECLAIM_CAPACITY);
    }

    pending_bits = 0u;
    if (trigger == RTOS_IPC_EVENT_TRIGGER_DOORBELL) {
        result = rtos_shm_ipc_get_rx_pending_snapshot(context->ipc, &pending_bits);
        if (result != UNIFIED_OK) {
            return 0u;
        }
        context->statistics.rx_pending_snapshot_count =
            context->statistics.rx_pending_snapshot_count + 1u;
    }

    processed = 0u;
    remaining_budget = RTOS_FIRMWARE_RX_DRAIN_BUDGET_TOTAL;
    for (interface_index = 0u;
         (interface_index < PUT_SHM_INTERFACE_COUNT) && (remaining_budget > 0u);
         ++interface_index) {
        if ((trigger == RTOS_IPC_EVENT_TRIGGER_DOORBELL) &&
            ((pending_bits & (uint32_t)(1u << interface_index)) == 0u)) {
            continue;
        }

        interface_budget = RTOS_FIRMWARE_RX_DRAIN_BUDGET_PER_INTERFACE;
        if (interface_budget > remaining_budget) {
            interface_budget = remaining_budget;
        }

        processed = processed + drain_rx_interface(context,
                                                   (put_shm_interface_t)interface_index,
                                                   interface_budget);
        remaining_budget = RTOS_FIRMWARE_RX_DRAIN_BUDGET_TOTAL - processed;
        if (context->state != RTOS_TASKS_STATE_NORMAL) {
            break;
        }
    }

    return processed;
}

/**
 * @brief 单步执行 Router Scheduler / TX Writer。
 *
 * @param context P2 task 上下文。
 * @param budget 本轮最多调度帧数。
 * @return 本轮成功调度或补写 reclaim 的数量。
 */
uint32_t rtos_router_scheduler_task_run_once(rtos_tasks_context_t *context,
                                             uint32_t budget)
{
    if (context == 0) {
        return 0u;
    }

    if (context->pending_reclaim_count > 0u) {
        return rtos_tasks_retry_pending_reclaims(context, budget);
    }

    if (context->state != RTOS_TASKS_STATE_NORMAL) {
        return 0u;
    }

    return rtos_router_drain(&context->router, budget);
}

/**
 * @brief 补写 reclaim ring 满时冻结的 reclaim descriptor。
 *
 * @param context P2 task 上下文。
 * @param budget 本轮最多补写数量。
 * @return 本轮成功补写数量。
 */
uint32_t rtos_tasks_retry_pending_reclaims(rtos_tasks_context_t *context,
                                           uint32_t budget)
{
    uint32_t written;                   /**< 已补写数量。 */
    rtos_route_output_t output;         /**< 当前待补写 reclaim。 */
    unified_error_t result;             /**< reclaim 写入结果。 */

    if ((context == 0) || (context->ipc == 0)) {
        return 0u;
    }

    written = 0u;
    while ((written < budget) && (context->pending_reclaim_count > 0u)) {
        output = context->pending_reclaims[0];
        result = rtos_shm_ipc_reclaim_frame(context->ipc,
                                            output.frame_id,
                                            output.reclaim_reason,
                                            output.source_interface,
                                            output.target_interface,
                                            output.epoch,
                                            output.flags);
        if (result == UNIFIED_ERR_IPC_QUEUE_FULL) {
            context->state = RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL;
            break;
        }

        if (result != UNIFIED_OK) {
            break;
        }

        clear_frame_reference(context, output.frame_id);
        remove_pending_reclaim(context, 0u);
        context->statistics.reclaim_enqueue_count =
            context->statistics.reclaim_enqueue_count + 1u;
        context->statistics.pending_reclaim_retry_count =
            context->statistics.pending_reclaim_retry_count + 1u;
        written = written + 1u;
    }

    if (context->pending_reclaim_count == 0u) {
        context->state = RTOS_TASKS_STATE_NORMAL;
    }

    return written;
}

/**
 * @brief 读取 P2 task 状态。
 *
 * @param context P2 task 上下文。
 * @return 当前状态。
 */
rtos_tasks_state_t rtos_tasks_get_state(const rtos_tasks_context_t *context)
{
    if (context == 0) {
        return RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL;
    }

    return context->state;
}

/**
 * @brief 读取 P2 task 统计快照。
 *
 * @param context P2 task 上下文。
 * @param out_statistics 输出统计。
 */
void rtos_tasks_get_statistics(const rtos_tasks_context_t *context,
                               rtos_tasks_statistics_t *out_statistics)
{
    if (out_statistics == 0) {
        return;
    }

    (void)memset(out_statistics, 0, sizeof(*out_statistics));
    if (context != 0) {
        *out_statistics = context->statistics;
    }
}
