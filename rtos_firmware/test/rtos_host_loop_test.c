/**
 * @file rtos_host_loop_test.c
 * @brief P4 Linux/RTOS host 端到端闭环测试。
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

/**
 * @brief 测试时钟。
 */
typedef struct {
    uint32_t now_ms; /**< 当前模拟时间。 */
} test_clock_t;

/** @brief 测试共享内存 region。 */
static put_shm_region_t g_region;

/**
 * @brief 模拟时间源。
 *
 * @param user_context 测试时钟。
 * @return 当前时间。
 */
static uint32_t test_time_source(void *user_context)
{
    test_clock_t *clock; /**< 测试时钟。 */

    clock = (test_clock_t *)user_context;
    return (clock == 0) ? 0u : clock->now_ms;
}

/**
 * @brief 写入 little-endian 16 位字段。
 *
 * @param bytes 输出字节。
 * @param value 输入值。
 */
static void write_le16(uint8_t bytes[2], uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xFFu);
}

/**
 * @brief 写入 little-endian 32 位字段。
 *
 * @param bytes 输出字节。
 * @param value 输入值。
 */
static void write_le32(uint8_t bytes[4], uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xFFu);
    bytes[2] = (uint8_t)((value >> 16u) & 0xFFu);
    bytes[3] = (uint8_t)((value >> 24u) & 0xFFu);
}

/**
 * @brief 构造 CID。
 *
 * @param first CID 首字节。
 * @param second CID 第二字节。
 * @param out_cid 输出 CID。
 */
static void make_cid(uint8_t first,
                     uint8_t second,
                     uint8_t out_cid[ANYMSG_CID_LENGTH])
{
    (void)memset(out_cid, 0, ANYMSG_CID_LENGTH);
    out_cid[0] = first;
    out_cid[1] = second;
}

/**
 * @brief 写入一帧完整 anyMSG。
 *
 * @param buffer Frame Pool buffer。
 * @param payload_length payload 字节数。
 * @param source_cid source CID。
 * @param destination_cid destination CID。
 * @param type anyMSG type。
 * @param local_time local_time 字段。
 * @return 完整帧长度。
 */
static uint16_t write_anymsg(uint8_t *buffer,
                             uint16_t payload_length,
                             const uint8_t source_cid[ANYMSG_CID_LENGTH],
                             const uint8_t destination_cid[ANYMSG_CID_LENGTH],
                             uint8_t type,
                             uint32_t local_time)
{
    anymsg_header_t *header; /**< anyMSG header。 */
    uint16_t frame_length;   /**< 完整帧长度。 */
    uint16_t i;              /**< payload 下标。 */

    frame_length = (uint16_t)(ANYMSG_HEADER_SIZE + payload_length);
    (void)memset(buffer, 0, frame_length);
    header = (anymsg_header_t *)buffer;
    write_le16(header->msg_length, frame_length);
    write_le16(header->payload_length, payload_length);
    write_le32(header->local_time, local_time);
    (void)memcpy(header->source_cid, source_cid, ANYMSG_CID_LENGTH);
    (void)memcpy(header->destination_cid, destination_cid, ANYMSG_CID_LENGTH);
    header->type = type;

    for (i = 0u; i < payload_length; ++i) {
        buffer[ANYMSG_OFFSET_PAYLOAD + i] = (uint8_t)(0xA0u + i);
    }

    return frame_length;
}

/**
 * @brief 初始化 Linux 和 RTOS IPC。
 *
 * @param linux_ipc Linux IPC。
 * @param rtos_ipc RTOS IPC。
 * @param tasks task 编排上下文。
 * @param clock 测试时钟。
 * @return 0 表示成功。
 */
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

