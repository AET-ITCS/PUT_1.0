/**
 * @file rtos_ipc_adapter_test.c
 * @brief P2 Linux RX + RTOS drain/route host 闭环测试。
 * @author Yukikaze
 */
#include "rtos_tasks.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "crc16.h"
#include "linux_shm_ipc.h"

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                          #condition);                                              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

/** @brief 测试 descriptor 诊断高位，避开低 5 位 trust flags。 */
#define TEST_DESCRIPTOR_DIAGNOSTIC_FLAGS (0x55000000u)

/** @brief 测试默认授权 trust flags。 */
#define TEST_DESCRIPTOR_TRUST_FLAGS                                                \
    (PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK | PUT_SHM_DESCRIPTOR_FLAG_INTEGRITY_OK |       \
     PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK | PUT_SHM_DESCRIPTOR_FLAG_CONTROL_ALLOWED)

/**
 * @brief 测试时钟。
 */
typedef struct {
    uint32_t now_ms; /**< 当前模拟时间。 */
} test_clock_t;

/**
 * @brief Linux mock 平台上下文。
 */
typedef struct {
    bool fail_notify;    /**< 强制 doorbell 失败。 */
    bool fail_atomic_or; /**< 强制 pending OR 失败。 */
    uint32_t notify_count;
    uint32_t atomic_or_count;
} linux_mock_context_t;

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
    if (clock == 0) {
        return 0u;
    }

    return clock->now_ms;
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
 * @brief 计算 descriptor CRC。
 *
 * @param descriptor descriptor 指针。
 * @return CRC-16/CCITT-FALSE。
 */
static uint16_t test_descriptor_crc(const put_shm_descriptor_t *descriptor)
{
    return unified_crc16_ccitt_false((const uint8_t *)descriptor,
                                     offsetof(put_shm_descriptor_t, descriptor_crc16));
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
    uint16_t i;              /**< payload 填充下标。 */

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
 * @brief mock Linux notify。
 *
 * @param direction 通知方向。
 * @param user_context mock 上下文。
 * @return UNIFIED_OK 或注入的 notify 失败。
 */
static unified_error_t mock_linux_notify(put_shm_direction_t direction, void *user_context)
{
    linux_mock_context_t *context; /**< mock 上下文。 */

    (void)direction;
    context = (linux_mock_context_t *)user_context;
    if (context != 0) {
        context->notify_count = context->notify_count + 1u;
        if (context->fail_notify) {
            return UNIFIED_ERR_IPC_NOTIFY_FAILED;
        }
    }

    return UNIFIED_OK;
}

/**
 * @brief mock Linux pending OR。
 *
 * @param address 共享字段地址。
 * @param mask OR mask。
 * @param user_context mock 上下文。
 * @return UNIFIED_OK 或注入的 pending 失败。
 */
static unified_error_t mock_linux_atomic_or(volatile uint32_t *address,
                                            uint32_t mask,
                                            void *user_context)
{
    linux_mock_context_t *context; /**< mock 上下文。 */

    if (address == 0) {
        return UNIFIED_ERR_NULL;
    }

    context = (linux_mock_context_t *)user_context;
    if (context != 0) {
        context->atomic_or_count = context->atomic_or_count + 1u;
        if (context->fail_atomic_or) {
            return UNIFIED_ERR_IPC_NOTIFY_FAILED;
        }
    }

    *address = *address | mask;
    return UNIFIED_OK;
}

/**
 * @brief 构造 Linux mock ops。
 *
 * @param context mock 上下文。
 * @return 平台操作集合。
 */
static linux_shm_platform_ops_t make_linux_ops(linux_mock_context_t *context)
{
    linux_shm_platform_ops_t ops; /**< 平台操作集合。 */

    ops = *linux_shm_platform_default_ops();
    ops.notify = mock_linux_notify;
    ops.atomic_or_u32 = mock_linux_atomic_or;
    ops.user_context = context;
    return ops;
}

/**
 * @brief 初始化 Linux 和 RTOS IPC。
 *
 * @param linux_ipc Linux IPC。
 * @param rtos_ipc RTOS IPC。
 * @param tasks P2 task 上下文。
 * @param linux_ops Linux ops，可为 NULL。
 * @param clock 测试时钟。
 * @return 0 表示成功。
 */
static int setup_system(linux_shm_ipc_t *linux_ipc,
                        rtos_shm_ipc_t *rtos_ipc,
                        rtos_tasks_context_t *tasks,
                        const linux_shm_platform_ops_t *linux_ops,
                        test_clock_t *clock)
{
    linux_shm_ipc_init(linux_ipc);
    if (clock != 0) {
        clock->now_ms = 100u;
    }

    CHECK(linux_shm_ipc_format_region(linux_ipc,
                                      &g_region,
                                      11u,
                                      22u,
                                      linux_ops) == UNIFIED_OK);
    CHECK(rtos_shm_ipc_attach(rtos_ipc, &g_region, 0) == UNIFIED_OK);
    rtos_tasks_context_init(tasks, rtos_ipc, test_time_source, clock);
    return 0;
}

/**
 * @brief 分配并发布一帧 RX descriptor。
 *
 * @param linux_ipc Linux IPC。
 * @param source_interface 来源接口。
 * @param target_interface descriptor 初始目标接口。
 * @param source_cid source CID。
 * @param destination_cid destination CID。
 * @param type anyMSG type。
 * @param priority priority。
 * @param ttl ttl。
 * @param epoch Linux epoch。
 * @param local_time local_time。
 * @param payload_length payload 长度。
 * @param descriptor_flags descriptor flags。
 * @param out_frame_id 输出 frame_id。
 * @return 0 表示成功。
 */
static int publish_frame_with_flags(linux_shm_ipc_t *linux_ipc,
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
                                    uint32_t descriptor_flags,
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
                                    descriptor_flags) == UNIFIED_OK);
    return 0;
}

