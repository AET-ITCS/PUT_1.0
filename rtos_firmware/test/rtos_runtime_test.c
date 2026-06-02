/**
 * @file rtos_runtime_test.c
 * @brief RTOS cooperative runtime host 闭环测试。
 * @author Yukikaze
 */
#include "rtos_runtime.h"

#include <stdbool.h>
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
 * @brief 固定测试时钟。
 */
typedef struct {
    uint32_t now_ms; /**< 当前模拟时间。 */
} fixed_clock_t;

/**
 * @brief 每次读取都会前进的测试时钟。
 */
typedef struct {
    uint32_t now_ms;  /**< 当前模拟时间。 */
    uint32_t step_ms; /**< 每次读取后前进的毫秒数。 */
} stepped_clock_t;

/**
 * @brief header cache invalidate mock。
 */
typedef struct {
    bool fail_next_header_invalidate; /**< 是否让下一次 header invalidate 失败。 */
    uint32_t header_invalidate_count; /**< header invalidate 调用次数。 */
} header_cache_mock_t;

/** @brief 测试共享内存 region。 */
static put_shm_region_t g_region;

/**
 * @brief 固定时间源。
 *
 * @param user_context fixed_clock_t 指针。
 * @return 当前模拟时间。
 */
static uint32_t fixed_time_source(void *user_context)
{
    fixed_clock_t *clock; /**< 固定测试时钟。 */

    clock = (fixed_clock_t *)user_context;
    return (clock == 0) ? 0u : clock->now_ms;
}

/**
 * @brief 递增时间源。
 *
 * @param user_context stepped_clock_t 指针。
 * @return 本次读取到的模拟时间。
 */
static uint32_t stepped_time_source(void *user_context)
{
    stepped_clock_t *clock; /**< 递增测试时钟。 */
    uint32_t now_ms;        /**< 本次返回时间。 */

    clock = (stepped_clock_t *)user_context;
    if (clock == 0) {
        return 0u;
    }

    now_ms = clock->now_ms;
    clock->now_ms = clock->now_ms + clock->step_ms;
    return now_ms;
}

/**
 * @brief mock cache flush。
 *
 * @param address 待 flush 地址。
 * @param length 待 flush 长度。
 * @param user_context mock 上下文，当前未使用。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t mock_cache_flush(const void *address,
                                        size_t length,
                                        void *user_context)
{
    (void)address;
    (void)length;
    (void)user_context;
    return UNIFIED_OK;
}

/**
 * @brief mock cache invalidate。
 *
 * @param address 待 invalidate 地址。
 * @param length 待 invalidate 长度。
 * @param user_context header_cache_mock_t 指针。
 * @return UNIFIED_OK 表示成功，否则返回注入错误。
 */
static unified_error_t mock_cache_invalidate(const void *address,
                                             size_t length,
                                             void *user_context)
{
    header_cache_mock_t *mock; /**< cache mock 上下文。 */

    mock = (header_cache_mock_t *)user_context;
    if ((mock != 0) &&
        (address == (const void *)&g_region.header) &&
        (length == sizeof(g_region.header))) {
        mock->header_invalidate_count = mock->header_invalidate_count + 1u;
        if (mock->fail_next_header_invalidate) {
            /* 只让下一次 header invalidate 失败，用于验证 runtime 阻断 RX drain。 */
            mock->fail_next_header_invalidate = false;
            return UNIFIED_ERR_IPC_NOT_READY;
        }
    }

    return UNIFIED_OK;
}

/**
 * @brief mock memory barrier。
 *
 * @param user_context mock 上下文，当前未使用。
 */
static void mock_memory_barrier(void *user_context)
{
    (void)user_context;
}

