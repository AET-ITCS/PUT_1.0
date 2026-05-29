/**
 * @file rtos_recovery_test.c
 * @brief P3 Recovery host 单测。
 * @author Yukikaze
 */
#include "rtos_tasks.h"

#include <stdio.h>
#include <string.h>

#include "linux_shm_ipc.h"

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                          #condition);                                              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

typedef struct {
    uint32_t now_ms; /**< 当前测试时间。 */
} test_clock_t;

static put_shm_region_t g_region; /**< 测试共享内存 region。 */

static uint32_t test_time_source(void *user_context)
{
    test_clock_t *clock; /**< 测试时钟。 */

    clock = (test_clock_t *)user_context;
    return (clock == 0) ? 0u : clock->now_ms;
}

static void write_le16(uint8_t bytes[2], uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xFFu);
}

static void write_le32(uint8_t bytes[4], uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xFFu);
    bytes[2] = (uint8_t)((value >> 16u) & 0xFFu);
    bytes[3] = (uint8_t)((value >> 24u) & 0xFFu);
}

static void make_cid(uint8_t first,
                     uint8_t second,
                     uint8_t out_cid[ANYMSG_CID_LENGTH])
{
    (void)memset(out_cid, 0, ANYMSG_CID_LENGTH);
    out_cid[0] = first;
    out_cid[1] = second;
}

static uint16_t write_anymsg(uint8_t *buffer,
                             const uint8_t source_cid[ANYMSG_CID_LENGTH],
                             const uint8_t destination_cid[ANYMSG_CID_LENGTH])
{
    anymsg_header_t *header; /**< anyMSG header。 */

    (void)memset(buffer, 0, ANYMSG_HEADER_SIZE);
    header = (anymsg_header_t *)buffer;
    write_le16(header->msg_length, ANYMSG_HEADER_SIZE);
    write_le16(header->payload_length, 0u);
    write_le32(header->local_time, 1u);
    (void)memcpy(header->source_cid, source_cid, ANYMSG_CID_LENGTH);
    (void)memcpy(header->destination_cid, destination_cid, ANYMSG_CID_LENGTH);
    header->type = ANYMSG_TYPE_RAW_CAN;
    return ANYMSG_HEADER_SIZE;
}

static int setup_system(linux_shm_ipc_t *linux_ipc,
                        rtos_shm_ipc_t *rtos_ipc,
                        rtos_tasks_context_t *tasks,
                        test_clock_t *clock)
{
    linux_shm_ipc_init(linux_ipc);
    clock->now_ms = 100u;
    CHECK(linux_shm_ipc_format_region(linux_ipc,
                                      &g_region,
                                      11u,
                                      22u,
                                      0) == UNIFIED_OK);
    CHECK(rtos_shm_ipc_attach(rtos_ipc, &g_region, 0) == UNIFIED_OK);
    rtos_tasks_context_init(tasks, rtos_ipc, test_time_source, clock);
    return 0;
}

static int publish_frame(linux_shm_ipc_t *linux_ipc,
                         uint32_t *out_frame_id)
{
    uint8_t source_cid[ANYMSG_CID_LENGTH];      /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint8_t *buffer;                            /**< Frame Pool buffer。 */
    uint16_t capacity;                          /**< Frame Pool block 容量。 */
    uint16_t frame_length;                      /**< 完整帧长度。 */

    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 2u, destination_cid);
    CHECK(linux_shm_frame_alloc(linux_ipc,
                                PUT_SHM_INTERFACE_CAN,
                                out_frame_id,
                                &buffer,
                                &capacity) == UNIFIED_OK);
    CHECK(capacity == PUT_SHM_FRAME_POOL_BLOCK_SIZE);
    frame_length = write_anymsg(buffer, source_cid, destination_cid);
    CHECK(linux_shm_frame_commit_rx(linux_ipc,
                                    *out_frame_id,
                                    frame_length,
                                    PUT_SHM_INTERFACE_CAN,
                                    PUT_SHM_INTERFACE_RS485,
                                    source_cid,
                                    destination_cid,
                                    ANYMSG_TYPE_RAW_CAN,
                                    2u,
                                    0u,
                                    11u,
                                    0u) == UNIFIED_OK);
    return 0;
}