/**
 * @brief 分配并发布一帧已授权 RX descriptor。
 *
 * @param linux_ipc Linux IPC。
 * @param source_interface 来源接口。
 * @param target_interface descriptor 初始目标接口。
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
    uint32_t descriptor_flags; /**< 显式授权的 descriptor flags。 */

    descriptor_flags = TEST_DESCRIPTOR_DIAGNOSTIC_FLAGS | TEST_DESCRIPTOR_TRUST_FLAGS;
    return publish_frame_with_flags(linux_ipc,
                                    source_interface,
                                    target_interface,
                                    source_cid,
                                    destination_cid,
                                    type,
                                    priority,
                                    ttl,
                                    epoch,
                                    local_time,
                                    payload_length,
                                    descriptor_flags,
                                    out_frame_id);
}

/**
 * @brief 验证 Linux RX 到 RTOS TX ring 的成功路径。
 *
 * @return 0 表示通过。
 */
static int test_linux_rx_to_rtos_tx(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< P2 task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint32_t frame_id;                     /**< frame_id。 */
    put_shm_descriptor_t *tx_descriptor;   /**< TX descriptor。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 2u, destination_cid);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        77u,
                        3u,
                        &frame_id) == 0);

    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].producer.write_seq == 0u);
    CHECK(rtos_router_scheduler_task_run_once(&tasks, 1u) == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].producer.write_seq == 1u);
    CHECK(g_region.reclaim_ring.producer.write_seq == 0u);
    tx_descriptor = &g_region.tx_rings[PUT_SHM_INTERFACE_RS485].descriptors[0];
    CHECK(tx_descriptor->frame_id == frame_id);
    CHECK(tx_descriptor->target_interface == (uint8_t)PUT_SHM_INTERFACE_RS485);
    CHECK(g_region.frame_pool[frame_id].bytes[ANYMSG_OFFSET_PAYLOAD] == 0xA0u);
    return 0;
}

/**
 * @brief 验证心跳快速消费路径。
 *
 * @return 0 表示通过。
 */
static int test_heartbeat_reclaim(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< P2 task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< source CID。 */
    uint8_t gateway_cid[ANYMSG_CID_LENGTH]; /**< gateway CID。 */
    uint32_t frame_id;                     /**< frame_id。 */
    put_shm_reclaim_descriptor_t *reclaim; /**< reclaim descriptor。 */
    rtos_endpoint_heartbeat_snapshot_t heartbeat; /**< 心跳快照。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    make_cid(ANYMSG_CID_CAN_MIN, 3u, source_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 4u, gateway_cid);
    CHECK(rtos_endpoint_heartbeat_set_gateway(&tasks.router.endpoint_heartbeat,
                                              gateway_cid) == UNIFIED_OK);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        gateway_cid,
                        ANYMSG_TYPE_ENDPOINT_TO_GATEWAY_HEARTBEAT,
                        3u,
                        0u,
                        11u,
                        1234u,
                        0u,
                        &frame_id) == 0);

    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].producer.write_seq == 0u);
    CHECK(g_region.reclaim_ring.producer.write_seq == 1u);
    reclaim = &g_region.reclaim_ring.descriptors[0];
    CHECK(reclaim->frame_id == frame_id);
    CHECK(reclaim->reason == (uint32_t)PUT_SHM_RECLAIM_REASON_HEARTBEAT_CONSUMED);
    rtos_endpoint_heartbeat_get_snapshot(&tasks.router.endpoint_heartbeat, &heartbeat);
    CHECK(heartbeat.entry_count == 1u);
    CHECK(heartbeat.entries[0].last_frame_local_time == 1234u);
    return 0;
}

/**
 * @brief 验证 no route / invalid anyMSG / epoch / TTL reclaim。
 *
 * @return 0 表示通过。
 */
static int test_reclaim_reasons(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< P2 task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint32_t frame_id;                     /**< frame_id。 */

    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    make_cid(ANYMSG_CID_RESERVED_HIGH_MIN, 2u, destination_cid);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        1u,
                        0u,
                        &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(g_region.reclaim_ring.descriptors[0].reason ==
          (uint32_t)PUT_SHM_RECLAIM_REASON_NO_ROUTE);

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    make_cid(ANYMSG_CID_RS485_MIN, 2u, destination_cid);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        1u,
                        0u,
                        &frame_id) == 0);
    g_region.frame_pool[frame_id].bytes[ANYMSG_OFFSET_RESERVED] = 1u;
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(g_region.reclaim_ring.descriptors[0].reason ==
          (uint32_t)PUT_SHM_RECLAIM_REASON_INVALID_FRAME);

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        12u,
                        1u,
                        0u,
                        &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(g_region.reclaim_ring.descriptors[0].reason ==
          (uint32_t)PUT_SHM_RECLAIM_REASON_EPOCH_MISMATCH);

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        1u,
                        11u,
                        1u,
                        0u,
                        &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    clock.now_ms = 102u;
    CHECK(rtos_router_scheduler_task_run_once(&tasks, 1u) == 1u);
    CHECK(g_region.reclaim_ring.descriptors[0].reason ==
          (uint32_t)PUT_SHM_RECLAIM_REASON_TTL_EXPIRED);
    return 0;
}

