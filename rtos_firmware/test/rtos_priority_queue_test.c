/**
 * @file rtos_priority_queue_test.c
 * @brief P1 优先级队列主机端测试。
 * @author Yukikaze
 */
#include "rtos_priority_queue.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__,   \
                          #condition);                                                \
            return 1;                                                                 \
        }                                                                             \
    } while (0)

/**
 * @brief 构造一个最小队列项。
 *
 * @param frame_id 帧 ID。
 * @param priority 优先级。
 * @param enqueue_time_ms 入队时间。
 * @return 构造完成的队列项。
 */
static rtos_priority_queue_item_t make_item(uint32_t frame_id,
                                            uint8_t priority,
                                            uint32_t enqueue_time_ms)
{
    rtos_priority_queue_item_t item; /**< 正在构造的队列项。 */

    (void)memset(&item, 0, sizeof(item));
    item.frame_id = frame_id;
    item.priority = priority;
    item.enqueue_time_ms = enqueue_time_ms;
    item.route_epoch_seen = 1u;
    item.source_interface = PUT_SHM_INTERFACE_CAN;
    item.target_interface = PUT_SHM_INTERFACE_RS485;
    return item;
}

/**
 * @brief 验证严格优先级和配额防饥饿调度。
 *
 * @return 0 表示测试通过，非 0 表示失败。
 */
static int test_quota_dequeue_order(void)
{
    rtos_priority_queue_t queue;          /**< 被测队列。 */
    rtos_priority_queue_item_t item;      /**< 入队或出队队列项。 */
    uint32_t i;                           /**< 循环下标。 */

    rtos_priority_queue_init(&queue);
    for (i = 0u; i < 17u; ++i) {
        item = make_item(i, 0u, i);
        CHECK(rtos_priority_queue_enqueue(&queue, &item, 0, 0) == UNIFIED_OK);
    }
    item = make_item(100u, 1u, 100u);
    CHECK(rtos_priority_queue_enqueue(&queue, &item, 0, 0) == UNIFIED_OK);
    item = make_item(200u, 2u, 200u);
    CHECK(rtos_priority_queue_enqueue(&queue, &item, 0, 0) == UNIFIED_OK);
    item = make_item(300u, 3u, 300u);
    CHECK(rtos_priority_queue_enqueue(&queue, &item, 0, 0) == UNIFIED_OK);

    for (i = 0u; i < RTOS_FIRMWARE_PRIORITY_0_QUOTA; ++i) {
        CHECK(rtos_priority_queue_dequeue(&queue, &item) == UNIFIED_OK);
        CHECK(item.priority == 0u);
    }

    CHECK(rtos_priority_queue_dequeue(&queue, &item) == UNIFIED_OK);
    CHECK(item.priority == 1u);
    CHECK(rtos_priority_queue_dequeue(&queue, &item) == UNIFIED_OK);
    CHECK(item.priority == 2u);
    CHECK(rtos_priority_queue_dequeue(&queue, &item) == UNIFIED_OK);
    CHECK(item.priority == 3u);
    CHECK(rtos_priority_queue_dequeue(&queue, &item) == UNIFIED_OK);
    CHECK(item.priority == 0u);
    CHECK(item.frame_id == 16u);

    return 0;
}

/**
 * @brief 验证低优先级驱逐和 priority 0 保护策略。
 *
 * @return 0 表示测试通过，非 0 表示失败。
 */
static int test_eviction_policy(void)
{
    rtos_priority_queue_t queue;             /**< 被测队列。 */
    rtos_priority_queue_item_t item;         /**< 新队列项。 */
    rtos_priority_queue_item_t evicted_item; /**< 被驱逐队列项。 */
    rtos_priority_queue_depth_t depth;       /**< 队列深度快照。 */
    bool evicted;                            /**< 驱逐标记。 */
    uint32_t i;                              /**< 循环下标。 */

    rtos_priority_queue_init(&queue);
    for (i = 0u; i < RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY; ++i) {
        item = make_item(i, 3u, i);
        CHECK(rtos_priority_queue_enqueue(&queue, &item, 0, 0) == UNIFIED_OK);
    }

    item = make_item(1000u, 2u, 1000u);
    CHECK(rtos_priority_queue_enqueue(&queue, &item, &evicted_item, &evicted) ==
          UNIFIED_OK);
    CHECK(evicted);
    CHECK(evicted_item.priority == 3u);
    CHECK(evicted_item.frame_id == 0u);
    rtos_priority_queue_get_depth(&queue, &depth);
    CHECK(depth.depth[3] == (RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY - 1u));
    CHECK(depth.depth[2] == 1u);
    CHECK(depth.total_depth == RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY);

    rtos_priority_queue_init(&queue);
    for (i = 0u; i < RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY; ++i) {
        item = make_item(i, 0u, i);
        CHECK(rtos_priority_queue_enqueue(&queue, &item, 0, 0) == UNIFIED_OK);
    }

    item = make_item(2000u, 0u, 2000u);
    evicted = true;
    CHECK(rtos_priority_queue_enqueue(&queue, &item, &evicted_item, &evicted) ==
          UNIFIED_ERR_IPC_QUEUE_FULL);
    CHECK(!evicted);
    rtos_priority_queue_get_depth(&queue, &depth);
    CHECK(depth.depth[0] == RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY);

    return 0;
}

/**
 * @brief 验证非法 priority 处理。
 *
 * @return 0 表示测试通过，非 0 表示失败。
 */
static int test_invalid_priority(void)
{
    rtos_priority_queue_t queue;     /**< 被测队列。 */
    rtos_priority_queue_item_t item; /**< 非法队列项。 */

    rtos_priority_queue_init(&queue);
    item = make_item(1u, 4u, 1u);
    CHECK(!rtos_priority_queue_priority_is_valid(item.priority));
    CHECK(rtos_priority_queue_enqueue(&queue, &item, 0, 0) == UNIFIED_ERR_INVALID_ARG);

    return 0;
}

/**
 * @brief P1 优先级队列主机端测试入口。
 *
 * @return 0 表示全部测试通过，非 0 表示失败。
 */
int main(void)
{
    CHECK(test_quota_dequeue_order() == 0);
    CHECK(test_eviction_policy() == 0);
    CHECK(test_invalid_priority() == 0);

    return 0;
}
