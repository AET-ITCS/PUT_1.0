/**
 * @file rtos_priority_queue.h
 * @brief rtos_firmware P1 优先级队列接口。
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
 * @brief P1 本地优先级队列项。
 */
typedef struct {
    uint32_t frame_id;          /**< 可信 Frame Pool block 引用。 */
    put_shm_interface_t source_interface; /**< 原始 RX 接口。 */
    put_shm_interface_t target_interface; /**< 已解析出的 TX 接口。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< anyMSG 源 CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< anyMSG 目的 CID。 */
    uint8_t type;              /**< anyMSG type。 */
    uint8_t priority;          /**< 调度优先级，0 为最高。 */
    uint8_t ttl;               /**< P1 TTL 毫秒值，0 表示禁用过期检查。 */
    uint8_t retry_count;       /**< 调度过程中累计的 TX 重试次数。 */
    uint32_t epoch;            /**< 路由输入携带的 Linux epoch。 */
    uint32_t flags;            /**< 透传给 sink 的路由输入标志。 */
    uint32_t receive_time_ms;  /**< 路由输入对 P1 可见的时间。 */
    uint32_t enqueue_time_ms;  /**< 队列项进入本地队列的时间。 */
    uint32_t route_epoch_seen; /**< 入队时记录的 active route epoch。 */
    uint32_t frame_local_time; /**< 用于心跳/测试的 anyMSG local_time 快照。 */
} rtos_priority_queue_item_t;

/**
 * @brief 按优先级统计的队列深度快照。
 */
typedef struct {
    uint32_t depth[RTOS_FIRMWARE_PRIORITY_COUNT]; /**< 各优先级队列项数量。 */
    uint32_t total_depth;                         /**< 队列项总数。 */
    uint32_t capacity;                            /**< 固定队列容量。 */
} rtos_priority_queue_depth_t;

/**
 * @brief 固定容量 P1 优先级队列。
 */
typedef struct {
    rtos_priority_queue_item_t items[RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY];
    uint8_t quota[RTOS_FIRMWARE_PRIORITY_COUNT];
    uint8_t quota_used[RTOS_FIRMWARE_PRIORITY_COUNT];
    uint8_t scheduler_priority;
    uint32_t count;
} rtos_priority_queue_t;

void rtos_priority_queue_init(rtos_priority_queue_t *queue);

bool rtos_priority_queue_priority_is_valid(uint8_t priority);

unified_error_t rtos_priority_queue_enqueue(rtos_priority_queue_t *queue,
                                            const rtos_priority_queue_item_t *item,
                                            rtos_priority_queue_item_t *evicted_item,
                                            bool *out_evicted);

unified_error_t rtos_priority_queue_dequeue(rtos_priority_queue_t *queue,
                                            rtos_priority_queue_item_t *out_item);

void rtos_priority_queue_get_depth(const rtos_priority_queue_t *queue,
                                   rtos_priority_queue_depth_t *out_depth);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_PRIORITY_QUEUE_H */