static void trigger_recovery(rtos_tasks_context_t *tasks)
{
    rtos_monitor_mark_recovery_pending(&tasks->monitor,
                                       RTOS_RECOVERY_TRIGGER_LINUX_HEARTBEAT);
    (void)rtos_heartbeat_task_run_once(tasks);
}

/**
 * @brief Recovery 丢弃 router 队列，不写 TX ring。
 *
 * @return 0 表示通过。
 */
static int test_recovery_discards_router_queue_without_tx(void)
{
    linux_shm_ipc_t linux_ipc;        /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;          /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;       /**< task 上下文。 */
    test_clock_t clock;               /**< 测试时钟。 */
    uint32_t frame_id;                /**< frame_id。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, &clock) == 0);
    CHECK(publish_frame(&linux_ipc, &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].producer.write_seq == 0u);

    trigger_recovery(&tasks);
    CHECK(rtos_tasks_get_state(&tasks) == RTOS_TASKS_STATE_RECOVERY);
    CHECK(rtos_recovery_task_run_once(&tasks, 1u) == 1u);
    CHECK(rtos_tasks_get_state(&tasks) == RTOS_TASKS_STATE_NORMAL);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].producer.write_seq == 0u);
    CHECK(g_region.reclaim_ring.producer.write_seq == 1u);
    CHECK(g_region.reclaim_ring.descriptors[0].frame_id == frame_id);
    CHECK(g_region.reclaim_ring.descriptors[0].reason ==
          (uint32_t)PUT_SHM_RECLAIM_REASON_QUEUE_FULL);
    return 0;
}

/**
 * @brief Recovery 在 reclaim ring 满时进入 blocked，drain 后恢复。
 *
 * @return 0 表示通过。
 */
static int test_recovery_reclaim_blocked_then_recovers(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint32_t frame_id;                     /**< frame_id。 */
    uint32_t i;                            /**< 循环下标。 */
    put_shm_reclaim_descriptor_t reclaim;  /**< Linux drain 输出。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, &clock) == 0);
    for (i = 0u; i < PUT_SHM_RECLAIM_RING_DEPTH; ++i) {
        CHECK(rtos_shm_ipc_reclaim_frame(&rtos_ipc,
                                         20u + i,
                                         PUT_SHM_RECLAIM_REASON_NO_ROUTE,
                                         PUT_SHM_INTERFACE_CAN,
                                         PUT_SHM_INTERFACE_RS485,
                                         11u,
                                         0u) == UNIFIED_OK);
    }
    CHECK(publish_frame(&linux_ipc, &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    trigger_recovery(&tasks);
    CHECK(rtos_recovery_task_run_once(&tasks, 1u) == 1u);
    CHECK(rtos_tasks_get_state(&tasks) == RTOS_TASKS_STATE_RECLAIM_BLOCKED);
    CHECK(tasks.pending_reclaim_count == 1u);

    (void)linux_shm_dequeue_reclaim_descriptor(&linux_ipc, &reclaim);
    CHECK(rtos_recovery_task_run_once(&tasks, 1u) == 1u);
    CHECK(tasks.pending_reclaim_count == 0u);
    CHECK(rtos_tasks_get_state(&tasks) == RTOS_TASKS_STATE_NORMAL);
    CHECK(g_region.reclaim_ring.producer.write_seq == (PUT_SHM_RECLAIM_RING_DEPTH + 1u));
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].producer.write_seq == 0u);
    (void)frame_id;
    return 0;
}

int main(void)
{
    CHECK(test_recovery_discards_router_queue_without_tx() == 0);
    CHECK(test_recovery_reclaim_blocked_then_recovers() == 0);
    return 0;
}