/**
 * @brief 验证坏 descriptor 不进入路由核心且不写 reclaim。
 *
 * @return 0 表示通过。
 */
static int test_invalid_descriptor_no_reclaim(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< P2 task 上下文。 */
    rtos_tasks_statistics_t statistics;    /**< P2 统计。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint32_t frame_id;                     /**< frame_id。 */
    put_shm_descriptor_t *descriptor;      /**< RX descriptor。 */

    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 2u, destination_cid);

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        1u,
                        0u,
                        &frame_id) == 0);
    g_region.rx_rings[PUT_SHM_INTERFACE_CAN].descriptors[0].type ^= 0x01u;
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(g_region.reclaim_ring.producer.write_seq == 0u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.read_seq == 1u);
    rtos_tasks_get_statistics(&tasks, &statistics);
    CHECK(statistics.invalid_descriptor_no_reclaim_count == 1u);
    CHECK(statistics.route_submit_count == 0u);

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        1u,
                        0u,
                        &frame_id) == 0);
    descriptor = &g_region.rx_rings[PUT_SHM_INTERFACE_CAN].descriptors[0];
    descriptor->frame_length = (uint16_t)(PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u);
    descriptor->descriptor_crc16 = test_descriptor_crc(descriptor);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(g_region.reclaim_ring.producer.write_seq == 0u);
    rtos_tasks_get_statistics(&tasks, &statistics);
    CHECK(statistics.invalid_descriptor_no_reclaim_count == 1u);

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        1u,
                        0u,
                        &frame_id) == 0);
    descriptor = &g_region.rx_rings[PUT_SHM_INTERFACE_CAN].descriptors[0];
    descriptor->source_interface = (uint8_t)PUT_SHM_INTERFACE_RS485;
    descriptor->descriptor_crc16 = test_descriptor_crc(descriptor);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(g_region.reclaim_ring.producer.write_seq == 0u);
    rtos_tasks_get_statistics(&tasks, &statistics);
    CHECK(statistics.invalid_descriptor_no_reclaim_count == 1u);
    return 0;
}

