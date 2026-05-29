/**
 * @file rtos_priority_queue.c
 * @brief rtos_firmware P1 优先级队列实现。
 * @author Yukikaze
 */
#include "rtos_priority_queue.h"

#include <string.h>

/**
 * @brief 重置当前调度轮次的配额计数。
 *
 * @param queue 优先级队列上下文。
 */
static void reset_quota_round(rtos_priority_queue_t *queue)
{
    if (queue != 0) {
        (void)memset(queue->quota_used, 0, sizeof(queue->quota_used));
        queue->scheduler_priority = 0u;
    }
}

/**
 * @brief 判断队列中是否存在指定优先级的队列项。
 *
 * @param queue 优先级队列上下文。
 * @param priority 待检查的优先级。
 * @return true 表示存在，false 表示不存在。
 */
static bool has_priority(const rtos_priority_queue_t *queue, uint8_t priority)
{
    uint32_t i; /**< 队列项扫描下标。 */

    if ((queue == 0) || !rtos_priority_queue_priority_is_valid(priority)) {
        return false;
    }

    for (i = 0u; i < queue->count; ++i) {
        if (queue->items[i].priority == priority) {
            return true;
        }
    }

    return false;
}

/**
 * @brief 查找指定优先级中最早入队的队列项。
 *
 * @param queue 优先级队列上下文。
 * @param priority 待查找的优先级。
 * @param out_index 输出队列项下标。
 * @return true 表示找到，false 表示未找到。
 */
static bool find_oldest_priority(const rtos_priority_queue_t *queue,
                                 uint8_t priority,
                                 uint32_t *out_index)
{
    uint32_t i;            /**< 队列项扫描下标。 */
    uint32_t oldest_index; /**< 最早匹配项下标。 */
    bool found;            /**< 是否找到匹配项。 */

    if ((queue == 0) || (out_index == 0) ||
        !rtos_priority_queue_priority_is_valid(priority)) {
        return false;
    }

    found = false;
    oldest_index = 0u;
    for (i = 0u; i < queue->count; ++i) {
        if (queue->items[i].priority == priority) {
            if (!found ||
                (queue->items[i].enqueue_time_ms < queue->items[oldest_index].enqueue_time_ms)) {
                oldest_index = i;
                found = true;
            }
        }
    }

    if (found) {
        *out_index = oldest_index;
    }

    return found;
}

/**
 * @brief 通过压缩数组移除指定下标的队列项。
 *
 * @param queue 优先级队列上下文。
 * @param index 待移除队列项下标。
 * @param out_item 输出被移除的队列项，可为 NULL。
 */
static void remove_index(rtos_priority_queue_t *queue,
                         uint32_t index,
                         rtos_priority_queue_item_t *out_item)
{
    uint32_t i; /**< 数组搬移下标。 */

    if ((queue == 0) || (index >= queue->count)) {
        return;
    }

    if (out_item != 0) {
        *out_item = queue->items[index];
    }

    for (i = index; (i + 1u) < queue->count; ++i) {
        queue->items[i] = queue->items[i + 1u];
    }
    queue->count = queue->count - 1u;
}

/**
 * @brief 为新队列项查找可驱逐的旧队列项。
 *
 * @param queue 优先级队列上下文。
 * @param new_priority 新队列项优先级。
 * @param out_index 输出可驱逐队列项下标。
 * @return true 表示找到可驱逐项，false 表示无可驱逐项。
 */