/**
 * @brief mock doorbell notify。
 *
 * @param direction 通知方向。
 * @param user_context mock 上下文，当前未使用。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t mock_notify(put_shm_direction_t direction, void *user_context)
{
    (void)direction;
    (void)user_context;
    return UNIFIED_OK;
}

/**
 * @brief mock 原子 OR。
 *
 * @param address 目标地址。
 * @param mask OR mask。
 * @param user_context mock 上下文，当前未使用。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t mock_atomic_or_u32(volatile uint32_t *address,
                                          uint32_t mask,
                                          void *user_context)
{
    (void)user_context;
    if (address == 0) {
        return UNIFIED_ERR_NULL;
    }
    *address = *address | mask;
    return UNIFIED_OK;
}

/**
 * @brief mock 原子 AND。
 *
 * @param address 目标地址。
 * @param mask AND mask。
 * @param user_context mock 上下文，当前未使用。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t mock_atomic_and_u32(volatile uint32_t *address,
                                           uint32_t mask,
                                           void *user_context)
{
    (void)user_context;
    if (address == 0) {
        return UNIFIED_ERR_NULL;
    }
    *address = *address & mask;
    return UNIFIED_OK;
}

/**
 * @brief mock 原子 ADD。
 *
 * @param address 目标地址。
 * @param value 累加值。
 * @param user_context mock 上下文，当前未使用。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t mock_atomic_add_u32(volatile uint32_t *address,
                                           uint32_t value,
                                           void *user_context)
{
    (void)user_context;
    if (address == 0) {
        return UNIFIED_ERR_NULL;
    }
    *address = *address + value;
    return UNIFIED_OK;
}

/**
 * @brief 填充测试平台操作集合。
 *
 * @param mock cache mock 上下文。
 * @param out_ops 输出平台操作集合。
 */
static void fill_mock_platform_ops(header_cache_mock_t *mock,
                                   rtos_shm_platform_ops_t *out_ops)
{
    (void)memset(out_ops, 0, sizeof(*out_ops));
    out_ops->cache_flush = mock_cache_flush;
    out_ops->cache_invalidate = mock_cache_invalidate;
    out_ops->memory_barrier = mock_memory_barrier;
    out_ops->notify = mock_notify;
    out_ops->atomic_or_u32 = mock_atomic_or_u32;
    out_ops->atomic_and_u32 = mock_atomic_and_u32;
    out_ops->atomic_add_u32 = mock_atomic_add_u32;
    out_ops->user_context = mock;
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
 * @brief 写入完整 anyMSG。
 *
 * @param buffer Frame Pool block。
 * @param payload_length payload 字节数。
 * @param source_cid source CID。
 * @param destination_cid destination CID。
 * @param type anyMSG type。
 * @param local_time local_time 字段。
 * @return 完整 anyMSG 长度。
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
    uint16_t index;          /**< payload 写入下标。 */

    frame_length = (uint16_t)(ANYMSG_HEADER_SIZE + payload_length);
    (void)memset(buffer, 0, frame_length);
    header = (anymsg_header_t *)buffer;
    write_le16(header->msg_length, frame_length);
    write_le16(header->payload_length, payload_length);
    write_le32(header->local_time, local_time);
    (void)memcpy(header->source_cid, source_cid, ANYMSG_CID_LENGTH);
    (void)memcpy(header->destination_cid, destination_cid, ANYMSG_CID_LENGTH);
    header->type = type;

    for (index = 0u; index < payload_length; ++index) {
        buffer[ANYMSG_OFFSET_PAYLOAD + index] = (uint8_t)(0xB0u + index);
    }

    return frame_length;
}

static int setup_runtime_with_ops(linux_shm_ipc_t *linux_ipc,
                                  rtos_runtime_context_t *runtime,
                                  rtos_router_time_source_t time_source,
                                  void *time_context,
                                  const rtos_shm_platform_ops_t *platform_ops);

/**
 * @brief 初始化 Linux IPC 和 RTOS runtime。
 *
 * @param linux_ipc Linux IPC 上下文。
 * @param runtime RTOS runtime 上下文。
 * @param time_source RTOS 时间源。
 * @param time_context 时间源上下文。
 * @return 0 表示成功。
 */
static int setup_runtime(linux_shm_ipc_t *linux_ipc,
                         rtos_runtime_context_t *runtime,
                         rtos_router_time_source_t time_source,
                         void *time_context)
{
    return setup_runtime_with_ops(linux_ipc, runtime, time_source, time_context, 0);
}

/**
 * @brief 使用指定平台 ops 初始化 Linux IPC 和 RTOS runtime。
 *
 * @param linux_ipc Linux IPC 上下文。
 * @param runtime RTOS runtime 上下文。
 * @param time_source RTOS 时间源。
 * @param time_context 时间源上下文。
 * @param platform_ops RTOS 平台操作集合，可为 NULL。
 * @return 0 表示成功。
 */
