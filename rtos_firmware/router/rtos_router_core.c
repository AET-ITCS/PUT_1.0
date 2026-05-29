/**
 * @file rtos_router_core.c
 * @brief 带可替换 TX 和 reclaim sink 的 P1 路由核心实现。
 * @author Yukikaze
 */
#include "rtos_router.h"

#include <string.h>

/**
 * @brief 各优先级 TX 重试上限。
 */
static const uint8_t k_retry_limit[RTOS_FIRMWARE_PRIORITY_COUNT] = {
    3u,
    2u,
    1u,
    0u,
};

/**
 * @brief 未注入时间源时使用的默认单调时间源。
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
 * @brief 读取当前路由时间。
 *
 * @param router 路由上下文。
 * @return 当前时间，单位毫秒。
 */
static uint32_t router_now(const rtos_router_context_t *router)
{
    if ((router == 0) || (router->time_source == 0)) {
        return 0u;
    }

    return router->time_source(router->time_context);
}

/**
 * @brief 复制 route input 中的 CID 字段。
 *
 * @param output 路由输出。
 * @param source_cid 源 CID。
 * @param destination_cid 目的 CID。
 */
static void copy_input_cids(rtos_route_output_t *output,
                            const uint8_t source_cid[ANYMSG_CID_LENGTH],
                            const uint8_t destination_cid[ANYMSG_CID_LENGTH])
{
    (void)memcpy(output->source_cid, source_cid, ANYMSG_CID_LENGTH);
    (void)memcpy(output->destination_cid, destination_cid, ANYMSG_CID_LENGTH);
}

/**
 * @brief 记录 reclaim drop reason 统计。
 *
 * @param router 路由上下文。
 * @param reason reclaim 原因。
 */
static void record_reclaim_stat(rtos_router_context_t *router,
                                put_shm_reclaim_reason_t reason)
{
    if (router == 0) {
        return;
    }

    router->statistics.reclaimed_count = router->statistics.reclaimed_count + 1u;
    if ((uint32_t)reason <= (uint32_t)PUT_SHM_RECLAIM_REASON_QUEUE_FULL) {
        router->statistics.drop_reason_count[(uint32_t)reason] =
            router->statistics.drop_reason_count[(uint32_t)reason] + 1u;
    }
}

/**
 * @brief 通过可选 mock sink 输出 reclaim 结果。
 *
 * @param router 路由上下文。
 * @param output reclaim 输出。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t emit_reclaim(rtos_router_context_t *router,
                                    const rtos_route_output_t *output)
{
    if ((router == 0) || (output == 0)) {
        return UNIFIED_ERR_NULL;
    }

    record_reclaim_stat(router, output->reclaim_reason);
    if (router->sinks.reclaim_sink == 0) {
        return UNIFIED_OK;
    }

    return router->sinks.reclaim_sink(output, router->sinks.user_context);
}

/**
 * @brief 根据 route input 构造并输出 reclaim 结果。
 *
 * @param router 路由上下文。
 * @param input 路由输入。
 * @param reason reclaim 原因。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t reclaim_input(rtos_router_context_t *router,
                                     const rtos_route_input_t *input,
                                     put_shm_reclaim_reason_t reason)
{
    rtos_route_output_t output; /**< reclaim 输出。 */
    uint32_t now_ms;            /**< 当前时间。 */

    if ((router == 0) || (input == 0)) {
        return UNIFIED_ERR_NULL;
    }

    now_ms = router_now(router);
    (void)memset(&output, 0, sizeof(output));
    output.kind = RTOS_ROUTE_OUTPUT_RECLAIM;
    output.frame_id = input->frame_id;
    output.source_interface = input->source_interface;
    output.target_interface = input->source_interface;
    output.type = input->type;
    output.priority = input->priority;
    output.epoch = input->epoch;
    output.flags = input->flags;
    output.latency_ms = now_ms - input->receive_time_ms;
    output.reclaim_reason = reason;
    copy_input_cids(&output, input->source_cid, input->destination_cid);

    return emit_reclaim(router, &output);
}

