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
        context->state = (context->state == RTOS_TASKS_STATE_RECOVERY) ?
            RTOS_TASKS_STATE_RECLAIM_BLOCKED : RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL;
        return UNIFIED_OK;
    }

    if (context->pending_reclaim_count >= RTOS_FIRMWARE_PENDING_RECLAIM_CAPACITY) {
        context->state = (context->state == RTOS_TASKS_STATE_RECOVERY) ?
            RTOS_TASKS_STATE_RECLAIM_BLOCKED : RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL;
        context->statistics.reclaim_blocked_count =
            context->statistics.reclaim_blocked_count + 1u;
        rtos_monitor_record_error(&context->monitor,
                                  RTOS_MONITOR_ERROR_RECLAIM_BLOCKED,
                                  tasks_now(context));
        return UNIFIED_ERR_IPC_RECLAIM_FULL;
    }

    context->pending_reclaims[context->pending_reclaim_count] = *output;
    context->pending_reclaim_count = context->pending_reclaim_count + 1u;
    context->statistics.pending_reclaim_count = context->pending_reclaim_count;
    context->state = (context->state == RTOS_TASKS_STATE_RECOVERY) ?
        RTOS_TASKS_STATE_RECLAIM_BLOCKED : RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL;
    context->statistics.reclaim_blocked_count =
        context->statistics.reclaim_blocked_count + 1u;
    rtos_monitor_record_error(&context->monitor,
                              RTOS_MONITOR_ERROR_RECLAIM_BLOCKED,
                              tasks_now(context));
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
        rtos_monitor_record_latency(&context->monitor,
                                    output->priority,
                                    output->latency_ms);
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
    rtos_route_output_t reclaim_output;      /**< 带原始 RX 元数据的 reclaim 输出。 */
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

    reclaim_output = *output;
    reclaim_output.source_interface =
        (put_shm_interface_t)reference->descriptor.source_interface;
    reclaim_output.target_interface =
        (put_shm_interface_t)reference->descriptor.target_interface;
    reclaim_output.epoch = reference->descriptor.epoch;
    reclaim_output.flags = reference->descriptor.flags;

    result = rtos_shm_ipc_reclaim_frame(context->ipc,
                                        reclaim_output.frame_id,
                                        reclaim_output.reclaim_reason,
                                        reclaim_output.source_interface,
                                        reclaim_output.target_interface,
                                        reclaim_output.epoch,
                                        reclaim_output.flags);
    if (result == UNIFIED_OK) {
        context->statistics.reclaim_enqueue_count =
            context->statistics.reclaim_enqueue_count + 1u;
        clear_frame_reference(context, output->frame_id);
        return UNIFIED_OK;
    }

    if (result == UNIFIED_ERR_IPC_QUEUE_FULL) {
        context->statistics.reclaim_full_count =
            context->statistics.reclaim_full_count + 1u;
        return store_pending_reclaim(context, &reclaim_output);
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
    rtos_monitor_context_init(&context->monitor);

    (void)memset(&sinks, 0, sizeof(sinks));
    sinks.tx_sink = rtos_tasks_tx_sink;
    sinks.reclaim_sink = rtos_tasks_reclaim_sink;
    sinks.user_context = context;
    rtos_router_init(&context->router, &sinks, context->time_source, context->time_context);

    linux_epoch = 1u;
    if ((ipc != 0) && ipc->initialized && (ipc->region != 0)) {
        linux_epoch = ipc->region->header.linux_epoch;
    }
    context->last_linux_epoch = linux_epoch;
    context->monitor.recovery.recovery_epoch = linux_epoch;
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

    if (trigger == RTOS_IPC_EVENT_TRIGGER_DOORBELL) {
        context->monitor.statistics.doorbell_rx_count =
            context->monitor.statistics.doorbell_rx_count + 1u;
    }

    if (context->state != RTOS_TASKS_STATE_NORMAL) {
        context->statistics.blocked_rx_drain_count =
            context->statistics.blocked_rx_drain_count + 1u;
        if (context->pending_reclaim_count > 0u) {
            return rtos_tasks_retry_pending_reclaims(
                context,
                RTOS_FIRMWARE_PENDING_RECLAIM_CAPACITY);
        }
        return 0u;
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
            context->state =
                ((context->state == RTOS_TASKS_STATE_RECOVERY) ||
                 (context->state == RTOS_TASKS_STATE_RECLAIM_BLOCKED)) ?
                RTOS_TASKS_STATE_RECLAIM_BLOCKED :
                RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL;
            context->statistics.reclaim_blocked_count =
                context->statistics.reclaim_blocked_count + 1u;
            rtos_monitor_record_error(&context->monitor,
                                      RTOS_MONITOR_ERROR_RECLAIM_BLOCKED,
                                      tasks_now(context));
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
        rtos_monitor_clear_error(&context->monitor,
                                 RTOS_MONITOR_ERROR_RECLAIM_BLOCKED);
        context->state = (context->monitor.recovery.recovery_pending ||
                          context->monitor.recovery.recovery_active) ?
            RTOS_TASKS_STATE_RECOVERY : RTOS_TASKS_STATE_NORMAL;
    }

    return written;
}