static int setup_runtime_with_ops(linux_shm_ipc_t *linux_ipc,
                                  rtos_runtime_context_t *runtime,
                                  rtos_router_time_source_t time_source,
                                  void *time_context,
                                  const rtos_shm_platform_ops_t *platform_ops)
{
    rtos_runtime_config_t config; /**< runtime 初始化配置。 */

    linux_shm_ipc_init(linux_ipc);
    CHECK(linux_shm_ipc_format_region(linux_ipc,
                                      &g_region,
                                      11u,
                                      22u,
                                      0) == UNIFIED_OK);

    (void)memset(&config, 0, sizeof(config));
    config.region = &g_region;
    config.platform_ops = platform_ops;
    config.time_source = time_source;
    config.time_context = time_context;
    config.scheduler_budget = 4u;
    config.recovery_budget = 4u;
    CHECK(rtos_runtime_init(runtime, &config) == UNIFIED_OK);
    CHECK(rtos_runtime_get_state(runtime) == RTOS_TASKS_STATE_NORMAL);
    return 0;
}

/**
 * @brief 发布一帧 RX descriptor。
 *
 * @param linux_ipc Linux IPC 上下文。
 * @param source_interface 来源接口。
 * @param target_interface 原始目标接口。
 * @param destination_first destination CID 首字节。
 * @param type anyMSG type。
 * @param priority descriptor priority。
 * @param ttl descriptor TTL。
 * @param epoch descriptor epoch。
 * @param payload_length payload 字节数。
 * @param out_frame_id 输出 frame_id。
 * @return 0 表示成功。
 */
static int publish_frame(linux_shm_ipc_t *linux_ipc,
                         put_shm_interface_t source_interface,
                         put_shm_interface_t target_interface,
                         uint8_t destination_first,
                         uint8_t type,
                         uint8_t priority,
                         uint8_t ttl,
                         uint32_t epoch,
                         uint16_t payload_length,
                         uint32_t *out_frame_id)
{
    uint8_t source_cid[ANYMSG_CID_LENGTH];      /**< source CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< destination CID。 */
    uint8_t *buffer;                            /**< Frame Pool block。 */
    uint16_t capacity;                          /**< Frame Pool block 容量。 */
    uint16_t frame_length;                      /**< 完整 anyMSG 长度。 */

    make_cid(ANYMSG_CID_WIFI_MIN, 1u, source_cid);
    make_cid(destination_first, 2u, destination_cid);
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
                                1000u + *out_frame_id);
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
                                    0x10000000u | *out_frame_id) == UNIFIED_OK);
    return 0;
}

/**
 * @brief 验证并释放 TX descriptor。
 *
 * @param linux_ipc Linux IPC 上下文。
 * @param target_interface 目标接口。
 * @param expected_frame_id 期望 frame_id。
 * @return 0 表示成功。
 */
static int expect_tx_and_release(linux_shm_ipc_t *linux_ipc,
                                 put_shm_interface_t target_interface,
                                 uint32_t expected_frame_id)
{
    put_shm_descriptor_t descriptor; /**< TX descriptor。 */
    const uint8_t *frame;            /**< 只读 frame 指针。 */
    uint16_t frame_length;           /**< frame 长度。 */

    frame = 0;
    frame_length = 0u;
    CHECK(linux_shm_dequeue_tx_descriptor(linux_ipc,
                                          target_interface,
                                          &descriptor,
                                          &frame,
                                          &frame_length) == UNIFIED_OK);
    CHECK(descriptor.frame_id == expected_frame_id);
    CHECK(descriptor.target_interface == (uint8_t)target_interface);
    CHECK(frame == g_region.frame_pool[expected_frame_id].bytes);
    CHECK(frame_length >= ANYMSG_HEADER_SIZE);
    CHECK(linux_shm_frame_release(linux_ipc,
                                  expected_frame_id,
                                  PUT_SHM_RECLAIM_REASON_NONE) == UNIFIED_OK);
    return 0;
}

/**
 * @brief 验证并处理 reclaim descriptor。
 *
 * @param linux_ipc Linux IPC 上下文。
 * @param expected_frame_id 期望 frame_id。
 * @param expected_reason 期望 reclaim reason。
 * @return 0 表示成功。
 */