static bool find_evictable_item(const rtos_priority_queue_t *queue,
                                uint8_t new_priority,
                                uint32_t *out_index)
{
    int priority; /**< 从低重要性到高重要性扫描的候选优先级。 */

    if ((queue == 0) || (out_index == 0) ||
        !rtos_priority_queue_priority_is_valid(new_priority)) {
        return false;
    }

    for (priority = (int)RTOS_FIRMWARE_PRIORITY_COUNT - 1; priority >= (int)new_priority;
         --priority) {
        if ((priority == 0) && (new_priority == 0u)) {
            /* P1 不为新的 priority 0 帧驱逐已有 priority 0 帧。 */
            continue;
        }

        if (find_oldest_priority(queue, (uint8_t)priority, out_index)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief 使用默认配额初始化固定容量优先级队列。
 *
 * @param queue 优先级队列上下文。
 */
void rtos_priority_queue_init(rtos_priority_queue_t *queue)
{
    if (queue == 0) {
        return;
    }

    (void)memset(queue, 0, sizeof(*queue));
    queue->quota[0] = (uint8_t)RTOS_FIRMWARE_PRIORITY_0_QUOTA;
    queue->quota[1] = (uint8_t)RTOS_FIRMWARE_PRIORITY_1_QUOTA;
    queue->quota[2] = (uint8_t)RTOS_FIRMWARE_PRIORITY_2_QUOTA;
    queue->quota[3] = (uint8_t)RTOS_FIRMWARE_PRIORITY_3_QUOTA;
}

/**
 * @brief 判断 priority 是否为 P1 支持的 0..3。
 *
 * @param priority 待检查优先级。
 * @return true 表示合法，false 表示非法。
 */
bool rtos_priority_queue_priority_is_valid(uint8_t priority)
{
    return priority < (uint8_t)RTOS_FIRMWARE_PRIORITY_COUNT;
}

/**
 * @brief 入队一个队列项，必要时驱逐同等或更低优先级的最旧队列项。
 *
 * @param queue 优先级队列上下文。
 * @param item 新队列项。
 * @param evicted_item 输出被驱逐的队列项，可为 NULL。
 * @param out_evicted 输出是否发生驱逐，可为 NULL。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_priority_queue_enqueue(rtos_priority_queue_t *queue,
                                            const rtos_priority_queue_item_t *item,
                                            rtos_priority_queue_item_t *evicted_item,
                                            bool *out_evicted)
{
    uint32_t evict_index; /**< 被选中驱逐的队列下标。 */

    if ((queue == 0) || (item == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (out_evicted != 0) {
        *out_evicted = false;
    }

    if (!rtos_priority_queue_priority_is_valid(item->priority)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (queue->count < RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY) {
        queue->items[queue->count] = *item;
        queue->count = queue->count + 1u;
        return UNIFIED_OK;
    }

    if (!find_evictable_item(queue, item->priority, &evict_index)) {
        return UNIFIED_ERR_IPC_QUEUE_FULL;
    }

    remove_index(queue, evict_index, evicted_item);
    if (out_evicted != 0) {
        *out_evicted = true;
    }

    queue->items[queue->count] = *item;
    queue->count = queue->count + 1u;
    return UNIFIED_OK;
}

/**
 * @brief 按严格优先级和配额防饥饿策略出队一个队列项。
 *
 * @param queue 优先级队列上下文。
 * @param out_item 输出队列项。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_priority_queue_dequeue(rtos_priority_queue_t *queue,
                                            rtos_priority_queue_item_t *out_item)
{
    uint32_t scan_count; /**< 本次调用中调度优先级推进次数。 */
    uint8_t priority;   /**< 当前调度优先级。 */
    uint32_t index;     /**< 被选中优先级的最旧队列项下标。 */

    if ((queue == 0) || (out_item == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (queue->count == 0u) {
        return UNIFIED_ERR_IPC_QUEUE_EMPTY;
    }

    for (scan_count = 0u; scan_count < (RTOS_FIRMWARE_PRIORITY_COUNT * 2u); ++scan_count) {
        if (queue->scheduler_priority >= (uint8_t)RTOS_FIRMWARE_PRIORITY_COUNT) {
            reset_quota_round(queue);
        }

        priority = queue->scheduler_priority;
        if ((queue->quota_used[priority] < queue->quota[priority]) &&
            has_priority(queue, priority) &&
            find_oldest_priority(queue, priority, &index)) {
            remove_index(queue, index, out_item);
            queue->quota_used[priority] = queue->quota_used[priority] + 1u;
            if ((queue->quota_used[priority] >= queue->quota[priority]) ||
                !has_priority(queue, priority)) {
                queue->scheduler_priority = queue->scheduler_priority + 1u;
            }
            return UNIFIED_OK;
        }

        queue->scheduler_priority = queue->scheduler_priority + 1u;
    }

    reset_quota_round(queue);
    return rtos_priority_queue_dequeue(queue, out_item);
}

/**
 * @brief 读取队列深度快照。
 *
 * @param queue 优先级队列上下文。
 * @param out_depth 输出队列深度快照。
 */
void rtos_priority_queue_get_depth(const rtos_priority_queue_t *queue,
                                   rtos_priority_queue_depth_t *out_depth)
{
    uint32_t i; /**< 队列项扫描下标。 */

    if (out_depth == 0) {
        return;
    }

    (void)memset(out_depth, 0, sizeof(*out_depth));
    out_depth->capacity = RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY;

    if (queue == 0) {
        return;
    }

    out_depth->total_depth = queue->count;
    for (i = 0u; i < queue->count; ++i) {
        if (rtos_priority_queue_priority_is_valid(queue->items[i].priority)) {
            out_depth->depth[queue->items[i].priority] =
                out_depth->depth[queue->items[i].priority] + 1u;
        }
    }
}