/**
 * @brief 计算 descriptor ring 当前占用。
 *
 * @param ring descriptor ring。
 * @return write_seq - read_seq。
 */
static uint32_t descriptor_ring_used(const put_shm_descriptor_ring_t *ring)
{
    if (ring == 0) {
        return 0u;
    }

    return ring->producer.write_seq - ring->consumer.read_seq;
}

/**
 * @brief 计算 reclaim ring 当前占用。
 *
 * @param ring reclaim ring。
 * @return write_seq - read_seq。
 */
static uint32_t reclaim_ring_used(const put_shm_reclaim_ring_t *ring)
{
    if (ring == 0) {
        return 0u;
    }

    return ring->producer.write_seq - ring->consumer.read_seq;
}

/**
 * @brief 检查共享内存 region header 是否仍匹配冻结 ABI。
 *
 * @param region 共享内存 region。
 * @return true 表示 header 可接受。
 */
static bool shm_region_header_is_valid(const put_shm_region_t *region)
{
    if (region == 0) {
        return false;
    }

    return (region->header.magic == PUT_SHM_REGION_MAGIC) &&
           (region->header.version == PUT_SHM_IPC_VERSION) &&
           (region->header.header_size == (uint16_t)sizeof(put_shm_region_header_t)) &&
           (region->header.region_size == PUT_SHM_REGION_SIZE);
}

/**
 * @brief 检查 descriptor ring header 是否匹配预期。
 *
 * @param ring descriptor ring。
 * @param kind 预期 ring 类型。
 * @param interface_id 预期接口 ID。
 * @param direction 预期通知方向。
 * @return true 表示 header 匹配。
 */
static bool descriptor_ring_header_is_valid(const put_shm_descriptor_ring_t *ring,
                                            put_shm_ring_kind_t kind,
                                            uint8_t interface_id,
                                            put_shm_direction_t direction)
{
    if (ring == 0) {
        return false;
    }

    return (ring->header.magic == PUT_SHM_RING_MAGIC) &&
           (ring->header.version == PUT_SHM_IPC_VERSION) &&
           (ring->header.depth == PUT_SHM_DESCRIPTOR_RING_DEPTH) &&
           (ring->header.descriptor_size == PUT_SHM_DESCRIPTOR_SIZE) &&
           (ring->header.interface_id == interface_id) &&
           (ring->header.ring_kind == (uint8_t)kind) &&
           (ring->header.direction == (uint8_t)direction);
}

/**
 * @brief 检查 reclaim ring header 是否匹配 ABI。
 *
 * @param ring reclaim ring。
 * @return true 表示 header 匹配。
 */