static int expect_reclaim(linux_shm_ipc_t *linux_ipc,
                          uint32_t expected_frame_id,
                          put_shm_reclaim_reason_t expected_reason)
{
    put_shm_reclaim_descriptor_t reclaim; /**< reclaim descriptor。 */

    CHECK(linux_shm_dequeue_reclaim_descriptor(linux_ipc, &reclaim) == UNIFIED_OK);
    CHECK(reclaim.frame_id == expected_frame_id);
    CHECK(reclaim.reason == (uint32_t)expected_reason);
    return 0;
}

/**
 * @brief 验证 TX 和 reclaim ring 暂无输出。
 *
 * @param linux_ipc Linux IPC 上下文。
 * @param target_interface 待检查 TX ring 目标接口。
 * @return 0 表示成功。
 */
static int expect_no_tx_or_reclaim(linux_shm_ipc_t *linux_ipc,
                                   put_shm_interface_t target_interface)
{
    put_shm_descriptor_t tx_descriptor;        /**< 临时 TX descriptor。 */
    put_shm_reclaim_descriptor_t reclaim;      /**< 临时 reclaim descriptor。 */

    CHECK(linux_shm_dequeue_tx_descriptor(linux_ipc,
                                          target_interface,
                                          &tx_descriptor,
                                          0,
                                          0) == UNIFIED_ERR_IPC_QUEUE_EMPTY);
    CHECK(linux_shm_dequeue_reclaim_descriptor(linux_ipc, &reclaim) ==
          UNIFIED_ERR_IPC_QUEUE_EMPTY);
    return 0;
}

/**
 * @brief 验证 runtime 初始化状态。
 *
 * @return 0 表示通过。
 */
static int test_runtime_init_enters_normal(void)
{
    linux_shm_ipc_t linux_ipc;             /**< Linux IPC 上下文。 */
    rtos_runtime_context_t runtime;        /**< runtime 上下文。 */
    rtos_runtime_statistics_t statistics;  /**< runtime 统计。 */
    fixed_clock_t clock;                   /**< 固定测试时钟。 */

    clock.now_ms = 100u;
    CHECK(setup_runtime(&linux_ipc, &runtime, fixed_time_source, &clock) == 0);
    rtos_runtime_get_statistics(&runtime, &statistics);
    CHECK(statistics.initialized);
    CHECK(statistics.state == RTOS_TASKS_STATE_NORMAL);
    CHECK(statistics.run_count == 0u);
    return 0;
}

/**
 * @brief 验证 doorbell 触发可完成 RX 到 TX 的单步闭环。
 *
 * @return 0 表示通过。
 */
static int test_doorbell_run_closes_tx_path(void)
{
    linux_shm_ipc_t linux_ipc;      /**< Linux IPC 上下文。 */
    rtos_runtime_context_t runtime; /**< runtime 上下文。 */
    fixed_clock_t clock;            /**< 固定测试时钟。 */
    uint32_t frame_id;              /**< 本次测试 frame_id。 */

    clock.now_ms = 100u;
    CHECK(setup_runtime(&linux_ipc, &runtime, fixed_time_source, &clock) == 0);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_WIFI,
                        PUT_SHM_INTERFACE_CAN,
                        ANYMSG_CID_CAN_MIN,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        4u,
                        &frame_id) == 0);
    CHECK(rtos_runtime_run_once(&runtime, RTOS_RUNTIME_TRIGGER_DOORBELL) > 0u);
    CHECK(expect_tx_and_release(&linux_ipc, PUT_SHM_INTERFACE_CAN, frame_id) == 0);
    return 0;
}

/**
 * @brief 验证 pending bit 丢失时周期触发仍可兜底 drain。
 *
 * @return 0 表示通过。
 */
static int test_periodic_run_drains_when_pending_bit_lost(void)
{
    linux_shm_ipc_t linux_ipc;      /**< Linux IPC 上下文。 */
    rtos_runtime_context_t runtime; /**< runtime 上下文。 */
    fixed_clock_t clock;            /**< 固定测试时钟。 */
    uint32_t frame_id;              /**< 本次测试 frame_id。 */

    clock.now_ms = 100u;
    CHECK(setup_runtime(&linux_ipc, &runtime, fixed_time_source, &clock) == 0);
    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_ETHERNET,
                        PUT_SHM_INTERFACE_RS485,
                        ANYMSG_CID_RS485_MIN,
                        ANYMSG_TYPE_MODBUS_RTU,
                        2u,
                        0u,
                        11u,
                        4u,
                        &frame_id) == 0);
    g_region.rx_pending_bitmap.bits = 0u;
    CHECK(rtos_runtime_run_once(&runtime, RTOS_RUNTIME_TRIGGER_PERIODIC) > 0u);
    CHECK(expect_tx_and_release(&linux_ipc, PUT_SHM_INTERFACE_RS485, frame_id) == 0);
    return 0;
}