/**
 * @brief 分配并发布一帧 RX descriptor。
 *
 * @param linux_ipc Linux IPC。
 * @param source_interface 来源接口。
 * @param target_interface 原始目标接口。
 * @param source_cid source CID。
 * @param destination_cid destination CID。
 * @param type anyMSG type。
 * @param priority priority。
 * @param ttl ttl。
 * @param epoch Linux epoch。
 * @param local_time local_time。
 * @param payload_length payload 长度。
 * @param out_frame_id 输出 frame_id。
 * @return 0 表示成功。
 */
static int publish_frame(linux_shm_ipc_t *linux_ipc,
                         put_shm_interface_t source_interface,
                         put_shm_interface_t target_interface,
                         const uint8_t source_cid[ANYMSG_CID_LENGTH],
                         const uint8_t destination_cid[ANYMSG_CID_LENGTH],
                         uint8_t type,
                         uint8_t priority,
                         uint8_t ttl,
                         uint32_t epoch,
                         uint32_t local_time,
                         uint16_t payload_length,
                         uint32_t *out_frame_id)
{
    uint8_t *buffer;       /**< Frame Pool buffer。 */
    uint16_t capacity;     /**< Frame Pool block 容量。 */
    uint16_t frame_length; /**< 完整帧长度。 */

    CHECK(linux_shm_frame_alloc(linux_ipc,
                                source_interface,
                                out_frame_id,
                                &buffer,
                                &capacity) == UNIFIED_OK);
    CHECK(capacity == PUT_SHM_FRAME_POOL_BLOCK_SIZE);
    frame_length = write_anymsg(buffer,
                                payload_length,
                                source_cid,
                                destination_cid,
                                type,
                                local_time);
    CHECK(linux_shm_frame_commit_rx(linux_ipc,
                                    *out_frame_id,
                                    frame_length,
                                    source_interface,
                                    target_interface,
                                    source_cid,
                                    destination_cid,
                                    type,
                                    priority,
                                    ttl,
                                    epoch,
                                    0x66000000u | *out_frame_id) == UNIFIED_OK);
    return 0;
}

/**
 * @brief 检查 Frame Pool used。
 *
 * @param linux_ipc Linux IPC。
 * @param expected_used 期望 used。
 * @return 0 表示成功。
 */
static int expect_frame_pool_used(const linux_shm_ipc_t *linux_ipc,
                                  uint64_t expected_used)
{
    linux_shm_ipc_stats_t stats; /**< IPC 统计。 */

    linux_shm_ipc_get_stats(linux_ipc, &stats);
    CHECK(stats.frame_pool.used == expected_used);
    return 0;
}

/**
 * @brief Linux 从目标 TX ring 出队并释放 frame。
 *
 * @param linux_ipc Linux IPC。
 * @param target_interface 目标接口。
 * @param expected_frame_id 期望 frame_id。
 * @param expected_source_interface 期望来源接口。
 * @param expected_length 期望 frame 长度。
 * @param expected_used_after_release release 后期望 Frame Pool used。
 * @return 0 表示成功。
 */
static int dequeue_tx_and_release(linux_shm_ipc_t *linux_ipc,
                                  put_shm_interface_t target_interface,
                                  uint32_t expected_frame_id,
                                  put_shm_interface_t expected_source_interface,
                                  uint16_t expected_length,
                                  uint64_t expected_used_after_release)
{
    put_shm_descriptor_t descriptor; /**< TX descriptor。 */
    const uint8_t *frame;            /**< Frame Pool 只读指针。 */
    uint16_t frame_length;           /**< Frame 长度。 */

    frame = 0;
    frame_length = 0u;
    CHECK(linux_shm_dequeue_tx_descriptor(linux_ipc,
                                          target_interface,
                                          &descriptor,
                                          &frame,
                                          &frame_length) == UNIFIED_OK);
    CHECK(descriptor.frame_id == expected_frame_id);
    CHECK(descriptor.source_interface == (uint8_t)expected_source_interface);
    CHECK(descriptor.target_interface == (uint8_t)target_interface);
    CHECK(frame == g_region.frame_pool[expected_frame_id].bytes);
    CHECK(frame_length == expected_length);
    if (expected_length > ANYMSG_HEADER_SIZE) {
        CHECK(frame[ANYMSG_OFFSET_PAYLOAD] == 0xA0u);
    }
    CHECK(linux_shm_frame_release(linux_ipc,
                                  expected_frame_id,
                                  PUT_SHM_RECLAIM_REASON_NONE) == UNIFIED_OK);
    CHECK(expect_frame_pool_used(linux_ipc, expected_used_after_release) == 0);
    return 0;
}

