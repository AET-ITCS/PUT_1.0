/**
 * @file rtos_priority_queue.h
 * @brief rtos_firmware P1 priority queue interface.
 * @author Yukikaze
 */
#ifndef RTOS_PRIORITY_QUEUE_H
#define RTOS_PRIORITY_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_firmware_config.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief P1 local priority queue item.
 */
typedef struct {
    uint32_t frame_id;          /**< Trusted Frame Pool block reference. */
    put_shm_interface_t source_interface; /**< Original RX interface. */
    put_shm_interface_t target_interface; /**< Resolved TX interface. */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< anyMSG source CID. */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< anyMSG destination CID. */
    uint8_t type;              /**< anyMSG type. */
    uint8_t priority;          /**< Scheduler priority, 0 is highest. */
    uint8_t ttl;               /**< P1 TTL in milliseconds, 0 disables TTL checks. */
    uint8_t retry_count;       /**< TX retry count accumulated by scheduler. */
    uint32_t epoch;            /**< Linux epoch seen in the route input. */
    uint32_t flags;            /**< Route input flags, carried through to sinks. */
    uint32_t receive_time_ms;  /**< Time when the route input became visible to P1. */
    uint32_t enqueue_time_ms;  /**< Time when the item entered the local queue. */
    uint32_t route_epoch_seen; /**< Active route epoch recorded at enqueue. */
    uint32_t frame_local_time; /**< anyMSG local_time snapshot for heartbeat/tests. */
} rtos_priority_queue_item_t;

/**
 * @brief Queue depth snapshot by priority.
 */
typedef struct {
    uint32_t depth[RTOS_FIRMWARE_PRIORITY_COUNT]; /**< Per-priority item count. */
    uint32_t total_depth;                         /**< Total item count. */
    uint32_t capacity;                            /**< Fixed queue capacity. */
} rtos_priority_queue_depth_t;

/**
 * @brief Fixed-capacity P1 priority queue.
 */
typedef struct {
    rtos_priority_queue_item_t items[RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY];
    uint8_t quota[RTOS_FIRMWARE_PRIORITY_COUNT];
    uint8_t quota_used[RTOS_FIRMWARE_PRIORITY_COUNT];
    uint8_t scheduler_priority;
    uint32_t count;
} rtos_priority_queue_t;

/**
 * @brief Initialize a fixed-capacity priority queue with default quotas.
 */
void rtos_priority_queue_init(rtos_priority_queue_t *queue);

/**
 * @brief Return true when a priority value is accepted by P1.
 */
bool rtos_priority_queue_priority_is_valid(uint8_t priority);

/**
 * @brief Enqueue an item, evicting the oldest same-or-lower priority item if needed.
 *
 * @param queue Queue context.
 * @param item New item.
 * @param evicted_item Optional output for an item evicted to make room.
 * @param out_evicted Optional flag set to true when evicted_item is valid.
 * @return UNIFIED_OK on enqueue, UNIFIED_ERR_IPC_QUEUE_FULL when no item can be evicted.
 */
unified_error_t rtos_priority_queue_enqueue(rtos_priority_queue_t *queue,
                                            const rtos_priority_queue_item_t *item,
                                            rtos_priority_queue_item_t *evicted_item,
                                            bool *out_evicted);

/**
 * @brief Dequeue one item according to strict priority plus quota scheduling.
 */
unified_error_t rtos_priority_queue_dequeue(rtos_priority_queue_t *queue,
                                            rtos_priority_queue_item_t *out_item);

/**
 * @brief Read queue depth counters.
 */
void rtos_priority_queue_get_depth(const rtos_priority_queue_t *queue,
                                   rtos_priority_queue_depth_t *out_depth);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_PRIORITY_QUEUE_H */