/**
 * @brief 验证指定 reclaim reason 的闭环。
 *
 * @param expected_reason 期望 reclaim reason。
 * @param destination_first destination CID 首字节。
 * @param epoch descriptor epoch。
 * @param make_invalid 是否破坏 anyMSG 静态字段。
 * @param use_stepped_clock 是否使用递增时钟触发 TTL。
 * @return 0 表示通过。
 */
static int exercise_reclaim_reason(put_shm_reclaim_reason_t expected_reason,
                                   uint8_t destination_first,
                                   uint32_t epoch,
                                   bool make_invalid,
                                   bool use_stepped_clock)
{
    linux_shm_ipc_t linux_ipc;      /**< Linux IPC 上下文。 */
    rtos_runtime_context_t runtime; /**< runtime 上下文。 */
    fixed_clock_t fixed_clock;      /**< 固定测试时钟。 */
    stepped_clock_t step_clock;     /**< 递增测试时钟。 */
    uint32_t frame_id;              /**< 本次测试 frame_id。 */
    uint8_t ttl;                    /**< descriptor TTL。 */

    fixed_clock.now_ms = 100u;
    step_clock.now_ms = 100u;
    step_clock.step_ms = 2u;
    if (use_stepped_clock) {
        CHECK(setup_runtime(&linux_ipc, &runtime, stepped_time_source, &step_clock) == 0);
        ttl = 1u;
    } else {
        CHECK(setup_runtime(&linux_ipc, &runtime, fixed_time_source, &fixed_clock) == 0);
        ttl = 0u;
    }

    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_WIFI,
                        PUT_SHM_INTERFACE_CAN,
                        destination_first,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        ttl,
                        epoch,
                        0u,
                        &frame_id) == 0);
    if (make_invalid) {
        /* 破坏保留字段，让 adapter 产出 invalid frame reclaim。 */
        g_region.frame_pool[frame_id].bytes[ANYMSG_OFFSET_RESERVED] = 1u;
    }

    CHECK(rtos_runtime_run_once(&runtime, RTOS_RUNTIME_TRIGGER_PERIODIC) > 0u);
    CHECK(expect_reclaim(&linux_ipc, frame_id, expected_reason) == 0);
    return 0;
}

/**
 * @brief 验证无路由、TTL、epoch 和非法 anyMSG reclaim。
 *
 * @return 0 表示通过。
 */
static int test_reclaim_paths_close_frame_pool(void)
{
    CHECK(exercise_reclaim_reason(PUT_SHM_RECLAIM_REASON_NO_ROUTE,
                                  ANYMSG_CID_RESERVED_HIGH_MIN,
                                  11u,
                                  false,
                                  false) == 0);
    CHECK(exercise_reclaim_reason(PUT_SHM_RECLAIM_REASON_EPOCH_MISMATCH,
                                  ANYMSG_CID_CAN_MIN,
                                  12u,
                                  false,
                                  false) == 0);
    CHECK(exercise_reclaim_reason(PUT_SHM_RECLAIM_REASON_INVALID_FRAME,
                                  ANYMSG_CID_CAN_MIN,
                                  11u,
                                  true,
                                  false) == 0);
    CHECK(exercise_reclaim_reason(PUT_SHM_RECLAIM_REASON_TTL_EXPIRED,
                                  ANYMSG_CID_CAN_MIN,
                                  11u,
                                  false,
                                  true) == 0);
    return 0;
}

/**
 * @brief drain 固定数量 reclaim descriptor。
 *
 * @param linux_ipc Linux IPC 上下文。
 * @param count drain 数量。
 * @return 0 表示成功。
 */