/**
 * @brief 根据队列项构造并输出 reclaim 结果。
 *
 * @param router 路由上下文。
 * @param item 队列项。
 * @param reason reclaim 原因。
 * @param out_output 输出 reclaim 结果，可为 NULL。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t reclaim_item(rtos_router_context_t *router,
                                    const rtos_priority_queue_item_t *item,
                                    put_shm_reclaim_reason_t reason,
                                    rtos_route_output_t *out_output)
{
    rtos_route_output_t output; /**< reclaim 输出。 */
    uint32_t now_ms;            /**< 当前时间。 */
    unified_error_t result;     /**< sink 回调返回结果。 */

    if ((router == 0) || (item == 0)) {
        return UNIFIED_ERR_NULL;
    }

    now_ms = router_now(router);
    (void)memset(&output, 0, sizeof(output));
    output.kind = RTOS_ROUTE_OUTPUT_RECLAIM;
    output.frame_id = item->frame_id;
    output.source_interface = item->source_interface;
    output.target_interface = item->target_interface;
    output.type = item->type;
    output.priority = item->priority;
    output.retry_count = item->retry_count;
    output.epoch = item->epoch;
    output.flags = item->flags;
    output.latency_ms = now_ms - item->receive_time_ms;
    output.reclaim_reason = reason;
    copy_input_cids(&output, item->source_cid, item->destination_cid);

    result = emit_reclaim(router, &output);
    if (out_output != 0) {
        *out_output = output;
    }

    return result;
}

/**
 * @brief 根据 route input 和已解析目标填充队列项。
 *
 * @param router 路由上下文。
 * @param input 路由输入。
 * @param target_interface 已解析的目标接口。
 * @param out_item 输出队列项。
 */
static void fill_queue_item(rtos_router_context_t *router,
                            const rtos_route_input_t *input,
                            put_shm_interface_t target_interface,
                            rtos_priority_queue_item_t *out_item)
{
    uint32_t now_ms; /**< 当前时间。 */

    now_ms = router_now(router);
    (void)memset(out_item, 0, sizeof(*out_item));
    out_item->frame_id = input->frame_id;
    out_item->source_interface = input->source_interface;
    out_item->target_interface = target_interface;
    (void)memcpy(out_item->source_cid, input->source_cid, ANYMSG_CID_LENGTH);
    (void)memcpy(out_item->destination_cid, input->destination_cid, ANYMSG_CID_LENGTH);
    out_item->type = input->type;
    out_item->priority = input->priority;
    out_item->ttl = input->ttl;
    out_item->epoch = input->epoch;
    out_item->flags = input->flags;
    out_item->receive_time_ms = input->receive_time_ms;
    out_item->enqueue_time_ms = now_ms;
    out_item->route_epoch_seen = router->route_table.active_route_epoch;
    out_item->frame_local_time = input->frame_local_time;
}

/**
 * @brief 记录具体非法帧统计。
 *
 * @param router 路由上下文。
 * @param input 路由输入。
 */
static void record_invalid_input(rtos_router_context_t *router,
                                 const rtos_route_input_t *input)
{
    if ((router == 0) || (input == 0)) {
        return;
    }

    if (!input->anymsg_header_valid) {
        router->statistics.invalid_header_count =
            router->statistics.invalid_header_count + 1u;
    }

    if (!rtos_router_type_is_valid(input->type)) {
        router->statistics.invalid_type_count =
            router->statistics.invalid_type_count + 1u;
    }

    if (!rtos_priority_queue_priority_is_valid(input->priority) &&
        (input->type != ANYMSG_TYPE_ENDPOINT_TO_GATEWAY_HEARTBEAT)) {
        router->statistics.invalid_priority_count =
            router->statistics.invalid_priority_count + 1u;
    }

    if (input->trust == RTOS_ROUTE_TRUST_AUTH_FAILED) {
        router->statistics.auth_failed_count = router->statistics.auth_failed_count + 1u;
    } else if (input->trust == RTOS_ROUTE_TRUST_INTEGRITY_FAILED) {
        router->statistics.integrity_failed_count =
            router->statistics.integrity_failed_count + 1u;
    } else if (input->trust == RTOS_ROUTE_TRUST_REPLAY_DROPPED) {
        router->statistics.replay_dropped_count =
            router->statistics.replay_dropped_count + 1u;
    }
}