static bool reclaim_ring_header_is_valid(const put_shm_reclaim_ring_t *ring)
{
    if (ring == 0) {
        return false;
    }

    return (ring->header.magic == PUT_SHM_RING_MAGIC) &&
           (ring->header.version == PUT_SHM_IPC_VERSION) &&
           (ring->header.depth == PUT_SHM_RECLAIM_RING_DEPTH) &&
           (ring->header.descriptor_size == PUT_SHM_RECLAIM_DESCRIPTOR_SIZE) &&
           (ring->header.ring_kind == (uint8_t)PUT_SHM_RING_KIND_RECLAIM);
}

/**
 * @brief 检查 region 内 RX/TX/reclaim ring header。
 *
 * @param region 共享内存 region。
 * @return true 表示全部 ring header 可接受。
 */
static bool shm_ring_headers_are_valid(const put_shm_region_t *region)
{
    uint32_t interface_index; /**< 接口扫描下标。 */

    if (region == 0) {
        return false;
    }

    for (interface_index = 0u; interface_index < PUT_SHM_INTERFACE_COUNT;
         ++interface_index) {
        if (!descriptor_ring_header_is_valid(&region->rx_rings[interface_index],
                                             PUT_SHM_RING_KIND_RX,
                                             (uint8_t)interface_index,
                                             PUT_SHM_DIRECTION_LINUX_TO_RTOS) ||
            !descriptor_ring_header_is_valid(&region->tx_rings[interface_index],
                                             PUT_SHM_RING_KIND_TX,
                                             (uint8_t)interface_index,
                                             PUT_SHM_DIRECTION_RTOS_TO_LINUX)) {
            return false;
        }
    }

    return reclaim_ring_header_is_valid(&region->reclaim_ring);
}

/**
 * @brief 判断是否仍有未冻结 pending reclaim 的本地 frame reference。
 *
 * @param context P3 task 上下文。
 * @return true 表示存在。
 */