static int drain_reclaim_descriptors(linux_shm_ipc_t *linux_ipc, uint32_t count)
{
    put_shm_reclaim_descriptor_t reclaim; /**< reclaim descriptor。 */
    uint32_t index;                       /**< drain 下标。 */

    for (index = 0u; index < count; ++index) {
        CHECK(linux_shm_dequeue_reclaim_descriptor(linux_ipc, &reclaim) == UNIFIED_OK);
    }

    return 0;
}

/**
 * @brief 验证 reclaim ring 满后 runtime 能补写并恢复。
 *
 * @return 0 表示通过。
 */
static int test_reclaim_full_retries_after_linux_drains(void)
{
    linux_shm_ipc_t linux_ipc;      /**< Linux IPC 上下文。 */
    rtos_runtime_context_t runtime; /**< runtime 上下文。 */
    fixed_clock_t clock;            /**< 固定测试时钟。 */
    uint32_t frame_id;              /**< 当前 frame_id。 */
    uint32_t index;                 /**< 发布下标。 */

    clock.now_ms = 100u;
    CHECK(setup_runtime(&linux_ipc, &runtime, fixed_time_source, &clock) == 0);
    for (index = 0u; index < PUT_SHM_RECLAIM_RING_DEPTH; ++index) {
        CHECK(publish_frame(&linux_ipc,
                            PUT_SHM_INTERFACE_WIFI,
                            PUT_SHM_INTERFACE_CAN,
                            ANYMSG_CID_RESERVED_HIGH_MIN,
                            ANYMSG_TYPE_RAW_CAN,
                            2u,
                            0u,
                            11u,
                            0u,
                            &frame_id) == 0);
        CHECK(rtos_runtime_run_once(&runtime, RTOS_RUNTIME_TRIGGER_PERIODIC) > 0u);
        CHECK(rtos_runtime_get_state(&runtime) == RTOS_TASKS_STATE_NORMAL);
    }

    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_WIFI,
                        PUT_SHM_INTERFACE_CAN,
                        ANYMSG_CID_RESERVED_HIGH_MIN,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        11u,
                        0u,
                        &frame_id) == 0);
    CHECK(rtos_runtime_run_once(&runtime, RTOS_RUNTIME_TRIGGER_PERIODIC) > 0u);
    CHECK(rtos_runtime_get_state(&runtime) != RTOS_TASKS_STATE_NORMAL);

    CHECK(drain_reclaim_descriptors(&linux_ipc, PUT_SHM_RECLAIM_RING_DEPTH) == 0);
    CHECK(rtos_runtime_run_once(&runtime, RTOS_RUNTIME_TRIGGER_PERIODIC) > 0u);
    CHECK(rtos_runtime_get_state(&runtime) == RTOS_TASKS_STATE_NORMAL);
    CHECK(expect_reclaim(&linux_ipc, frame_id, PUT_SHM_RECLAIM_REASON_NO_ROUTE) == 0);
    return 0;
}

/**
 * @brief 验证 Linux epoch 改变会触发并完成 recovery。
 *
 * @return 0 表示通过。
 */
static int test_epoch_change_runs_recovery(void)
{
    linux_shm_ipc_t linux_ipc;      /**< Linux IPC 上下文。 */
    rtos_runtime_context_t runtime; /**< runtime 上下文。 */
    fixed_clock_t clock;            /**< 固定测试时钟。 */
    uint32_t frame_id;              /**< 本次测试 frame_id。 */

    clock.now_ms = 100u;
    CHECK(setup_runtime(&linux_ipc, &runtime, fixed_time_source, &clock) == 0);
    g_region.header.linux_epoch = 33u;
    CHECK(rtos_runtime_run_once(&runtime, RTOS_RUNTIME_TRIGGER_PERIODIC) > 0u);
    CHECK(rtos_runtime_get_state(&runtime) == RTOS_TASKS_STATE_NORMAL);

    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_WIFI,
                        PUT_SHM_INTERFACE_CAN,
                        ANYMSG_CID_CAN_MIN,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        33u,
                        4u,
                        &frame_id) == 0);
    CHECK(rtos_runtime_run_once(&runtime, RTOS_RUNTIME_TRIGGER_DOORBELL) > 0u);
    CHECK(expect_tx_and_release(&linux_ipc, PUT_SHM_INTERFACE_CAN, frame_id) == 0);
    return 0;
}