/**
 * @brief Linux 从 reclaim ring 出队并校验释放。
 *
 * @param linux_ipc Linux IPC。
 * @param expected_frame_id 期望 frame_id。
 * @param expected_reason 期望 reclaim reason。
 * @return 0 表示成功。
 */
static int dequeue_reclaim_and_expect(linux_shm_ipc_t *linux_ipc,
                                      uint32_t expected_frame_id,
                                      put_shm_reclaim_reason_t expected_reason)
{
    put_shm_reclaim_descriptor_t reclaim; /**< reclaim descriptor。 */

    CHECK(linux_shm_dequeue_reclaim_descriptor(linux_ipc, &reclaim) == UNIFIED_OK);
    CHECK(reclaim.frame_id == expected_frame_id);
    CHECK(reclaim.reason == (uint32_t)expected_reason);
    CHECK(expect_frame_pool_used(linux_ipc, 0u) == 0);
    return 0;
}

/**
 * @brief drain 指定数量的 reclaim descriptor。
 *
 * @param linux_ipc Linux IPC。
 * @param expected_count 期望 drain 数量。
 * @return 0 表示成功。
 */
static int drain_reclaim_count(linux_shm_ipc_t *linux_ipc,
                               uint32_t expected_count)
{
    put_shm_reclaim_descriptor_t reclaim; /**< reclaim descriptor。 */
    uint32_t count;                       /**< drain 计数。 */

    for (count = 0u; count < expected_count; ++count) {
        CHECK(linux_shm_dequeue_reclaim_descriptor(linux_ipc, &reclaim) == UNIFIED_OK);
    }
    CHECK(linux_shm_dequeue_reclaim_descriptor(linux_ipc, &reclaim) ==
          UNIFIED_ERR_IPC_QUEUE_EMPTY);
    return 0;
}

/**
 * @brief 执行一帧成功 TX 闭环。
 *
 * @param linux_ipc Linux IPC。
 * @param tasks task 上下文。
 * @param source_interface 来源接口。
 * @param target_interface 目标接口。
 * @param source_first source CID 首字节。
 * @param destination_first destination CID 首字节。
 * @param sequence 序号。
 * @return 0 表示成功。
 */