static bool unclaimed_frame_reference_exists(const rtos_tasks_context_t *context)
{
    uint32_t frame_id; /**< frame reference 扫描下标。 */

    if (context == 0) {
        return false;
    }

    for (frame_id = 0u; frame_id < PUT_SHM_FRAME_POOL_BLOCK_COUNT; ++frame_id) {
        if (context->frame_references[frame_id].in_use &&
            !pending_reclaim_exists(context, frame_id)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Recovery 时回收不在 router 队列中的本地 frame reference。
 *
 * @param context P3 task 上下文。
 * @param budget 本轮预算。
 * @return 本轮提交 reclaim 的数量。
 */
static uint32_t reclaim_orphan_frame_references(rtos_tasks_context_t *context,
                                                uint32_t budget)
{
    uint32_t frame_id;              /**< frame reference 扫描下标。 */
    uint32_t reclaimed;             /**< 已提交 reclaim 数量。 */
    rtos_route_output_t output;     /**< reclaim 输出。 */
    put_shm_descriptor_t *descriptor; /**< 原始 RX descriptor。 */
    unified_error_t result;         /**< reclaim sink 结果。 */

    if (context == 0) {
        return 0u;
    }

    reclaimed = 0u;
    for (frame_id = 0u;
         (frame_id < PUT_SHM_FRAME_POOL_BLOCK_COUNT) && (reclaimed < budget);
         ++frame_id) {
        if (!context->frame_references[frame_id].in_use ||
            pending_reclaim_exists(context, frame_id)) {
            continue;
        }

        descriptor = &context->frame_references[frame_id].descriptor;
        (void)memset(&output, 0, sizeof(output));
        output.kind = RTOS_ROUTE_OUTPUT_RECLAIM;
        output.frame_id = frame_id;
        output.source_interface = (put_shm_interface_t)descriptor->source_interface;
        output.target_interface = (put_shm_interface_t)descriptor->target_interface;
        (void)memcpy(output.source_cid, descriptor->source_cid, ANYMSG_CID_LENGTH);
        (void)memcpy(output.destination_cid,
                     descriptor->destination_cid,
                     ANYMSG_CID_LENGTH);
        output.type = descriptor->type;
        output.priority = descriptor->priority;
        output.epoch = descriptor->epoch;
        output.flags = descriptor->flags;
        output.reclaim_reason = PUT_SHM_RECLAIM_REASON_QUEUE_FULL;

        result = rtos_tasks_reclaim_sink(&output, context);
        if (result != UNIFIED_OK) {
            break;
        }
        reclaimed = reclaimed + 1u;
    }

    return reclaimed;
}

/**
 * @brief 更新单个 stuck 监控项。
 *
 * @param context P3 task 上下文。
 * @param active 当前是否处于异常条件。
 * @param since_ms 首次观察到异常的时间戳。
 * @param reason 错误 reason bit。
 * @param now_ms 当前时间。
 * @return true 表示当前条件已达到 stuck 阈值。
 */
static bool update_stuck_monitor(rtos_tasks_context_t *context,
                                 bool active,
                                 uint32_t *since_ms,
                                 rtos_monitor_error_reason_t reason,
                                 uint32_t now_ms)
{
    if ((context == 0) || (since_ms == 0)) {
        return false;
    }

    if (!active) {
        *since_ms = 0u;
        return false;
    }

    if (*since_ms == 0u) {
        *since_ms = now_ms;
    }

    if ((now_ms - *since_ms) >= RTOS_FIRMWARE_MONITOR_STUCK_WARN_MS) {
        rtos_monitor_record_error(&context->monitor, reason, now_ms);
        return true;
    }

    return false;
}

/**
 * @brief Host 注入 Linux heartbeat seq。
 *
 * @param context P3 task 上下文。
 * @param linux_heartbeat_seq Linux heartbeat seq。
 */
void rtos_tasks_observe_linux_heartbeat(rtos_tasks_context_t *context,
                                        uint32_t linux_heartbeat_seq)
{
    if (context != 0) {
        rtos_monitor_observe_linux_heartbeat(&context->monitor,
                                             linux_heartbeat_seq,
                                             tasks_now(context));
    }
}

/**
 * @brief 单步执行 Heartbeat Task。
 *
 * @param context P3 task 上下文。
 * @return 本轮发生的状态动作数量。
 */
uint32_t rtos_heartbeat_task_run_once(rtos_tasks_context_t *context)
{
    uint32_t now_ms;                         /**< 当前时间。 */
    uint32_t actions;                        /**< 状态动作计数。 */
    uint32_t endpoint_transitions;           /**< 端心跳状态转换数。 */
    rtos_linux_heartbeat_state_t hb_state;   /**< Linux heartbeat 状态。 */

    if (context == 0) {
        return 0u;
    }

    actions = 0u;
    now_ms = tasks_now(context);
    rtos_monitor_tick_rtos_heartbeat(&context->monitor, now_ms);
    hb_state = rtos_monitor_poll_linux_heartbeat(&context->monitor, now_ms);
    endpoint_transitions =
        rtos_endpoint_heartbeat_scan_timeouts(&context->router.endpoint_heartbeat, now_ms);
    if (endpoint_transitions > 0u) {
        actions = actions + endpoint_transitions;
        rtos_monitor_record_error(&context->monitor,
                                  RTOS_MONITOR_ERROR_ENDPOINT_HEARTBEAT,
                                  now_ms);
    }

    if (hb_state == RTOS_LINUX_HEARTBEAT_STATE_GLOBAL_DEGRADED) {
        if (context->state == RTOS_TASKS_STATE_NORMAL) {
            context->state = RTOS_TASKS_STATE_DEGRADED;
            actions = actions + 1u;
        }
    }

    if (context->monitor.recovery.recovery_pending) {
        context->state = RTOS_TASKS_STATE_RECOVERY;
        actions = actions + 1u;
    }

    return actions;
}

/**
 * @brief 单步执行 Error Monitor Task。
 *
 * @param context P3 task 上下文。
 * @return 当前记录的错误 bit 数量近似值。
 */
uint32_t rtos_error_monitor_task_run_once(rtos_tasks_context_t *context)
{
    uint32_t now_ms;                  /**< 当前时间。 */
    uint32_t interface_index;         /**< 接口扫描下标。 */
    uint32_t used;                    /**< ring 当前占用。 */
    uint32_t tx_notify_fail_count;    /**< TX/reclaim notify fail 总数。 */
    uint32_t errors;                  /**< 错误计数。 */
    bool any_rx_backlog;              /**< 是否存在 RX backlog stuck。 */
    bool any_tx_full;                 /**< 是否存在 TX full stuck。 */
    bool pending_stuck;               /**< pending bit 是否 stuck。 */
    bool reclaim_stuck;               /**< reclaim 是否 blocked。 */
    rtos_router_statistics_t router_statistics; /**< router 统计。 */

    if ((context == 0) || (context->ipc == 0) || (context->ipc->region == 0)) {
        return 0u;
    }

    now_ms = tasks_now(context);
    errors = 0u;
    any_rx_backlog = false;
    any_tx_full = false;
    pending_stuck = false;
    reclaim_stuck = false;

    if (!shm_region_header_is_valid(context->ipc->region)) {
        rtos_monitor_record_error(&context->monitor,
                                  RTOS_MONITOR_ERROR_SHM_FORMAT,
                                  now_ms);
        rtos_monitor_mark_recovery_pending(&context->monitor,
                                           RTOS_RECOVERY_TRIGGER_SHM_REBUILT);
        context->state = RTOS_TASKS_STATE_DEGRADED;
        return 1u;
    }
    if (!shm_ring_headers_are_valid(context->ipc->region)) {
        rtos_monitor_record_error(&context->monitor,
                                  RTOS_MONITOR_ERROR_SHM_FORMAT,
                                  now_ms);
        rtos_monitor_mark_recovery_pending(&context->monitor,
                                           RTOS_RECOVERY_TRIGGER_RING_DESCRIPTOR);
        context->state = RTOS_TASKS_STATE_DEGRADED;
        return 1u;
    }
    rtos_monitor_clear_error(&context->monitor, RTOS_MONITOR_ERROR_SHM_FORMAT);

    if (context->ipc->region->header.linux_epoch != context->last_linux_epoch) {
        context->last_linux_epoch = context->ipc->region->header.linux_epoch;
        rtos_monitor_mark_recovery_pending(&context->monitor,
                                           RTOS_RECOVERY_TRIGGER_LINUX_EPOCH);
        context->state = RTOS_TASKS_STATE_RECOVERY;
        errors = errors + 1u;
    }

    for (interface_index = 0u; interface_index < PUT_SHM_INTERFACE_COUNT;
         ++interface_index) {
        used = descriptor_ring_used(&context->ipc->region->rx_rings[interface_index]);
        if (update_stuck_monitor(context,
                                 used > 0u,
                                 &context->rx_backlog_since_ms[interface_index],
                                 RTOS_MONITOR_ERROR_RX_BACKLOG,
                                 now_ms)) {
            any_rx_backlog = true;
        }

        used = descriptor_ring_used(&context->ipc->region->tx_rings[interface_index]);
        if (update_stuck_monitor(context,
                                 used >= PUT_SHM_DESCRIPTOR_RING_DEPTH,
                                 &context->tx_full_since_ms[interface_index],
                                 RTOS_MONITOR_ERROR_TX_RING_FULL,
                                 now_ms)) {
            any_tx_full = true;
        }
    }

    pending_stuck =
        update_stuck_monitor(context,
                             context->ipc->region->rx_pending_bitmap.bits != 0u,
                             &context->rx_pending_since_ms,
                             RTOS_MONITOR_ERROR_PENDING_STUCK,
                             now_ms) ||
        update_stuck_monitor(context,
                             context->ipc->region->tx_pending_bitmap.bits != 0u,
                             &context->tx_pending_since_ms,
                             RTOS_MONITOR_ERROR_PENDING_STUCK,
                             now_ms) ||
        update_stuck_monitor(context,
                             context->ipc->region->reclaim_pending.bits != 0u,
                             &context->reclaim_pending_since_ms,
                             RTOS_MONITOR_ERROR_PENDING_STUCK,
                             now_ms);

    reclaim_stuck =
        update_stuck_monitor(context,
                             (context->pending_reclaim_count > 0u) ||
                                 (reclaim_ring_used(&context->ipc->region->reclaim_ring) >=
                                  PUT_SHM_RECLAIM_RING_DEPTH),
                             &context->reclaim_blocked_since_ms,
                             RTOS_MONITOR_ERROR_RECLAIM_BLOCKED,
                             now_ms);

    if (!any_rx_backlog) {
        rtos_monitor_clear_error(&context->monitor, RTOS_MONITOR_ERROR_RX_BACKLOG);
    }
    if (!any_tx_full) {
        rtos_monitor_clear_error(&context->monitor, RTOS_MONITOR_ERROR_TX_RING_FULL);
    }
    if (!pending_stuck) {
        rtos_monitor_clear_error(&context->monitor, RTOS_MONITOR_ERROR_PENDING_STUCK);
    }
    if (!reclaim_stuck && (context->pending_reclaim_count == 0u)) {
        rtos_monitor_clear_error(&context->monitor, RTOS_MONITOR_ERROR_RECLAIM_BLOCKED);
    }

    tx_notify_fail_count = context->ipc->region->reclaim_ring.producer.notify_fail_count;
    for (interface_index = 0u; interface_index < PUT_SHM_INTERFACE_COUNT;
         ++interface_index) {
        tx_notify_fail_count = tx_notify_fail_count +
            context->ipc->region->tx_rings[interface_index].producer.notify_fail_count;
    }
    if (tx_notify_fail_count > 0u) {
        rtos_monitor_record_error(&context->monitor,
                                  RTOS_MONITOR_ERROR_MAILBOX_NOTIFY,
                                  now_ms);
    }

    rtos_router_get_statistics(&context->router, &router_statistics);
    if ((router_statistics.drop_reason_count[PUT_SHM_RECLAIM_REASON_TTL_EXPIRED] > 0u) ||
        (router_statistics.drop_reason_count[PUT_SHM_RECLAIM_REASON_EPOCH_MISMATCH] > 0u) ||
        (router_statistics.drop_reason_count[PUT_SHM_RECLAIM_REASON_NO_ROUTE] > 0u)) {
        rtos_monitor_record_error(&context->monitor,
                                  RTOS_MONITOR_ERROR_ROUTE_DROPS,
                                  now_ms);
    }

    rtos_statistics_task_run_once(context);
    return errors + ((context->monitor.error_state.error_bits != 0u) ? 1u : 0u);
}

/**
 * @brief 单步执行 Recovery Task。
 *
 * @param context P3 task 上下文。
 * @param budget 本轮 reclaim 预算。
 * @return 本轮提交 reclaim 的数量。
 */
uint32_t rtos_recovery_task_run_once(rtos_tasks_context_t *context,
                                     uint32_t budget)
{
    uint32_t processed;                  /**< 本轮处理数量。 */
    uint32_t remaining_budget;           /**< 剩余预算。 */
    unified_error_t result;              /**< IPC reattach 结果。 */
    rtos_route_table_snapshot_t table;   /**< route table 快照。 */

    if ((context == 0) || (context->ipc == 0) || (context->ipc->region == 0)) {
        return 0u;
    }

    if (context->state != RTOS_TASKS_STATE_RECOVERY) {
        if (!context->monitor.recovery.recovery_pending &&
            !context->monitor.recovery.recovery_active) {
            return 0u;
        }
        context->state = RTOS_TASKS_STATE_RECOVERY;
    }

    if (!context->monitor.recovery.recovery_active) {
        rtos_monitor_mark_recovery_started(&context->monitor);
    }

    processed = 0u;
    remaining_budget = budget;
    if ((remaining_budget > 0u) && (context->pending_reclaim_count > 0u)) {
        processed = processed + rtos_tasks_retry_pending_reclaims(context, remaining_budget);
        remaining_budget = (processed >= budget) ? 0u : (budget - processed);
    }

    if ((remaining_budget > 0u) && (context->pending_reclaim_count == 0u)) {
        processed = processed + rtos_router_reclaim_queued(&context->router,
                                                           PUT_SHM_RECLAIM_REASON_QUEUE_FULL,
                                                           remaining_budget);
        remaining_budget = (processed >= budget) ? 0u : (budget - processed);
    }

    if ((remaining_budget > 0u) &&
        (rtos_router_get_queued_count(&context->router) == 0u) &&
        (context->pending_reclaim_count == 0u)) {
        processed = processed + reclaim_orphan_frame_references(context, remaining_budget);
    }

    if ((context->pending_reclaim_count > 0u) ||
        (context->state == RTOS_TASKS_STATE_RECLAIM_BLOCKED)) {
        context->state = RTOS_TASKS_STATE_RECLAIM_BLOCKED;
        return processed;
    }

    if ((rtos_router_get_queued_count(&context->router) > 0u) ||
        unclaimed_frame_reference_exists(context)) {
        context->state = RTOS_TASKS_STATE_RECOVERY;
        return processed;
    }

    result = rtos_shm_ipc_attach(context->ipc,
                                 context->ipc->region,
                                 &context->ipc->platform_ops);
    if (result != UNIFIED_OK) {
        rtos_monitor_record_error(&context->monitor,
                                  RTOS_MONITOR_ERROR_SHM_FORMAT,
                                  tasks_now(context));
        context->state = RTOS_TASKS_STATE_DEGRADED;
        return processed;
    }

    context->last_linux_epoch = context->ipc->region->header.linux_epoch;
    context->monitor.recovery.recovery_epoch = context->last_linux_epoch;
    rtos_router_set_linux_epoch(&context->router, context->last_linux_epoch);

    rtos_router_get_route_table(&context->router, &table);
    if (!table.valid) {
        rtos_router_route_table_default(&table);
        (void)rtos_router_set_route_table(&context->router, &table);
    }

    rtos_endpoint_heartbeat_clear_entries(&context->router.endpoint_heartbeat);
    rtos_monitor_clear_error(&context->monitor, RTOS_MONITOR_ERROR_SHM_FORMAT);
    rtos_monitor_clear_error(&context->monitor, RTOS_MONITOR_ERROR_RECLAIM_BLOCKED);
    rtos_monitor_mark_recovery_done(&context->monitor);
    context->statistics.recovery_count = context->monitor.statistics.recovery_count;
    context->state = RTOS_TASKS_STATE_NORMAL;
    (void)rtos_statistics_task_run_once(context);
    return processed;
}

/**
 * @brief 单步执行 Statistics Task，聚合 task/router/endpoint/monitor 统计。
 *
 * @param context P3 task 上下文。
 * @return 新的统计快照序号。
 */
uint32_t rtos_statistics_task_run_once(rtos_tasks_context_t *context)
{
    rtos_router_statistics_t router_statistics;     /**< router 统计。 */
    rtos_endpoint_heartbeat_statistics_t endpoint_statistics; /**< endpoint 统计。 */
    uint32_t interface_index;                       /**< 接口扫描下标。 */
    uint32_t reason;                                /**< reclaim reason 下标。 */
    uint32_t doorbell_tx_count;                     /**< RTOS->Linux doorbell 数。 */
    uint32_t mailbox_fail_count;                    /**< notify fail 数。 */
    uint32_t tx_ring_full_count;                    /**< TX ring full 数。 */

    if (context == 0) {
        return 0u;
    }

    rtos_router_get_statistics(&context->router, &router_statistics);
    rtos_endpoint_heartbeat_get_statistics(&context->router.endpoint_heartbeat,
                                           &endpoint_statistics);

    context->monitor.statistics.snapshot_sequence =
        context->monitor.statistics.snapshot_sequence + 1u;
    context->monitor.statistics.rx_ring_drain_count =
        context->statistics.rx_drain_count;
    context->monitor.statistics.tx_ring_write_count =
        context->statistics.tx_enqueue_count;
    context->monitor.statistics.route_success_count =
        router_statistics.routed_count;
    context->monitor.statistics.route_miss_count =
        router_statistics.drop_reason_count[PUT_SHM_RECLAIM_REASON_NO_ROUTE];
    context->monitor.statistics.ttl_drop_count =
        router_statistics.drop_reason_count[PUT_SHM_RECLAIM_REASON_TTL_EXPIRED];
    context->monitor.statistics.epoch_drop_count =
        router_statistics.drop_reason_count[PUT_SHM_RECLAIM_REASON_EPOCH_MISMATCH];
    context->monitor.statistics.reclaim_ring_full_count =
        context->statistics.reclaim_full_count;
    context->monitor.statistics.reclaim_blocked_count =
        context->statistics.reclaim_blocked_count;
    context->monitor.statistics.pending_reclaim_retry_count =
        context->statistics.pending_reclaim_retry_count;
    context->monitor.statistics.auth_failed_drop_count =
        router_statistics.auth_failed_count;
    context->monitor.statistics.integrity_failed_drop_count =
        router_statistics.integrity_failed_count;
    context->monitor.statistics.replay_drop_count =
        router_statistics.replay_dropped_count;
    context->monitor.statistics.invalid_descriptor_count =
        context->statistics.rx_dequeue_error_count;
    context->monitor.statistics.invalid_descriptor_no_reclaim_count =
        context->statistics.invalid_descriptor_no_reclaim_count;
    context->monitor.statistics.invalid_anymsg_count =
        router_statistics.invalid_header_count + router_statistics.invalid_type_count;
    context->monitor.statistics.endpoint_hb_rx_count =
        endpoint_statistics.rx_count;
    context->monitor.statistics.endpoint_hb_invalid_count =
        endpoint_statistics.invalid_count;
    context->monitor.statistics.endpoint_hb_timeout_count =
        endpoint_statistics.timeout_count;
    context->monitor.statistics.endpoint_hb_recover_count =
        endpoint_statistics.recover_count;
    context->monitor.statistics.endpoint_hb_table_full_count =
        endpoint_statistics.table_full_count;

    for (reason = 0u; reason <= (uint32_t)PUT_SHM_RECLAIM_REASON_QUEUE_FULL; ++reason) {
        context->monitor.statistics.drop_reason_count[reason] =
            router_statistics.drop_reason_count[reason];
    }

    for (interface_index = 0u; interface_index < PUT_SHM_INTERFACE_COUNT;
         ++interface_index) {
        context->monitor.statistics.routed_by_interface[interface_index] =
            router_statistics.routed_by_interface[interface_index];
    }

    for (interface_index = 0u; interface_index < RTOS_FIRMWARE_PRIORITY_COUNT;
         ++interface_index) {
        context->monitor.statistics.routed_by_priority[interface_index] =
            router_statistics.routed_by_priority[interface_index];
    }

    doorbell_tx_count = 0u;
    mailbox_fail_count = context->ipc != 0 && context->ipc->region != 0 ?
        context->ipc->region->reclaim_ring.producer.notify_fail_count : 0u;
    tx_ring_full_count = 0u;
    if ((context->ipc != 0) && (context->ipc->region != 0)) {
        doorbell_tx_count =
            context->ipc->region->reclaim_ring.producer.notify_count;
        for (interface_index = 0u; interface_index < PUT_SHM_INTERFACE_COUNT;
             ++interface_index) {
            doorbell_tx_count = doorbell_tx_count +
                context->ipc->region->tx_rings[interface_index].producer.notify_count;
            mailbox_fail_count = mailbox_fail_count +
                context->ipc->region->tx_rings[interface_index].producer.notify_fail_count;
            tx_ring_full_count = tx_ring_full_count +
                context->ipc->region->tx_rings[interface_index].producer.drop_count;
        }
    }

    context->monitor.statistics.doorbell_tx_count = doorbell_tx_count;
    context->monitor.statistics.mailbox_fail_count = mailbox_fail_count;
    context->monitor.statistics.tx_ring_full_count = tx_ring_full_count;
    context->statistics.monitor_statistics = context->monitor.statistics;
    return context->monitor.statistics.snapshot_sequence;
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
        return RTOS_TASKS_STATE_DEGRADED;
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