/**
 * @brief 验证 doorbell 路径会先处理 epoch recovery 再 drain RX。
 *
 * @return 0 表示通过。
 */
static int test_doorbell_epoch_change_recovers_before_rx_drain(void)
{
    linux_shm_ipc_t linux_ipc;      /**< Linux IPC 上下文。 */
    rtos_runtime_context_t runtime; /**< runtime 上下文。 */
    fixed_clock_t clock;            /**< 固定测试时钟。 */
    uint32_t frame_id;              /**< 本次测试 frame_id。 */

    clock.now_ms = 100u;
    CHECK(setup_runtime(&linux_ipc, &runtime, fixed_time_source, &clock) == 0);
    g_region.header.linux_epoch = 44u;

    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_WIFI,
                        PUT_SHM_INTERFACE_CAN,
                        ANYMSG_CID_CAN_MIN,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        44u,
                        4u,
                        &frame_id) == 0);
    CHECK(rtos_runtime_run_once(&runtime, RTOS_RUNTIME_TRIGGER_DOORBELL) > 0u);
    CHECK(rtos_runtime_get_state(&runtime) == RTOS_TASKS_STATE_NORMAL);
    CHECK(expect_tx_and_release(&linux_ipc, PUT_SHM_INTERFACE_CAN, frame_id) == 0);
    return 0;
}

/**
 * @brief 验证 header invalidate 失败时 doorbell 不会继续 drain RX。
 *
 * @return 0 表示通过。
 */
static int test_doorbell_preflight_cache_failure_blocks_rx_drain(void)
{
    linux_shm_ipc_t linux_ipc;          /**< Linux IPC 上下文。 */
    rtos_runtime_context_t runtime;     /**< runtime 上下文。 */
    fixed_clock_t clock;                /**< 固定测试时钟。 */
    header_cache_mock_t cache_mock;     /**< cache mock 上下文。 */
    rtos_shm_platform_ops_t mock_ops;   /**< mock 平台操作集合。 */
    uint32_t frame_id;                  /**< 本次测试 frame_id。 */

    clock.now_ms = 100u;
    (void)memset(&cache_mock, 0, sizeof(cache_mock));
    fill_mock_platform_ops(&cache_mock, &mock_ops);
    CHECK(setup_runtime_with_ops(&linux_ipc,
                                 &runtime,
                                 fixed_time_source,
                                 &clock,
                                 &mock_ops) == 0);
    g_region.header.linux_epoch = 55u;
    cache_mock.fail_next_header_invalidate = true;

    CHECK(publish_frame(&linux_ipc,
                        PUT_SHM_INTERFACE_WIFI,
                        PUT_SHM_INTERFACE_CAN,
                        ANYMSG_CID_CAN_MIN,
                        ANYMSG_TYPE_RAW_CAN,
                        2u,
                        0u,
                        55u,
                        4u,
                        &frame_id) == 0);
    CHECK(rtos_runtime_run_once(&runtime, RTOS_RUNTIME_TRIGGER_DOORBELL) == 0u);
    CHECK(cache_mock.header_invalidate_count == 1u);
    CHECK(expect_no_tx_or_reclaim(&linux_ipc, PUT_SHM_INTERFACE_CAN) == 0);

    CHECK(rtos_runtime_run_once(&runtime, RTOS_RUNTIME_TRIGGER_DOORBELL) > 0u);
    CHECK(cache_mock.header_invalidate_count >= 2u);
    CHECK(expect_tx_and_release(&linux_ipc, PUT_SHM_INTERFACE_CAN, frame_id) == 0);
    return 0;
}

/**
 * @brief runtime 测试入口。
 *
 * @return 0 表示全部测试通过。
 */
int main(void)
{
    CHECK(test_runtime_init_enters_normal() == 0);
    CHECK(test_doorbell_run_closes_tx_path() == 0);
    CHECK(test_periodic_run_drains_when_pending_bit_lost() == 0);
    CHECK(test_reclaim_paths_close_frame_pool() == 0);
    CHECK(test_reclaim_full_retries_after_linux_drains() == 0);
    CHECK(test_epoch_change_runs_recovery() == 0);
    CHECK(test_doorbell_epoch_change_recovers_before_rx_drain() == 0);
    CHECK(test_doorbell_preflight_cache_failure_blocks_rx_drain() == 0);
    return 0;
}