/**
 * @brief 判断输入是否应按非法帧回收。
 *
 * @param input 路由输入。
 * @return true 表示应作为 invalid frame 回收，false 表示可继续处理。
 */
static bool input_is_invalid(const rtos_route_input_t *input)
{
    if (input == 0) {
        return true;
    }

    if (!input->anymsg_header_valid || !rtos_router_type_is_valid(input->type) ||
        !rtos_router_trust_is_routable(input->trust)) {
        return true;
    }

    if ((input->type != ANYMSG_TYPE_ENDPOINT_TO_GATEWAY_HEARTBEAT) &&
        !rtos_priority_queue_priority_is_valid(input->priority)) {
        return true;
    }

    return false;
}

/**
 * @brief 初始化 P1 路由上下文。
 *
 * @param router 路由上下文。
 * @param sinks 可替换 sink 回调集合，可为 NULL。
 * @param time_source 时间源回调，可为 NULL。
 * @param time_context 时间源用户上下文。
 */
void rtos_router_init(rtos_router_context_t *router,
                      const rtos_router_sinks_t *sinks,
                      rtos_router_time_source_t time_source,
                      void *time_context)
{
    if (router == 0) {
        return;
    }

    (void)memset(router, 0, sizeof(*router));
    rtos_priority_queue_init(&router->queue);
    rtos_endpoint_heartbeat_init(&router->endpoint_heartbeat);
    rtos_router_route_table_default(&router->route_table);
    if (sinks != 0) {
        router->sinks = *sinks;
    }
    router->time_source = (time_source != 0) ? time_source : default_time_source;
    router->time_context = time_context;
    router->current_linux_epoch = 1u;
}

/**
 * @brief 设置 P1 epoch 检查接受的 Linux epoch。
 *
 * @param router 路由上下文。
 * @param linux_epoch 当前 Linux epoch。
 */
void rtos_router_set_linux_epoch(rtos_router_context_t *router, uint32_t linux_epoch)
{
    if (router != 0) {
        router->current_linux_epoch = linux_epoch;
    }
}

/**
 * @brief 提交一条可信 route input 到 P1 路由核心。
 *
 * @param router 路由上下文。
 * @param input 路由输入。
 * @return UNIFIED_OK 表示提交完成，否则返回公共错误码。
 */
unified_error_t rtos_router_submit(rtos_router_context_t *router,
                                   const rtos_route_input_t *input)
{
    put_shm_interface_t target_interface;       /**< 路由查询结果。 */
    rtos_priority_queue_item_t item;            /**< 新队列项。 */
    rtos_priority_queue_item_t evicted_item;    /**< 背压驱逐出的旧队列项。 */
    bool evicted;                               /**< 本次入队是否驱逐旧队列项。 */
    unified_error_t enqueue_result;             /**< 队列入队结果。 */
    unified_error_t heartbeat_result;           /**< 心跳表更新结果。 */
    uint32_t now_ms;                            /**< 当前时间。 */

    if ((router == 0) || (input == 0)) {
        return UNIFIED_ERR_NULL;
    }

    router->statistics.submitted_count = router->statistics.submitted_count + 1u;
    now_ms = router_now(router);

    if (input_is_invalid(input)) {
        record_invalid_input(router, input);
        return reclaim_input(router, input, PUT_SHM_RECLAIM_REASON_INVALID_FRAME);
    }

    if (input->epoch != router->current_linux_epoch) {
        return reclaim_input(router, input, PUT_SHM_RECLAIM_REASON_EPOCH_MISMATCH);
    }

    if (rtos_router_ttl_is_expired(now_ms, input->receive_time_ms, input->ttl)) {
        return reclaim_input(router, input, PUT_SHM_RECLAIM_REASON_TTL_EXPIRED);
    }

    if (input->type == ANYMSG_TYPE_ENDPOINT_TO_GATEWAY_HEARTBEAT) {
        heartbeat_result = rtos_endpoint_heartbeat_consume(&router->endpoint_heartbeat,
                                                           input->source_cid,
                                                           input->destination_cid,
                                                           input->source_interface,
                                                           now_ms,
                                                           input->frame_local_time);
        (void)heartbeat_result;
        router->statistics.heartbeat_consumed_count =
            router->statistics.heartbeat_consumed_count + 1u;
        return reclaim_input(router, input, PUT_SHM_RECLAIM_REASON_HEARTBEAT_CONSUMED);
    }

    if (rtos_router_route_table_lookup(&router->route_table,
                                       input->destination_cid[0],
                                       &target_interface) != UNIFIED_OK) {
        return reclaim_input(router, input, PUT_SHM_RECLAIM_REASON_NO_ROUTE);
    }

    fill_queue_item(router, input, target_interface, &item);
    enqueue_result = rtos_priority_queue_enqueue(&router->queue,
                                                 &item,
                                                 &evicted_item,
                                                 &evicted);
    if (enqueue_result != UNIFIED_OK) {
        return reclaim_input(router, input, PUT_SHM_RECLAIM_REASON_QUEUE_FULL);
    }

    if (evicted) {
        router->statistics.queue_evicted_count =
            router->statistics.queue_evicted_count + 1u;
        (void)reclaim_item(router,
                           &evicted_item,
                           PUT_SHM_RECLAIM_REASON_QUEUE_FULL,
                           0);
    }

    router->statistics.enqueued_count = router->statistics.enqueued_count + 1u;
    return UNIFIED_OK;
}

