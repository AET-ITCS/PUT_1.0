/**
 * @file rtos_priority_queue.c
 * @brief rtos_firmware P1 priority queue implementation.
 * @author Yukikaze
 */
#include "rtos_priority_queue.h"

#include <string.h>

/**
 * @brief Reset per-round quota counters.
 */
static void reset_quota_round(rtos_priority_queue_t *queue)
{
    if (queue != 0) {
        (void)memset(queue->quota_used, 0, sizeof(queue->quota_used));
        queue->scheduler_priority = 0u;
    }
}

/**
 * @brief Return true when the queue contains an item at the given priority.
 */
static bool has_priority(const rtos_priority_queue_t *queue, uint8_t priority)
{
    uint32_t i; /**< Item scan index. */

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
 * @brief Find the oldest item with the requested priority.
 */
static bool find_oldest_priority(const rtos_priority_queue_t *queue,
                                 uint8_t priority,
                                 uint32_t *out_index)
{
    uint32_t i;            /**< Item scan index. */
    uint32_t oldest_index; /**< Oldest matching index. */
    bool found;            /**< Whether a matching item was found. */

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
 * @brief Remove one item by compacting the array.
 */
static void remove_index(rtos_priority_queue_t *queue,
                         uint32_t index,
                         rtos_priority_queue_item_t *out_item)
{
    uint32_t i; /**< Shift index. */

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
 * @brief Find an item that can be evicted for a new item priority.
 */
static bool find_evictable_item(const rtos_priority_queue_t *queue,
                                uint8_t new_priority,
                                uint32_t *out_index)
{
    int priority; /**< Candidate priority scanned low-to-high importance. */

    if ((queue == 0) || (out_index == 0) ||
        !rtos_priority_queue_priority_is_valid(new_priority)) {
        return false;
    }

    for (priority = (int)RTOS_FIRMWARE_PRIORITY_COUNT - 1; priority >= (int)new_priority;
         --priority) {
        if ((priority == 0) && (new_priority == 0u)) {
            /* P1 never evicts priority 0 for another priority 0 frame. */
            continue;
        }

        if (find_oldest_priority(queue, (uint8_t)priority, out_index)) {
            return true;
        }
    }

    return false;
}

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

bool rtos_priority_queue_priority_is_valid(uint8_t priority)
{
    return priority < (uint8_t)RTOS_FIRMWARE_PRIORITY_COUNT;
}

unified_error_t rtos_priority_queue_enqueue(rtos_priority_queue_t *queue,
                                            const rtos_priority_queue_item_t *item,
                                            rtos_priority_queue_item_t *evicted_item,
                                            bool *out_evicted)
{
    uint32_t evict_index; /**< Queue index selected for eviction. */

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

unified_error_t rtos_priority_queue_dequeue(rtos_priority_queue_t *queue,
                                            rtos_priority_queue_item_t *out_item)
{
    uint32_t scan_count; /**< Number of scheduler advances in this call. */
    uint8_t priority;   /**< Current scheduler priority. */
    uint32_t index;     /**< Oldest item index for the selected priority. */

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

void rtos_priority_queue_get_depth(const rtos_priority_queue_t *queue,
                                   rtos_priority_queue_depth_t *out_depth)
{
    uint32_t i; /**< Item scan index. */

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