static int exercise_tx_path(linux_shm_ipc_t *linux_ipc,
                            rtos_tasks_context_t *tasks,
                            put_shm_interface_t source_interface,
                            put_shm_interface_t target_interface,
                            uint8_t source_first,
                            uint8_t destination_first,
                            uint8_t sequence)
{
    uint8_t source_cid[ANYMSG_CID_LENGTH];      /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint32_t frame_id;                          /**< frame_id。 */
    uint16_t frame_length;                      /**< 完整 frame 长度。 */

    make_cid(source_first, sequence, source_cid);
    make_cid(destination_first, (uint8_t)(sequence + 1u), destination_cid);
    frame_length = (uint16_t)(ANYMSG_HEADER_SIZE + 4u);
    CHECK(publish_frame(linux_ipc,
                        source_interface,
                        target_interface,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        1000u + sequence,
                        4u,
                        &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(rtos_router_scheduler_task_run_once(tasks, 1u) == 1u);
    CHECK(dequeue_tx_and_release(linux_ipc,
                                 target_interface,
                                 frame_id,
                                 source_interface,
                                 frame_length,
                                 0u) == 0);
    return 0;
}

/**
 * @brief 验证成功路由后 Linux TX drain 和释放闭环。
 *
 * @return 0 表示通过。
 */
static int test_successful_tx_paths_close_frame_pool(void)
{
    linux_shm_ipc_t linux_ipc;  /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;    /**< RTOS IPC。 */
    rtos_tasks_context_t tasks; /**< task 上下文。 */
    test_clock_t clock;         /**< 测试时钟。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, &clock) == 0);
    CHECK(exercise_tx_path(&linux_ipc,
                           &tasks,
                           PUT_SHM_INTERFACE_CAN,
                           PUT_SHM_INTERFACE_RS485,
                           ANYMSG_CID_CAN_MIN,
                           ANYMSG_CID_RS485_MIN,
                           1u) == 0);
    CHECK(exercise_tx_path(&linux_ipc,
                           &tasks,
                           PUT_SHM_INTERFACE_RS485,
                           PUT_SHM_INTERFACE_CAN,
                           ANYMSG_CID_RS485_MIN,
                           ANYMSG_CID_CAN_MIN,
                           2u) == 0);
    CHECK(exercise_tx_path(&linux_ipc,
                           &tasks,
                           PUT_SHM_INTERFACE_ETHERNET,
                           PUT_SHM_INTERFACE_WIFI,
                           ANYMSG_CID_ETHERNET_MIN,
                           ANYMSG_CID_WIFI_MIN,
                           3u) == 0);
    CHECK(expect_frame_pool_used(&linux_ipc, 0u) == 0);
    (void)rtos_ipc;
    return 0;
}

/**
 * @brief 发布并期望一帧进入 reclaim 闭环。
 *
 * @param linux_ipc Linux IPC。
 * @param tasks task 上下文。
 * @param clock 测试时钟。
 * @param destination_first destination CID 首字节。
 * @param type anyMSG type。
 * @param ttl ttl。
 * @param epoch Linux epoch。
 * @param make_invalid 是否破坏 anyMSG 静态字段。
 * @param expected_reason 期望 reclaim reason。
 * @return 0 表示成功。
 */
static int expect_reclaim_path(linux_shm_ipc_t *linux_ipc,
                               rtos_tasks_context_t *tasks,
                               test_clock_t *clock,
                               uint8_t destination_first,
                               uint8_t type,
                               uint8_t ttl,
                               uint32_t epoch,
                               bool make_invalid,
                               put_shm_reclaim_reason_t expected_reason)
{
    uint8_t source_cid[ANYMSG_CID_LENGTH];      /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint32_t frame_id;                          /**< frame_id。 */

    make_cid(ANYMSG_CID_CAN_MIN, 7u, source_cid);
    make_cid(destination_first, 8u, destination_cid);
    CHECK(publish_frame(linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        type,
                        2u,
                        ttl,
                        epoch,
                        2000u,
                        0u,
                        &frame_id) == 0);
    if (make_invalid) {
        g_region.frame_pool[frame_id].bytes[ANYMSG_OFFSET_RESERVED] = 1u;
    }

    CHECK(rtos_ipc_event_task_run_once(tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    if (expected_reason == PUT_SHM_RECLAIM_REASON_TTL_EXPIRED) {
        clock->now_ms = clock->now_ms + 2u;
        CHECK(rtos_router_scheduler_task_run_once(tasks, 1u) == 1u);
    }
    CHECK(dequeue_reclaim_and_expect(linux_ipc, frame_id, expected_reason) == 0);
    return 0;
}

/**
 * @brief 验证心跳、无路由、TTL、epoch 和 invalid frame reclaim 闭环。
 *
 * @return 0 表示通过。
 */
static int test_reclaim_paths_close_frame_pool(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t gateway_cid[ANYMSG_CID_LENGTH]; /**< gateway CID。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, &clock) == 0);
    make_cid(ANYMSG_CID_RS485_MIN, 8u, gateway_cid);
    CHECK(rtos_endpoint_heartbeat_set_gateway(&tasks.router.endpoint_heartbeat,
                                              gateway_cid) == UNIFIED_OK);
    CHECK(expect_reclaim_path(&linux_ipc,
                              &tasks,
                              &clock,
                              ANYMSG_CID_RS485_MIN,
                              ANYMSG_TYPE_ENDPOINT_TO_GATEWAY_HEARTBEAT,
                              0u,
                              11u,
                              false,
                              PUT_SHM_RECLAIM_REASON_HEARTBEAT_CONSUMED) == 0);
    CHECK(expect_reclaim_path(&linux_ipc,
                              &tasks,
                              &clock,
                              ANYMSG_CID_RESERVED_HIGH_MIN,
                              ANYMSG_TYPE_RAW_CAN,
                              0u,
                              11u,
                              false,
                              PUT_SHM_RECLAIM_REASON_NO_ROUTE) == 0);
    CHECK(expect_reclaim_path(&linux_ipc,
                              &tasks,
                              &clock,
                              ANYMSG_CID_RS485_MIN,
                              ANYMSG_TYPE_RAW_CAN,
                              0u,
                              12u,
                              false,
                              PUT_SHM_RECLAIM_REASON_EPOCH_MISMATCH) == 0);
    CHECK(expect_reclaim_path(&linux_ipc,
                              &tasks,
                              &clock,
                              ANYMSG_CID_RS485_MIN,
                              ANYMSG_TYPE_RAW_CAN,
                              0u,
                              11u,
                              true,
                              PUT_SHM_RECLAIM_REASON_INVALID_FRAME) == 0);
    CHECK(expect_reclaim_path(&linux_ipc,
                              &tasks,
                              &clock,
                              ANYMSG_CID_RS485_MIN,
                              ANYMSG_TYPE_RAW_CAN,
                              1u,
                              11u,
                              false,
                              PUT_SHM_RECLAIM_REASON_TTL_EXPIRED) == 0);
    CHECK(expect_frame_pool_used(&linux_ipc, 0u) == 0);
    (void)rtos_ipc;
    return 0;
}

/**
 * @brief 构造 TX filler descriptor。
 *
 * @param frame_id frame_id。
 * @param source_cid source CID。
 * @param destination_cid destination CID。
 * @param out_descriptor 输出 descriptor。
 */
static void make_filler_descriptor(uint32_t frame_id,
                                   const uint8_t source_cid[ANYMSG_CID_LENGTH],
                                   const uint8_t destination_cid[ANYMSG_CID_LENGTH],
                                   put_shm_descriptor_t *out_descriptor)
{
    (void)memset(out_descriptor, 0, sizeof(*out_descriptor));
    out_descriptor->frame_id = frame_id;
    out_descriptor->frame_offset = frame_id * PUT_SHM_FRAME_POOL_BLOCK_SIZE;
    out_descriptor->frame_length = ANYMSG_HEADER_SIZE;
    out_descriptor->source_interface = (uint8_t)PUT_SHM_INTERFACE_CAN;
    out_descriptor->target_interface = (uint8_t)PUT_SHM_INTERFACE_RS485;
    (void)memcpy(out_descriptor->source_cid, source_cid, ANYMSG_CID_LENGTH);
    (void)memcpy(out_descriptor->destination_cid, destination_cid, ANYMSG_CID_LENGTH);
    out_descriptor->type = ANYMSG_TYPE_RAW_CAN;
    out_descriptor->priority = 2u;
    out_descriptor->epoch = 11u;
}

/**
 * @brief 验证 TX ring full 后 bounded retry 转 QUEUE_FULL reclaim。
 *
 * @return 0 表示通过。
 */
static int test_tx_ring_full_reclaims_and_linux_acks(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint32_t frame_id;                     /**< frame_id。 */
    uint32_t i;                            /**< 循环下标。 */
    put_shm_descriptor_t filler;           /**< TX ring 填充 descriptor。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, &clock) == 0);
    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 2u, destination_cid);
    for (i = 0u; i < PUT_SHM_DESCRIPTOR_RING_DEPTH; ++i) {
        make_filler_descriptor(20u + i, source_cid, destination_cid, &filler);
        CHECK(rtos_shm_ipc_enqueue_tx_descriptor(&rtos_ipc,
                                                 PUT_SHM_INTERFACE_RS485,
                                                 &filler) == UNIFIED_OK);
    }

    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        3000u,
                        0u,
                        &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(rtos_router_scheduler_task_run_once(&tasks, 1u) == 1u);
    CHECK(dequeue_reclaim_and_expect(&linux_ipc,
                                     frame_id,
                                     PUT_SHM_RECLAIM_REASON_QUEUE_FULL) == 0);
    return 0;
}

/**
 * @brief 验证 reclaim ring full 暂停 RX，并在 Linux drain 后补写恢复。
 *
 * @return 0 表示通过。
 */
static int test_reclaim_ring_full_blocks_and_recovers(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< source CID。 */
    uint8_t no_route_cid[ANYMSG_CID_LENGTH]; /**< no route CID。 */
    uint8_t routed_cid[ANYMSG_CID_LENGTH]; /**< route CID。 */
    uint32_t frame_id;                     /**< 当前 frame_id。 */
    uint32_t routed_frame_id;              /**< 阻塞期间发布的可路由 frame。 */
    uint32_t i;                            /**< 循环下标。 */
    uint32_t read_seq_before;              /**< 降级后 RX read_seq。 */
    put_shm_reclaim_descriptor_t reclaim;  /**< reclaim 输出。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, &clock) == 0);
    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RESERVED_HIGH_MIN, 2u, no_route_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 3u, routed_cid);

    for (i = 0u; i < PUT_SHM_RECLAIM_RING_DEPTH; ++i) {
        CHECK(publish_frame(&linux_ipc,
                            PUT_SHM_INTERFACE_CAN,
                            PUT_SHM_INTERFACE_RS485,
                            source_cid,
                            no_route_cid,
                            ANYMSG_TYPE_RAW_CAN,
                            2u,
                            0u,
                            11u,
                            4000u + i,
                            0u,
                            &frame_id) == 0);
        CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    }
    CHECK(g_region.reclaim_ring.producer.write_seq == PUT_SHM_RECLAIM_RING_DEPTH);

    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        no_route_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        5000u,
                        0u,
                        &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(rtos_tasks_get_state(&tasks) == RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL);
    CHECK(tasks.pending_reclaim_count == 1u);

    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        routed_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        6000u,
                        4u,
                        &routed_frame_id) == 0);
    read_seq_before = g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.read_seq;
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 0u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.read_seq == read_seq_before);

    CHECK(linux_shm_dequeue_reclaim_descriptor(&linux_ipc, &reclaim) == UNIFIED_OK);
    CHECK(rtos_router_scheduler_task_run_once(&tasks, 1u) == 1u);
    CHECK(tasks.pending_reclaim_count == 0u);
    CHECK(rtos_tasks_get_state(&tasks) == RTOS_TASKS_STATE_NORMAL);

    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(rtos_router_scheduler_task_run_once(&tasks, 1u) == 1u);
    CHECK(dequeue_tx_and_release(&linux_ipc,
                                 PUT_SHM_INTERFACE_RS485,
                                 routed_frame_id,
                                 PUT_SHM_INTERFACE_CAN,
                                 (uint16_t)(ANYMSG_HEADER_SIZE + 4u),
                                 PUT_SHM_RECLAIM_RING_DEPTH) == 0);

    CHECK(drain_reclaim_count(&linux_ipc, PUT_SHM_RECLAIM_RING_DEPTH) == 0);
    CHECK(expect_frame_pool_used(&linux_ipc, 0u) == 0);
    return 0;
}

/**
 * @brief 验证 Linux 拒绝未发布 frame 的 RTOS TX/reclaim 回写。
 *
 * @return 0 表示通过。
 */
static int test_unpublished_rtos_outputs_are_rejected(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    put_shm_descriptor_t descriptor;       /**< TX descriptor。 */
    put_shm_descriptor_t tx_output;        /**< Linux TX 输出。 */
    put_shm_reclaim_descriptor_t reclaim;  /**< Linux reclaim 输出。 */
    linux_shm_ipc_stats_t stats;           /**< IPC 统计。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, &clock) == 0);
    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 2u, destination_cid);
    make_filler_descriptor(7u, source_cid, destination_cid, &descriptor);
    CHECK(rtos_shm_ipc_enqueue_tx_descriptor(&rtos_ipc,
                                             PUT_SHM_INTERFACE_RS485,
                                             &descriptor) == UNIFIED_OK);
    CHECK(linux_shm_dequeue_tx_descriptor(&linux_ipc,
                                          PUT_SHM_INTERFACE_RS485,
                                          &tx_output,
                                          0,
                                          0) == UNIFIED_ERR_INVALID_ARG);

    CHECK(rtos_shm_ipc_reclaim_frame(&rtos_ipc,
                                     8u,
                                     PUT_SHM_RECLAIM_REASON_NO_ROUTE,
                                     PUT_SHM_INTERFACE_CAN,
                                     PUT_SHM_INTERFACE_RS485,
                                     11u,
                                     0u) == UNIFIED_OK);
    CHECK(linux_shm_dequeue_reclaim_descriptor(&linux_ipc, &reclaim) ==
          UNIFIED_ERR_INVALID_ARG);
    linux_shm_ipc_get_stats(&linux_ipc, &stats);
    CHECK(stats.descriptor_format_error_count == 2u);
    CHECK(expect_frame_pool_used(&linux_ipc, 0u) == 0);
    (void)tasks;
    return 0;
}

/**
 * @brief 验证长时间 host 闭环后 Frame Pool used 稳定回落。
 *
 * @return 0 表示通过。
 */
static int test_long_host_loop_frame_pool_stable(void)
{
    linux_shm_ipc_t linux_ipc;  /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;    /**< RTOS IPC。 */
    rtos_tasks_context_t tasks; /**< task 上下文。 */
    test_clock_t clock;         /**< 测试时钟。 */
    uint32_t i;                 /**< 循环下标。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, &clock) == 0);
    for (i = 0u; i < (4u * PUT_SHM_FRAME_POOL_BLOCK_COUNT); ++i) {
        clock.now_ms = 100u + i;
        CHECK(exercise_tx_path(&linux_ipc,
                               &tasks,
                               PUT_SHM_INTERFACE_CAN,
                               PUT_SHM_INTERFACE_RS485,
                               ANYMSG_CID_CAN_MIN,
                               ANYMSG_CID_RS485_MIN,
                               (uint8_t)(i & 0xFFu)) == 0);
    }
    CHECK(expect_frame_pool_used(&linux_ipc, 0u) == 0);
    (void)rtos_ipc;
    return 0;
}

/**
 * @brief P4 host 闭环测试入口。
 *
 * @return 0 表示全部测试通过。
 */
int main(void)
{
    CHECK(test_successful_tx_paths_close_frame_pool() == 0);
    CHECK(test_reclaim_paths_close_frame_pool() == 0);
    CHECK(test_tx_ring_full_reclaims_and_linux_acks() == 0);
    CHECK(test_reclaim_ring_full_blocks_and_recovers() == 0);
    CHECK(test_unpublished_rtos_outputs_are_rejected() == 0);
    CHECK(test_long_host_loop_frame_pool_stable() == 0);
    return 0;
}