/**
 * @brief 调度一个已入队帧到 TX 或 reclaim。
 *
 * @param router 路由上下文。
 * @param out_output 输出本次调度结果，可为 NULL。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_router_schedule_once(rtos_router_context_t *router,
                                          rtos_route_output_t *out_output)
{
    rtos_priority_queue_item_t item;      /**< 已出队队列项。 */
    put_shm_interface_t target_interface; /**< 路由 epoch 刷新后的目标接口。 */
    rtos_route_output_t output;           /**< TX 输出。 */
    unified_error_t result;               /**< 队列或 sink 返回结果。 */
    uint32_t now_ms;                      /**< 当前时间。 */
    uint8_t retry_limit;                  /**< 当前优先级的重试上限。 */

    if (router == 0) {
        return UNIFIED_ERR_NULL;
    }

    result = rtos_priority_queue_dequeue(&router->queue, &item);
    if (result != UNIFIED_OK) {
        return result;
    }

    now_ms = router_now(router);
    if (rtos_router_ttl_is_expired(now_ms, item.receive_time_ms, item.ttl)) {
        return reclaim_item(router, &item, PUT_SHM_RECLAIM_REASON_TTL_EXPIRED, out_output);
    }

    if (item.route_epoch_seen != router->route_table.active_route_epoch) {
        if (rtos_router_route_table_lookup(&router->route_table,
                                           item.destination_cid[0],
                                           &target_interface) != UNIFIED_OK) {
            return reclaim_item(router, &item, PUT_SHM_RECLAIM_REASON_NO_ROUTE, out_output);
        }
        item.target_interface = target_interface;
        item.route_epoch_seen = router->route_table.active_route_epoch;
    }

    retry_limit = k_retry_limit[item.priority];
    for (;;) {
        (void)memset(&output, 0, sizeof(output));
        output.kind = RTOS_ROUTE_OUTPUT_TX;
        output.frame_id = item.frame_id;
        output.source_interface = item.source_interface;
        output.target_interface = item.target_interface;
        output.type = item.type;
        output.priority = item.priority;
        output.retry_count = item.retry_count;
        output.epoch = item.epoch;
        output.flags = item.flags;
        output.latency_ms = now_ms - item.receive_time_ms;
        copy_input_cids(&output, item.source_cid, item.destination_cid);

        result = UNIFIED_OK;
        if (router->sinks.tx_sink != 0) {
            result = router->sinks.tx_sink(&output, router->sinks.user_context);
        }

        if (result == UNIFIED_OK) {
            router->statistics.routed_count = router->statistics.routed_count + 1u;
            router->statistics.routed_by_interface[(uint32_t)item.target_interface] =
                router->statistics.routed_by_interface[(uint32_t)item.target_interface] + 1u;
            router->statistics.routed_by_priority[(uint32_t)item.priority] =
                router->statistics.routed_by_priority[(uint32_t)item.priority] + 1u;
            if (out_output != 0) {
                *out_output = output;
            }
            return UNIFIED_OK;
        }

        if (item.retry_count >= retry_limit) {
            return reclaim_item(router, &item, PUT_SHM_RECLAIM_REASON_QUEUE_FULL, out_output);
        }

        item.retry_count = item.retry_count + 1u;
        router->statistics.tx_retry_count = router->statistics.tx_retry_count + 1u;
    }
}