/**
 * @brief 验证外部入口 trust flags 保护控制路径。
 *
 * @return 0 表示通过。
 */
static int test_external_trust_flags_gate_control_path(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< P2 task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< Wi-Fi source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< CAN destination CID。 */
    uint32_t frame_id;                     /**< frame_id。 */
    uint32_t auth_only_flags;              /**< 缺少 CONTROL_ALLOWED 的外部授权 flags。 */
    uint32_t full_control_flags;           /**< 完整控制授权 flags。 */
    rtos_router_statistics_t router_stats; /**< 路由统计。 */

    make_cid(ANYMSG_CID_WIFI_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_CAN_MIN, 2u, destination_cid);

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    CHECK(publish_frame_with_flags(&linux_ipc,
                                   PUT_SHM_INTERFACE_WIFI,
                                   PUT_SHM_INTERFACE_CAN,
                                   source_cid,
                                   destination_cid,
                                   ANYMSG_TYPE_RAW_CAN,
                                   2u,
                                   0u,
                                   11u,
                                   1u,
                                   0u,
                                   0u,
                                   &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(g_region.reclaim_ring.descriptors[0].reason ==
          (uint32_t)PUT_SHM_RECLAIM_REASON_INVALID_FRAME);
    rtos_router_get_statistics(&tasks.router, &router_stats);
    CHECK(router_stats.auth_failed_count == 1u);

    auth_only_flags = PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK |
                      PUT_SHM_DESCRIPTOR_FLAG_INTEGRITY_OK |
                      PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK;
    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    CHECK(publish_frame_with_flags(&linux_ipc,
                                   PUT_SHM_INTERFACE_WIFI,
                                   PUT_SHM_INTERFACE_CAN,
                                   source_cid,
                                   destination_cid,
                                   ANYMSG_TYPE_RAW_CAN,
                                   2u,
                                   0u,
                                   11u,
                                   1u,
                                   0u,
                                   auth_only_flags,
                                   &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(g_region.reclaim_ring.descriptors[0].reason ==
          (uint32_t)PUT_SHM_RECLAIM_REASON_INVALID_FRAME);
    rtos_router_get_statistics(&tasks.router, &router_stats);
    CHECK(router_stats.auth_failed_count == 1u);

    full_control_flags = auth_only_flags | PUT_SHM_DESCRIPTOR_FLAG_CONTROL_ALLOWED;
    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    CHECK(publish_frame_with_flags(&linux_ipc,
                                   PUT_SHM_INTERFACE_WIFI,
                                   PUT_SHM_INTERFACE_CAN,
                                   source_cid,
                                   destination_cid,
                                   ANYMSG_TYPE_RAW_CAN,
                                   2u,
                                   0u,
                                   11u,
                                   1u,
                                   0u,
                                   full_control_flags,
                                   &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(g_region.reclaim_ring.producer.write_seq == 0u);
    CHECK(rtos_router_scheduler_task_run_once(&tasks, 1u) == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_CAN].producer.write_seq == 1u);
    return 0;
}

/**
 * @brief 验证完整性和重放 trust flags 映射到独立统计。
 *
 * @return 0 表示通过。
 */
static int test_external_integrity_and_replay_flags_are_required(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< P2 task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< Wi-Fi source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< CAN destination CID。 */
    uint32_t frame_id;                     /**< frame_id。 */
    uint32_t missing_integrity_flags;      /**< 缺少 INTEGRITY_OK 的 flags。 */
    uint32_t missing_replay_flags;         /**< 缺少 REPLAY_OK 的 flags。 */
    rtos_router_statistics_t router_stats; /**< 路由统计。 */

    make_cid(ANYMSG_CID_WIFI_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_CAN_MIN, 2u, destination_cid);
    missing_integrity_flags = PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK |
                              PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK |
                              PUT_SHM_DESCRIPTOR_FLAG_CONTROL_ALLOWED;
    missing_replay_flags = PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK |
                           PUT_SHM_DESCRIPTOR_FLAG_INTEGRITY_OK |
                           PUT_SHM_DESCRIPTOR_FLAG_CONTROL_ALLOWED;

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    CHECK(publish_frame_with_flags(&linux_ipc,
                                   PUT_SHM_INTERFACE_WIFI,
                                   PUT_SHM_INTERFACE_CAN,
                                   source_cid,
                                   destination_cid,
                                   ANYMSG_TYPE_RAW_CAN,
                                   2u,
                                   0u,
                                   11u,
                                   1u,
                                   0u,
                                   missing_integrity_flags,
                                   &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    rtos_router_get_statistics(&tasks.router, &router_stats);
    CHECK(router_stats.integrity_failed_count == 1u);
    CHECK(g_region.reclaim_ring.descriptors[0].reason ==
          (uint32_t)PUT_SHM_RECLAIM_REASON_INVALID_FRAME);

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    CHECK(publish_frame_with_flags(&linux_ipc,
                                   PUT_SHM_INTERFACE_WIFI,
                                   PUT_SHM_INTERFACE_CAN,
                                   source_cid,
                                   destination_cid,
                                   ANYMSG_TYPE_RAW_CAN,
                                   2u,
                                   0u,
                                   11u,
                                   1u,
                                   0u,
                                   missing_replay_flags,
                                   &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    rtos_router_get_statistics(&tasks.router, &router_stats);
    CHECK(router_stats.replay_dropped_count == 1u);
    CHECK(g_region.reclaim_ring.descriptors[0].reason ==
          (uint32_t)PUT_SHM_RECLAIM_REASON_INVALID_FRAME);
    return 0;
}

/**
 * @brief 验证 pending/doorbell 失败后 periodic drain 兜底。
 *
 * @return 0 表示通过。
 */
static int test_periodic_drain_after_pending_failure(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< P2 task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    linux_mock_context_t linux_mock;       /**< Linux mock 上下文。 */
    linux_shm_platform_ops_t linux_ops;    /**< Linux mock ops。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint32_t frame_id;                     /**< frame_id。 */

    (void)memset(&linux_mock, 0, sizeof(linux_mock));
    linux_mock.fail_atomic_or = true;
    linux_ops = make_linux_ops(&linux_mock);
    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, &linux_ops, &clock) == 0);
    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 2u, destination_cid);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        1u,
                        0u,
                        &frame_id) == 0);
    CHECK(g_region.rx_pending_bitmap.bits == 0u);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_DOORBELL) == 0u);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(rtos_router_scheduler_task_run_once(&tasks, 1u) == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].producer.write_seq == 1u);
    return 0;
}

/**
 * @brief 验证真实 TX ring full 后 bounded retry 并转 reclaim。
 *
 * @return 0 表示通过。
 */
static int test_tx_ring_full_reclaims(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< P2 task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint32_t frame_id;                     /**< frame_id。 */
    uint32_t i;                            /**< 循环下标。 */
    put_shm_descriptor_t filler;           /**< TX ring 填充 descriptor。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 2u, destination_cid);
    for (i = 0u; i < PUT_SHM_DESCRIPTOR_RING_DEPTH; ++i) {
        (void)memset(&filler, 0, sizeof(filler));
        filler.frame_id = 20u + i;
        filler.frame_offset = filler.frame_id * PUT_SHM_FRAME_POOL_BLOCK_SIZE;
        filler.frame_length = ANYMSG_HEADER_SIZE;
        filler.source_interface = (uint8_t)PUT_SHM_INTERFACE_CAN;
        filler.target_interface = (uint8_t)PUT_SHM_INTERFACE_RS485;
        (void)memcpy(filler.source_cid, source_cid, ANYMSG_CID_LENGTH);
        (void)memcpy(filler.destination_cid, destination_cid, ANYMSG_CID_LENGTH);
        filler.type = ANYMSG_TYPE_RAW_CAN;
        filler.priority = 2u;
        filler.epoch = 11u;
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
                        1u,
                        0u,
                        &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(rtos_router_scheduler_task_run_once(&tasks, 1u) == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].producer.write_seq ==
          PUT_SHM_DESCRIPTOR_RING_DEPTH);
    CHECK(g_region.reclaim_ring.producer.write_seq == 1u);
    CHECK(g_region.reclaim_ring.descriptors[0].frame_id == frame_id);
    CHECK(g_region.reclaim_ring.descriptors[0].reason ==
          (uint32_t)PUT_SHM_RECLAIM_REASON_QUEUE_FULL);
    return 0;
}

/**
 * @brief 验证 reclaim full 降级、暂停 RX 和补写恢复。
 *
 * @return 0 表示通过。
 */
static int test_reclaim_full_blocks_and_recovers(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< P2 task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint32_t frame_id;                     /**< frame_id。 */
    uint32_t second_frame_id;              /**< 第二个 frame_id。 */
    uint32_t i;                            /**< 循环下标。 */
    put_shm_reclaim_descriptor_t reclaim;  /**< Linux drain 输出。 */
    uint32_t read_seq_before_blocked_event; /**< blocked event 前 read_seq。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    for (i = 0u; i < PUT_SHM_RECLAIM_RING_DEPTH; ++i) {
        CHECK(rtos_shm_ipc_reclaim_frame(&rtos_ipc,
                                         10u + i,
                                         PUT_SHM_RECLAIM_REASON_NO_ROUTE,
                                         PUT_SHM_INTERFACE_CAN,
                                         PUT_SHM_INTERFACE_RS485,
                                         11u,
                                         0u) == UNIFIED_OK);
    }
    CHECK(g_region.reclaim_ring.producer.write_seq == PUT_SHM_RECLAIM_RING_DEPTH);

    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RESERVED_HIGH_MIN, 2u, destination_cid);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        1u,
                        0u,
                        &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    CHECK(rtos_tasks_get_state(&tasks) == RTOS_TASKS_STATE_DEGRADED_RECLAIM_FULL);
    CHECK(tasks.pending_reclaim_count == 1u);

    make_cid(ANYMSG_CID_RS485_MIN, 3u, destination_cid);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        1u,
                        0u,
                        &second_frame_id) == 0);
    read_seq_before_blocked_event = g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.read_seq;
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 0u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.read_seq ==
          read_seq_before_blocked_event);

    (void)linux_shm_dequeue_reclaim_descriptor(&linux_ipc, &reclaim);
    CHECK(rtos_router_scheduler_task_run_once(&tasks, 1u) == 1u);
    CHECK(tasks.pending_reclaim_count == 0u);
    CHECK(rtos_tasks_get_state(&tasks) == RTOS_TASKS_STATE_NORMAL);
    CHECK(g_region.reclaim_ring.producer.write_seq == (PUT_SHM_RECLAIM_RING_DEPTH + 1u));
    return 0;
}

/**
 * @brief 验证 route epoch 变化后调度前重新查路由。
 *
 * @return 0 表示通过。
 */
static int test_route_epoch_refresh_before_tx(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< P2 task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint32_t frame_id;                     /**< frame_id。 */
    rtos_route_table_snapshot_t table;     /**< 路由表快照。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 2u, destination_cid);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        1u,
                        0u,
                        &frame_id) == 0);
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 1u);
    rtos_router_get_route_table(&tasks.router, &table);
    table.active_route_epoch = table.active_route_epoch + 1u;
    table.cid_segment_targets[ANYMSG_CID_SEGMENT_RS485] = PUT_SHM_INTERFACE_WIFI;
    CHECK(rtos_router_set_route_table(&tasks.router, &table) == UNIFIED_OK);
    CHECK(rtos_router_scheduler_task_run_once(&tasks, 1u) == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_WIFI].producer.write_seq == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].producer.write_seq == 0u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_WIFI].descriptors[0].frame_id == frame_id);
    return 0;
}

/**
 * @brief 验证全局降级状态阻止新的 RX drain。
 *
 * @return 0 表示通过。
 */
static int test_global_degraded_blocks_rx_drain(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;               /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;            /**< P3 task 上下文。 */
    test_clock_t clock;                    /**< 测试时钟。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint32_t frame_id;                     /**< frame_id。 */
    uint32_t read_seq_before;              /**< 降级前 RX read_seq。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    rtos_tasks_observe_linux_heartbeat(&tasks, 1u);
    clock.now_ms = 1100u;
    CHECK(rtos_heartbeat_task_run_once(&tasks) > 0u);
    CHECK(rtos_tasks_get_state(&tasks) == RTOS_TASKS_STATE_DEGRADED);

    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 2u, destination_cid);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_CAN,
                        PUT_SHM_INTERFACE_RS485,
                        source_cid,
                        destination_cid,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        1u,
                        0u,
                        &frame_id) == 0);
    read_seq_before = g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.read_seq;
    CHECK(rtos_ipc_event_task_run_once(&tasks, RTOS_IPC_EVENT_TRIGGER_PERIODIC) == 0u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.read_seq == read_seq_before);
    return 0;
}

/**
 * @brief 验证 linux_epoch 变化触发 Recovery。
 *
 * @return 0 表示通过。
 */
static int test_linux_epoch_change_triggers_recovery(void)
{
    linux_shm_ipc_t linux_ipc;          /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;            /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;         /**< P3 task 上下文。 */
    test_clock_t clock;                 /**< 测试时钟。 */
    rtos_recovery_snapshot_t recovery;  /**< recovery 快照。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    g_region.header.linux_epoch = 12u;
    CHECK(rtos_error_monitor_task_run_once(&tasks) > 0u);
    CHECK(rtos_tasks_get_state(&tasks) == RTOS_TASKS_STATE_RECOVERY);
    rtos_monitor_get_recovery_snapshot(&tasks.monitor, &recovery);
    CHECK(recovery.recovery_pending);
    CHECK((recovery.trigger_bits & RTOS_RECOVERY_TRIGGER_LINUX_EPOCH) != 0u);
    return 0;
}

/**
 * @brief 验证 magic/version 异常进入 DEGRADED。
 *
 * @return 0 表示通过。
 */
static int test_magic_version_error_degrades(void)
{
    linux_shm_ipc_t linux_ipc;          /**< Linux IPC。 */
    rtos_shm_ipc_t rtos_ipc;            /**< RTOS IPC。 */
    rtos_tasks_context_t tasks;         /**< P3 task 上下文。 */
    test_clock_t clock;                 /**< 测试时钟。 */
    rtos_error_state_snapshot_t error;  /**< 错误快照。 */

    CHECK(setup_system(&linux_ipc, &rtos_ipc, &tasks, 0, &clock) == 0);
    g_region.header.magic = 0u;
    CHECK(rtos_error_monitor_task_run_once(&tasks) == 1u);
    CHECK(rtos_tasks_get_state(&tasks) == RTOS_TASKS_STATE_DEGRADED);
    rtos_monitor_get_error_state_snapshot(&tasks.monitor, &error);
    CHECK((error.error_bits & RTOS_MONITOR_ERROR_SHM_FORMAT) != 0u);
    return 0;
}

/**
 * @brief P2 host 闭环测试入口。
 *
 * @return 0 表示全部测试通过。
 */
int main(void)
{
    CHECK(test_linux_rx_to_rtos_tx() == 0);
    CHECK(test_heartbeat_reclaim() == 0);
    CHECK(test_reclaim_reasons() == 0);
    CHECK(test_invalid_descriptor_no_reclaim() == 0);
    CHECK(test_external_trust_flags_gate_control_path() == 0);
    CHECK(test_external_integrity_and_replay_flags_are_required() == 0);
    CHECK(test_periodic_drain_after_pending_failure() == 0);
    CHECK(test_tx_ring_full_reclaims() == 0);
    CHECK(test_reclaim_full_blocks_and_recovers() == 0);
    CHECK(test_route_epoch_refresh_before_tx() == 0);
    CHECK(test_global_degraded_blocks_rx_drain() == 0);
    CHECK(test_linux_epoch_change_triggers_recovery() == 0);
    CHECK(test_magic_version_error_degrades() == 0);
    return 0;
}