/**
 * @brief 按 budget 调度本地队列中的多个帧。
 *
 * @param router 路由上下文。
 * @param budget 本轮最多调度数量。
 * @return 本轮实际消费的队列项数量。
 */
uint32_t rtos_router_drain(rtos_router_context_t *router, uint32_t budget)
{
    uint32_t processed;       /**< 已处理队列项数量。 */
    unified_error_t result;   /**< 单次调度返回结果。 */

    if (router == 0) {
        return 0u;
    }

    processed = 0u;
    while (processed < budget) {
        result = rtos_router_schedule_once(router, 0);
        if (result == UNIFIED_ERR_IPC_QUEUE_EMPTY) {
            break;
        }
        if (result != UNIFIED_OK) {
            break;
        }
        processed = processed + 1u;
    }

    return processed;
}

/**
 * @brief 按 Recovery/drop reason 回收本地调度队列中的帧。
 *
 * @param router 路由上下文。
 * @param reason reclaim reason。
 * @param budget 本轮最多回收数量。
 * @return 本轮成功提交到 reclaim sink 的数量。
 */
uint32_t rtos_router_reclaim_queued(rtos_router_context_t *router,
                                    put_shm_reclaim_reason_t reason,
                                    uint32_t budget)
{
    uint32_t processed;                 /**< 已回收队列项数量。 */
    rtos_priority_queue_item_t item;    /**< 已出队队列项。 */
    unified_error_t result;             /**< 队列或 reclaim sink 结果。 */

    if (router == 0) {
        return 0u;
    }

    processed = 0u;
    while (processed < budget) {
        result = rtos_priority_queue_dequeue(&router->queue, &item);
        if (result == UNIFIED_ERR_IPC_QUEUE_EMPTY) {
            break;
        }
        if (result != UNIFIED_OK) {
            break;
        }

        result = reclaim_item(router, &item, reason, 0);
        if (result != UNIFIED_OK) {
            break;
        }
        processed = processed + 1u;
    }

    return processed;
}

/**
 * @brief 读取 router 本地队列深度。
 *
 * @param router 路由上下文。
 * @return 本地队列项数量。
 */
uint32_t rtos_router_get_queued_count(const rtos_router_context_t *router)
{
    if (router == 0) {
        return 0u;
    }

    return router->queue.count;
}

/**
 * @brief 读取路由表快照。
 *
 * @param router 路由上下文。
 * @param out_table 输出路由表快照。
 */
void rtos_router_get_route_table(const rtos_router_context_t *router,
                                 rtos_route_table_snapshot_t *out_table)
{
    if ((router != 0) && (out_table != 0)) {
        *out_table = router->route_table;
    }
}

/**
 * @brief 替换路由表快照。
 *
 * @param router 路由上下文。
 * @param table 新路由表快照。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_router_set_route_table(rtos_router_context_t *router,
                                            const rtos_route_table_snapshot_t *table)
{
    if ((router == 0) || (table == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (!table->valid) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (!rtos_router_route_table_crc_is_valid(table)) {
        router->statistics.route_table_crc_error_count =
            router->statistics.route_table_crc_error_count + 1u;
        return UNIFIED_ERR_CRC;
    }

    router->route_table = *table;
    return UNIFIED_OK;
}

/**
 * @brief 读取路由统计快照。
 *
 * @param router 路由上下文。
 * @param out_statistics 输出统计快照。
 */
void rtos_router_get_statistics(const rtos_router_context_t *router,
                                rtos_router_statistics_t *out_statistics)
{
    if ((router != 0) && (out_statistics != 0)) {
        *out_statistics = router->statistics;
    }
}
