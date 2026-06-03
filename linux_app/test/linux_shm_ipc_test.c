/**
 * @file linux_shm_ipc_test.c
 * @brief Linux 侧共享内存 IPC v2 host 单元测试。
 * @author Yukikaze
 */
#include "linux_shm_ipc.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "crc16.h"

/** @brief 测试断言宏。 */
#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                          #condition);                                              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

/**
 * @brief mock 平台上下文。
 */
typedef struct {
    uint32_t flush_count;              /**< cache flush 调用次数。 */
    uint32_t invalidate_count;         /**< cache invalidate 调用次数。 */
    uint32_t barrier_count;            /**< memory barrier 调用次数。 */
    uint32_t notify_count;             /**< notify 调用次数。 */
    uint32_t atomic_or_count;          /**< 原子 OR 调用次数。 */
    uint32_t atomic_and_count;         /**< 原子 AND 调用次数。 */
    uint32_t atomic_add_count;         /**< 原子 ADD 调用次数。 */
    bool fail_notify;                  /**< 是否强制 notify 失败。 */
    uint32_t fail_flush_on_call;       /**< 指定第几次 cache flush 失败，0 表示不失败。 */
    bool fail_atomic_or;               /**< 是否强制 pending 原子 OR 失败。 */
    bool inject_rs485_set_during_can_clear; /**< 是否在清 CAN bit 时并发设置 RS485 bit。 */
    bool injected_rs485_set_during_can_clear; /**< 是否已完成 RS485 bit 注入。 */
    bool inject_tx_enqueue_during_pending_clear; /**< 是否在 clear 前注入同接口 TX descriptor。 */
    bool injected_tx_enqueue_during_pending_clear; /**< 是否已完成 clear 前 TX 注入。 */
    put_shm_descriptor_ring_t *inject_ring; /**< 注入 TX descriptor 的 ring。 */
    put_shm_pending_line_t *inject_pending; /**< 注入 pending 的控制行。 */
    put_shm_descriptor_t inject_descriptor; /**< 注入 descriptor。 */
} mock_context_t;

/** @brief 测试共享内存 region。 */
static put_shm_region_t g_region;

/**
 * @brief 计算 descriptor CRC。
 *
 * @param descriptor descriptor 指针。
 * @return CRC-16/CCITT-FALSE 结果。
 */
static uint16_t test_descriptor_crc(const put_shm_descriptor_t *descriptor)
{
    return unified_crc16_ccitt_false((const uint8_t *)descriptor,
                                     offsetof(put_shm_descriptor_t, descriptor_crc16));
}

/**
 * @brief 计算 reclaim descriptor CRC。
 *
 * @param descriptor reclaim descriptor 指针。
 * @return CRC-16/CCITT-FALSE 结果。
 */
static uint16_t test_reclaim_crc(const put_shm_reclaim_descriptor_t *descriptor)
{
    return unified_crc16_ccitt_false((const uint8_t *)descriptor,
                                     offsetof(put_shm_reclaim_descriptor_t, descriptor_crc16));
}

/**
 * @brief mock cache flush。
 *
 * @param address 待 flush 地址。
 * @param length 待 flush 长度。
 * @param user_context mock 上下文。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t mock_cache_flush(const void *address, size_t length, void *user_context)
{
    mock_context_t *context; /**< mock 上下文。 */

    (void)address;
    (void)length;
    context = (mock_context_t *)user_context;
    if (context != 0) {
        /* 记录 flush 调用次数。 */
        context->flush_count++;
        if ((context->fail_flush_on_call != 0u) &&
            (context->flush_count == context->fail_flush_on_call)) {
            /* 按测试配置注入 cache flush 失败。 */
            return UNIFIED_ERR_IPC_NOT_READY;
        }
    }
    return UNIFIED_OK;
}

/**
 * @brief mock cache invalidate。
 *
 * @param address 待 invalidate 地址。
 * @param length 待 invalidate 长度。
 * @param user_context mock 上下文。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t mock_cache_invalidate(const void *address, size_t length, void *user_context)
{
    mock_context_t *context; /**< mock 上下文。 */

    (void)address;
    (void)length;
    context = (mock_context_t *)user_context;
    if (context != 0) {
        /* 记录 invalidate 调用次数。 */
        context->invalidate_count++;
    }
    return UNIFIED_OK;
}

/**
 * @brief mock memory barrier。
 *
 * @param user_context mock 上下文。
 */
static void mock_memory_barrier(void *user_context)
{
    mock_context_t *context; /**< mock 上下文。 */

    context = (mock_context_t *)user_context;
    if (context != 0) {
        /* 记录 barrier 调用次数。 */
        context->barrier_count++;
    }
}

/**
 * @brief mock notify。
 *
 * @param direction 通知方向。
 * @param user_context mock 上下文。
 * @return UNIFIED_OK 表示成功，否则返回通知失败。
 */
static unified_error_t mock_notify(put_shm_direction_t direction, void *user_context)
{
    mock_context_t *context; /**< mock 上下文。 */

    (void)direction;
    context = (mock_context_t *)user_context;
    if (context != 0) {
        /* 记录 notify 调用次数。 */
        context->notify_count++;
        if (context->fail_notify) {
            return UNIFIED_ERR_IPC_NOTIFY_FAILED;
        }
    }
    return UNIFIED_OK;
}

/**
 * @brief mock 原子 OR。
 *
 * @param address 待更新地址。
 * @param mask OR mask。
 * @param user_context mock 上下文。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t mock_atomic_or_u32(volatile uint32_t *address,
                                          uint32_t mask,
                                          void *user_context)
{
    mock_context_t *context; /**< mock 上下文。 */

    if (address == 0) {
        return UNIFIED_ERR_NULL;
    }

    context = (mock_context_t *)user_context;
    if (context != 0) {
        /* 记录 OR 次数。 */
        context->atomic_or_count++;
        if (context->fail_atomic_or) {
            /* 按测试配置注入 pending OR 失败。 */
            return UNIFIED_ERR_IPC_NOTIFY_FAILED;
        }
    }
    *address = *address | mask;
    return UNIFIED_OK;
}

/**
 * @brief mock 原子 AND。
 *
 * @param address 待更新地址。
 * @param mask AND mask。
 * @param user_context mock 上下文。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t mock_atomic_and_u32(volatile uint32_t *address,
                                           uint32_t mask,
                                           void *user_context)
{
    mock_context_t *context; /**< mock 上下文。 */

    if (address == 0) {
        return UNIFIED_ERR_NULL;
    }

    context = (mock_context_t *)user_context;
    if (context != 0) {
        /* 记录 AND 次数并按需注入竞态。 */
        context->atomic_and_count++;
        if (context->inject_rs485_set_during_can_clear &&
            !context->injected_rs485_set_during_can_clear &&
            (address == &g_region.tx_pending_bitmap.bits) &&
            (mask == ~(uint32_t)(1u << PUT_SHM_INTERFACE_CAN))) {
            *address = *address | (uint32_t)(1u << PUT_SHM_INTERFACE_RS485);
            context->injected_rs485_set_during_can_clear = true;
        }
        if (context->inject_tx_enqueue_during_pending_clear &&
            !context->injected_tx_enqueue_during_pending_clear &&
            (context->inject_ring != 0) &&
            (context->inject_pending != 0) &&
            (address == &context->inject_pending->bits) &&
            (mask == ~(uint32_t)(1u << context->inject_ring->header.interface_id))) {
            uint32_t descriptor_index; /**< 注入 descriptor 下标。 */
            uint32_t write_seq;        /**< 注入前写序号。 */

            write_seq = context->inject_ring->producer.write_seq;
            descriptor_index = write_seq % context->inject_ring->producer.depth;
            context->inject_descriptor.descriptor_crc16 =
                test_descriptor_crc(&context->inject_descriptor);
            context->inject_ring->descriptors[descriptor_index] = context->inject_descriptor;
            context->inject_ring->producer.write_seq = write_seq + 1u;
            context->inject_ring->producer.enqueue_count++;
            *address = *address | (uint32_t)(1u << context->inject_ring->header.interface_id);
            context->injected_tx_enqueue_during_pending_clear = true;
        }
    }
    *address = *address & mask;
    return UNIFIED_OK;
}

/**
 * @brief mock 原子 ADD。
 *
 * @param address 待更新地址。
 * @param value 累加值。
 * @param user_context mock 上下文。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t mock_atomic_add_u32(volatile uint32_t *address,
                                           uint32_t value,
                                           void *user_context)
{
    mock_context_t *context; /**< mock 上下文。 */

    if (address == 0) {
        return UNIFIED_ERR_NULL;
    }
    context = (mock_context_t *)user_context;
    if (context != 0) {
        /* 记录 ADD 次数。 */
        context->atomic_add_count++;
    }
    *address = *address + value;
    return UNIFIED_OK;
}

/**
 * @brief 构造 mock 平台操作集合。
 *
 * @param context mock 上下文。
 * @return mock 平台操作集合。
 */
static linux_shm_platform_ops_t make_ops(mock_context_t *context)
{
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */

    ops = *linux_shm_platform_default_ops();
    ops.cache_flush = mock_cache_flush;
    ops.cache_invalidate = mock_cache_invalidate;
    ops.memory_barrier = mock_memory_barrier;
    ops.notify = mock_notify;
    ops.atomic_or_u32 = mock_atomic_or_u32;
    ops.atomic_and_u32 = mock_atomic_and_u32;
    ops.atomic_add_u32 = mock_atomic_add_u32;
    ops.user_context = context;
    return ops;
}

/**
 * @brief 初始化测试 IPC。
 *
 * @param ipc IPC 上下文。
 * @param context mock 上下文。
 * @param ops mock 平台操作集合。
 */
static void setup_ipc(linux_shm_ipc_t *ipc,
                      mock_context_t *context,
                      linux_shm_platform_ops_t *ops)
{
    linux_shm_ipc_init(ipc);
    memset(context, 0, sizeof(*context));
    *ops = make_ops(context);
    (void)linux_shm_ipc_format_region(ipc, &g_region, 11u, 22u, ops);
}

/**
 * @brief 构造测试 descriptor。
 *
 * @param frame_id Frame Pool block ID。
 * @param source_interface 来源接口。
 * @param target_interface 目标接口。
 * @return 测试 descriptor。
 */
static put_shm_descriptor_t make_descriptor(uint32_t frame_id,
                                            put_shm_interface_t source_interface,
                                            put_shm_interface_t target_interface)
{
    put_shm_descriptor_t descriptor; /**< 测试 descriptor。 */

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.frame_id = frame_id;
    descriptor.frame_offset = frame_id * PUT_SHM_FRAME_POOL_BLOCK_SIZE;
    descriptor.frame_length = ANYMSG_HEADER_SIZE;
    descriptor.source_interface = (uint8_t)source_interface;
    descriptor.target_interface = (uint8_t)target_interface;
    descriptor.source_cid[0] = 0x20u;
    descriptor.destination_cid[0] = 0xC0u;
    descriptor.type = ANYMSG_TYPE_RAW_CAN;
    descriptor.priority = 2u;
    descriptor.ttl = 8u;
    descriptor.epoch = 11u;
    descriptor.flags = 0xA5000000u | frame_id;
    return descriptor;
}

/**
 * @brief 分配 frame 并发布到 RX ring，使其进入 RX_QUEUED 状态。
 *
 * @param ipc IPC 上下文。
 * @param source_interface 来源接口。
 * @param target_interface 目标接口。
 * @param out_frame_id 输出 frame_id。
 * @return 0 表示成功。
 */
static int allocate_rx_queued_frame(linux_shm_ipc_t *ipc,
                                    put_shm_interface_t source_interface,
                                    put_shm_interface_t target_interface,
                                    uint32_t *out_frame_id)
{
    uint8_t *buffer;                 /**< 分配到的 frame buffer。 */
    uint16_t capacity;               /**< frame buffer 容量。 */
    put_shm_descriptor_t descriptor; /**< RX descriptor。 */

    CHECK(linux_shm_frame_alloc(ipc, source_interface, out_frame_id, &buffer, &capacity) ==
          UNIFIED_OK);
    CHECK(buffer == g_region.frame_pool[*out_frame_id].bytes);
    CHECK(capacity == PUT_SHM_FRAME_POOL_BLOCK_SIZE);
    descriptor = make_descriptor(*out_frame_id, source_interface, target_interface);
    CHECK(linux_shm_enqueue_rx_descriptor(ipc, source_interface, &descriptor) == UNIFIED_OK);
    CHECK(ipc->frames[*out_frame_id].state == LINUX_SHM_FRAME_STATE_RX_QUEUED);
    return 0;
}

/**
 * @brief 直接模拟 RTOS 发布 TX descriptor。
 *
 * @param ring TX ring。
 * @param pending_line pending 控制行。
 * @param descriptor descriptor。
 */
static void publish_tx_direct(put_shm_descriptor_ring_t *ring,
                              put_shm_pending_line_t *pending_line,
                              const put_shm_descriptor_t *descriptor)
{
    put_shm_descriptor_t descriptor_copy; /**< 带 CRC 的 descriptor 副本。 */
    uint32_t descriptor_index;            /**< descriptor 下标。 */
    uint32_t pending_bit;                 /**< 当前 ring pending bit。 */

    descriptor_copy = *descriptor;
    descriptor_copy.descriptor_crc16 = test_descriptor_crc(&descriptor_copy);
    descriptor_index = ring->producer.write_seq % ring->producer.depth;
    ring->descriptors[descriptor_index] = descriptor_copy;
    ring->producer.write_seq++;
    ring->producer.enqueue_count++;
    pending_bit = (uint32_t)(1u << ring->header.interface_id);
    pending_line->bits = pending_line->bits | pending_bit;
}

/**
 * @brief 直接模拟 RTOS 发布 reclaim descriptor。
 *
 * @param region 共享内存 region。
 * @param descriptor reclaim descriptor。
 */
static void publish_reclaim_direct(put_shm_region_t *region,
                                   const put_shm_reclaim_descriptor_t *descriptor)
{
    put_shm_reclaim_descriptor_t descriptor_copy; /**< 带 CRC 的 reclaim 副本。 */
    uint32_t descriptor_index;                    /**< descriptor 下标。 */

    descriptor_copy = *descriptor;
    descriptor_copy.descriptor_crc16 = test_reclaim_crc(&descriptor_copy);
    descriptor_index = region->reclaim_ring.producer.write_seq %
                       region->reclaim_ring.producer.depth;
    region->reclaim_ring.descriptors[descriptor_index] = descriptor_copy;
    region->reclaim_ring.producer.write_seq++;
    region->reclaim_ring.producer.enqueue_count++;
    region->reclaim_pending.bits = 1u;
}

/**
 * @brief 测试 format 和 attach 校验。
 *
 * @return 0 表示通过。
 */
static int test_format_and_attach(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    uint16_t saved_version;       /**< 临时保存的 ABI 版本。 */
    uint16_t saved_depth;         /**< 临时保存的 ring 深度。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(g_region.header.magic == PUT_SHM_REGION_MAGIC);
    CHECK(g_region.header.version == PUT_SHM_IPC_VERSION);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].header.ring_kind ==
          (uint8_t)PUT_SHM_RING_KIND_RX);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].header.ring_kind ==
          (uint8_t)PUT_SHM_RING_KIND_TX);
    CHECK(g_region.reclaim_ring.header.ring_kind == (uint8_t)PUT_SHM_RING_KIND_RECLAIM);

    CHECK(linux_shm_ipc_attach(0, &g_region, &ops) == UNIFIED_ERR_NULL);
    CHECK(linux_shm_ipc_attach(&ipc, 0, &ops) == UNIFIED_ERR_NULL);
    CHECK(linux_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_OK);

    g_region.header.magic = 0u;
    CHECK(linux_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_ERR_PROTOCOL_HEADER);
    g_region.header.magic = PUT_SHM_REGION_MAGIC;

    saved_version = g_region.header.version;
    g_region.header.version = 0u;
    CHECK(linux_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_ERR_PROTOCOL_HEADER);
    g_region.header.version = saved_version;

    saved_depth = g_region.rx_rings[PUT_SHM_INTERFACE_CAN].header.depth;
    g_region.rx_rings[PUT_SHM_INTERFACE_CAN].header.depth = 0u;
    CHECK(linux_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_ERR_LENGTH);
    g_region.rx_rings[PUT_SHM_INTERFACE_CAN].header.depth = saved_depth;
    return 0;
}

/**
 * @brief 测试显式 init 能清理脏上下文。
 *
 * @return 0 表示通过。
 */
static int test_ipc_init_clears_dirty_context(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    linux_shm_ipc_stats_t stats;  /**< 统计快照。 */

    memset(&ipc, 0xA5, sizeof(ipc));
    memset(&context, 0, sizeof(context));
    ops = make_ops(&context);
    linux_shm_ipc_init(&ipc);
    CHECK(linux_shm_ipc_format_region(&ipc, &g_region, 11u, 22u, &ops) == UNIFIED_OK);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.frame_pool.capacity == PUT_SHM_FRAME_POOL_BLOCK_COUNT);
    CHECK(!ipc.mapped);
    return 0;
}

/**
 * @brief 测试 host 后端 map/unmap。
 *
 * @return 0 表示通过。
 */
static int test_host_map_unmap(void)
{
    linux_shm_ipc_t ipc;              /**< IPC 上下文。 */
    put_shm_region_t *mapped_region;  /**< map 后的 region 地址。 */
    void *mapping_context;            /**< map 后的映射私有上下文。 */
    size_t mapped_size;               /**< map 后的映射大小。 */

    memset(&ipc, 0, sizeof(ipc));
    CHECK(linux_shm_ipc_map(&ipc, 0u, PUT_SHM_REGION_SIZE, 0) == UNIFIED_OK);
    CHECK(ipc.region != 0);
    CHECK((((uintptr_t)ipc.region) % PUT_SHM_CACHE_LINE_SIZE) == 0u);
    CHECK(ipc.mapped);

    mapped_region = ipc.region;
    mapping_context = ipc.mapping_context;
    mapped_size = ipc.mapped_size;
    CHECK(linux_shm_ipc_format_region(&ipc, mapped_region, 11u, 22u, 0) == UNIFIED_OK);
    CHECK(ipc.region == mapped_region);
    CHECK(ipc.mapping_context == mapping_context);
    CHECK(ipc.mapped_size == mapped_size);
    CHECK(ipc.mapped);

    CHECK(linux_shm_ipc_attach(&ipc, mapped_region, 0) == UNIFIED_OK);
    CHECK(ipc.region == mapped_region);
    CHECK(ipc.mapping_context == mapping_context);
    CHECK(ipc.mapped_size == mapped_size);
    CHECK(ipc.mapped);

    linux_shm_ipc_unmap(&ipc);
    CHECK(ipc.region == 0);
    return 0;
}

/**
 * @brief 测试 Frame Pool 分配、释放和配额。
 *
 * @return 0 表示通过。
 */
static int test_frame_pool_alloc_release_quota(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    uint32_t frame_id;            /**< 分配到的 frame_id。 */
    uint8_t *buffer;              /**< 分配到的 frame buffer。 */
    uint16_t capacity;            /**< frame buffer 容量。 */
    linux_shm_ipc_stats_t stats;  /**< 统计快照。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(linux_shm_ipc_set_interface_quota(&ipc, PUT_SHM_INTERFACE_CAN, 1u) == UNIFIED_OK);
    CHECK(linux_shm_frame_alloc(&ipc, PUT_SHM_INTERFACE_CAN, &frame_id, &buffer, &capacity) ==
          UNIFIED_OK);
    CHECK(frame_id == 0u);
    CHECK(buffer == g_region.frame_pool[0].bytes);
    CHECK(capacity == PUT_SHM_FRAME_POOL_BLOCK_SIZE);
    CHECK(linux_shm_frame_alloc(&ipc, PUT_SHM_INTERFACE_CAN, &frame_id, &buffer, &capacity) ==
          UNIFIED_ERR_IPC_FRAME_POOL_FULL);
    CHECK(linux_shm_frame_release(&ipc, 0u, PUT_SHM_RECLAIM_REASON_NONE) == UNIFIED_OK);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.frame_pool.allocated == 1u);
    CHECK(stats.frame_pool.released == 1u);
    CHECK(stats.frame_pool.full_count == 1u);
    CHECK(stats.frame_pool.used == 0u);
    return 0;
}

/**
 * @brief 测试 Frame Pool 全局耗尽。
 *
 * @return 0 表示通过。
 */
static int test_frame_pool_global_full(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    uint32_t frame_id;            /**< 分配到的 frame_id。 */
    uint8_t *buffer;              /**< 分配到的 frame buffer。 */
    uint16_t capacity;            /**< frame buffer 容量。 */
    uint32_t index;               /**< 分配循环索引。 */
    linux_shm_ipc_stats_t stats;  /**< 统计快照。 */

    setup_ipc(&ipc, &context, &ops);
    for (index = 0u; index < PUT_SHM_FRAME_POOL_BLOCK_COUNT; ++index) {
        CHECK(linux_shm_frame_alloc(&ipc,
                                    PUT_SHM_INTERFACE_CAN,
                                    &frame_id,
                                    &buffer,
                                    &capacity) == UNIFIED_OK);
        CHECK(frame_id == index);
    }

    CHECK(linux_shm_frame_alloc(&ipc,
                                PUT_SHM_INTERFACE_CAN,
                                &frame_id,
                                &buffer,
                                &capacity) == UNIFIED_ERR_IPC_FRAME_POOL_FULL);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.frame_pool.used == PUT_SHM_FRAME_POOL_BLOCK_COUNT);
    CHECK(stats.frame_pool.high_watermark == PUT_SHM_FRAME_POOL_BLOCK_COUNT);
    CHECK(stats.frame_pool.full_count == 1u);
    return 0;
}

/**
 * @brief 测试 RX enqueue、pending 和 notify partial-success。
 *
 * @return 0 表示通过。
 */
static int test_rx_enqueue_and_notify_failure(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    uint32_t frame_id;            /**< 分配到的 frame_id。 */
    uint8_t *buffer;              /**< 分配到的 frame buffer。 */
    uint16_t capacity;            /**< frame buffer 容量。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH]; /**< 来源 CID。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< 目标 CID。 */

    setup_ipc(&ipc, &context, &ops);
    memset(source_cid, 0x20, sizeof(source_cid));
    memset(destination_cid, 0xC0, sizeof(destination_cid));
    CHECK(linux_shm_frame_alloc(&ipc, PUT_SHM_INTERFACE_CAN, &frame_id, &buffer, &capacity) ==
          UNIFIED_OK);
    memset(buffer, 0x5A, ANYMSG_HEADER_SIZE);
    context.fail_notify = true;
    CHECK(linux_shm_frame_commit_rx(&ipc,
                                    frame_id,
                                    ANYMSG_HEADER_SIZE,
                                    PUT_SHM_INTERFACE_CAN,
                                    PUT_SHM_INTERFACE_RS485,
                                    source_cid,
                                    destination_cid,
                                    ANYMSG_TYPE_RAW_CAN,
                                    2u,
                                    8u,
                                    11u,
                                    0u) == UNIFIED_OK);
    CHECK(context.notify_count == 1u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].producer.write_seq == 1u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].producer.notify_fail_count == 1u);
    CHECK(g_region.rx_pending_bitmap.bits == (1u << PUT_SHM_INTERFACE_CAN));
    return 0;
}

/**
 * @brief 测试 producer line flush 失败后 RX descriptor 仍按已发布处理。
 *
 * @return 0 表示通过。
 */
static int test_rx_enqueue_producer_flush_failure_partial_success(void)
{
    linux_shm_ipc_t ipc;              /**< IPC 上下文。 */
    mock_context_t context;           /**< mock 上下文。 */
    linux_shm_platform_ops_t ops;     /**< mock 平台操作集合。 */
    uint32_t frame_id;                /**< 分配到的 frame_id。 */
    uint8_t *buffer;                  /**< 分配到的 frame buffer。 */
    uint16_t capacity;                /**< frame buffer 容量。 */
    put_shm_descriptor_t descriptor;  /**< 测试 descriptor。 */
    linux_shm_ipc_stats_t stats;      /**< 统计快照。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(linux_shm_frame_alloc(&ipc, PUT_SHM_INTERFACE_CAN, &frame_id, &buffer, &capacity) ==
          UNIFIED_OK);
    CHECK(buffer == g_region.frame_pool[frame_id].bytes);
    CHECK(capacity == PUT_SHM_FRAME_POOL_BLOCK_SIZE);
    descriptor = make_descriptor(frame_id, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    context.fail_flush_on_call = 2u;
    CHECK(linux_shm_enqueue_rx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_CAN,
                                          &descriptor) == UNIFIED_OK);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].producer.write_seq == 1u);
    CHECK(ipc.frames[frame_id].state == LINUX_SHM_FRAME_STATE_RX_QUEUED);
    CHECK(g_region.rx_pending_bitmap.bits == 0u);
    CHECK(linux_shm_frame_release(&ipc,
                                  frame_id,
                                  PUT_SHM_RECLAIM_REASON_NONE) == UNIFIED_ERR_INVALID_ARG);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.cache_sync_error_count == 1u);
    return 0;
}

/**
 * @brief 测试 pending OR 失败后 RX descriptor 仍按已发布处理。
 *
 * @return 0 表示通过。
 */
static int test_rx_enqueue_pending_failure_partial_success(void)
{
    linux_shm_ipc_t ipc;              /**< IPC 上下文。 */
    mock_context_t context;           /**< mock 上下文。 */
    linux_shm_platform_ops_t ops;     /**< mock 平台操作集合。 */
    uint32_t frame_id;                /**< 分配到的 frame_id。 */
    uint8_t *buffer;                  /**< 分配到的 frame buffer。 */
    uint16_t capacity;                /**< frame buffer 容量。 */
    put_shm_descriptor_t descriptor;  /**< 测试 descriptor。 */
    linux_shm_ipc_stats_t stats;      /**< 统计快照。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(linux_shm_frame_alloc(&ipc, PUT_SHM_INTERFACE_CAN, &frame_id, &buffer, &capacity) ==
          UNIFIED_OK);
    descriptor = make_descriptor(frame_id, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    context.fail_atomic_or = true;
    CHECK(linux_shm_enqueue_rx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_CAN,
                                          &descriptor) == UNIFIED_OK);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].producer.write_seq == 1u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].producer.notify_fail_count == 1u);
    CHECK(ipc.frames[frame_id].state == LINUX_SHM_FRAME_STATE_RX_QUEUED);
    CHECK(g_region.rx_pending_bitmap.bits == 0u);
    CHECK(linux_shm_enqueue_rx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_CAN,
                                          &descriptor) == UNIFIED_ERR_INVALID_ARG);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.mailbox.notify_fail_count == 1u);
    return 0;
}

/**
 * @brief 测试同一 frame 不能重复写入 RX ring。
 *
 * @return 0 表示通过。
 */
static int test_rx_enqueue_rejects_duplicate_frame(void)
{
    linux_shm_ipc_t ipc;              /**< IPC 上下文。 */
    mock_context_t context;           /**< mock 上下文。 */
    linux_shm_platform_ops_t ops;     /**< mock 平台操作集合。 */
    uint32_t frame_id;                /**< 分配到的 frame_id。 */
    uint8_t *buffer;                  /**< 分配到的 frame buffer。 */
    uint16_t capacity;                /**< frame buffer 容量。 */
    put_shm_descriptor_t descriptor;  /**< 测试 descriptor。 */
    uint32_t write_seq_before_retry;  /**< 重复入队前的 write_seq。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(linux_shm_frame_alloc(&ipc, PUT_SHM_INTERFACE_CAN, &frame_id, &buffer, &capacity) ==
          UNIFIED_OK);
    descriptor = make_descriptor(frame_id, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    CHECK(linux_shm_enqueue_rx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_CAN,
                                          &descriptor) == UNIFIED_OK);

    write_seq_before_retry = g_region.rx_rings[PUT_SHM_INTERFACE_CAN].producer.write_seq;
    CHECK(ipc.frames[frame_id].state == LINUX_SHM_FRAME_STATE_RX_QUEUED);
    CHECK(linux_shm_enqueue_rx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_CAN,
                                          &descriptor) == UNIFIED_ERR_INVALID_ARG);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].producer.write_seq == write_seq_before_retry);
    return 0;
}

/**
 * @brief 测试 RX_QUEUED frame 在 reclaim 前不能公开释放。
 *
 * @return 0 表示通过。
 */
static int test_release_rejects_rx_queued_without_reclaim(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    uint32_t frame_id;            /**< 分配到的 frame_id。 */
    uint8_t *buffer;              /**< 分配到的 frame buffer。 */
    uint16_t capacity;            /**< frame buffer 容量。 */
    put_shm_descriptor_t descriptor; /**< 测试 descriptor。 */
    put_shm_reclaim_descriptor_t reclaim; /**< reclaim descriptor。 */
    put_shm_reclaim_descriptor_t reclaim_output; /**< 输出 reclaim descriptor。 */
    linux_shm_ipc_stats_t stats;  /**< 统计快照。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(linux_shm_frame_alloc(&ipc, PUT_SHM_INTERFACE_CAN, &frame_id, &buffer, &capacity) ==
          UNIFIED_OK);
    descriptor = make_descriptor(frame_id, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    CHECK(linux_shm_enqueue_rx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_CAN,
                                          &descriptor) == UNIFIED_OK);
    CHECK(ipc.frames[frame_id].state == LINUX_SHM_FRAME_STATE_RX_QUEUED);
    CHECK(linux_shm_frame_release(&ipc,
                                  frame_id,
                                  PUT_SHM_RECLAIM_REASON_NONE) == UNIFIED_ERR_INVALID_ARG);
    CHECK((ipc.allocation_bitmap & (uint64_t)(1ULL << frame_id)) != 0u);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.frame_pool.used == 1u);

    memset(&reclaim, 0, sizeof(reclaim));
    reclaim.frame_id = frame_id;
    reclaim.reason = PUT_SHM_RECLAIM_REASON_NO_ROUTE;
    reclaim.source_interface = (uint8_t)PUT_SHM_INTERFACE_CAN;
    reclaim.target_interface = (uint8_t)PUT_SHM_INTERFACE_RS485;
    reclaim.epoch = 11u;
    publish_reclaim_direct(&g_region, &reclaim);
    CHECK(linux_shm_dequeue_reclaim_descriptor(&ipc, &reclaim_output) == UNIFIED_OK);
    CHECK((ipc.allocation_bitmap & (uint64_t)(1ULL << frame_id)) == 0u);
    return 0;
}

/**
 * @brief 测试 RX ring 满时不自动释放 frame。
 *
 * @return 0 表示通过。
 */
static int test_rx_ring_full_keeps_frame_allocated(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    put_shm_descriptor_t descriptor; /**< 测试 descriptor。 */
    uint32_t index;               /**< ring 填充索引。 */
    uint32_t frame_id;            /**< 分配到的 frame_id。 */
    uint8_t *buffer;              /**< 分配到的 frame buffer。 */
    uint16_t capacity;            /**< frame buffer 容量。 */
    linux_shm_ipc_stats_t stats;  /**< 统计快照。 */

    setup_ipc(&ipc, &context, &ops);
    for (index = 0u; index < PUT_SHM_DESCRIPTOR_RING_DEPTH; ++index) {
        descriptor = make_descriptor(index, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
        ipc.allocation_bitmap |= (uint64_t)(1ULL << index);
        ipc.frames[index].state = LINUX_SHM_FRAME_STATE_ALLOCATED;
        ipc.frames[index].source_interface = (uint8_t)PUT_SHM_INTERFACE_CAN;
        CHECK(linux_shm_enqueue_rx_descriptor(&ipc,
                                              PUT_SHM_INTERFACE_CAN,
                                              &descriptor) == UNIFIED_OK);
    }

    CHECK(linux_shm_frame_alloc(&ipc, PUT_SHM_INTERFACE_CAN, &frame_id, &buffer, &capacity) ==
          UNIFIED_OK);
    descriptor = make_descriptor(frame_id, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    CHECK(linux_shm_enqueue_rx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_CAN,
                                          &descriptor) == UNIFIED_ERR_IPC_QUEUE_FULL);
    CHECK((ipc.allocation_bitmap & (uint64_t)(1ULL << frame_id)) != 0u);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.rx_rings[PUT_SHM_INTERFACE_CAN].full_count == 1u);
    return 0;
}

/**
 * @brief 测试 TX dequeue 保持元数据和返回 frame 指针。
 *
 * @return 0 表示通过。
 */
static int test_tx_dequeue_success(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    uint32_t frame_id;            /**< 分配到的 frame_id。 */
    put_shm_descriptor_t descriptor; /**< 输入 descriptor。 */
    put_shm_descriptor_t output;   /**< 输出 descriptor。 */
    const uint8_t *frame;          /**< 输出 frame 指针。 */
    uint16_t frame_length;         /**< 输出 frame 长度。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(allocate_rx_queued_frame(&ipc,
                                   PUT_SHM_INTERFACE_CAN,
                                   PUT_SHM_INTERFACE_RS485,
                                   &frame_id) == 0);
    descriptor = make_descriptor(frame_id, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    publish_tx_direct(&g_region.tx_rings[PUT_SHM_INTERFACE_RS485],
                      &g_region.tx_pending_bitmap,
                      &descriptor);
    CHECK(linux_shm_dequeue_tx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_RS485,
                                          &output,
                                          &frame,
                                          &frame_length) == UNIFIED_OK);
    CHECK(output.frame_id == frame_id);
    CHECK(frame == g_region.frame_pool[frame_id].bytes);
    CHECK(frame_length == ANYMSG_HEADER_SIZE);
    CHECK(g_region.tx_pending_bitmap.bits == 0u);
    return 0;
}

/**
 * @brief 测试 TX CRC 和接口错配坏 descriptor 会被消费。
 *
 * @return 0 表示通过。
 */
static int test_tx_bad_descriptor_consumed(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    uint32_t frame_id;            /**< 分配到的 frame_id。 */
    put_shm_descriptor_t descriptor; /**< 测试 descriptor。 */
    put_shm_descriptor_t output;   /**< 输出 descriptor。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(allocate_rx_queued_frame(&ipc,
                                   PUT_SHM_INTERFACE_CAN,
                                   PUT_SHM_INTERFACE_RS485,
                                   &frame_id) == 0);
    descriptor = make_descriptor(frame_id, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_ETHERNET);
    publish_tx_direct(&g_region.tx_rings[PUT_SHM_INTERFACE_RS485],
                      &g_region.tx_pending_bitmap,
                      &descriptor);
    CHECK(linux_shm_dequeue_tx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_RS485,
                                          &output,
                                          0,
                                          0) == UNIFIED_ERR_INVALID_ARG);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].consumer.read_seq == 1u);

    descriptor = make_descriptor(frame_id, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    publish_tx_direct(&g_region.tx_rings[PUT_SHM_INTERFACE_RS485],
                      &g_region.tx_pending_bitmap,
                      &descriptor);
    g_region.tx_rings[PUT_SHM_INTERFACE_RS485].descriptors[1].type ^= 0x01u;
    CHECK(linux_shm_dequeue_tx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_RS485,
                                          &output,
                                          0,
                                          0) == UNIFIED_ERR_CRC);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].consumer.read_seq == 2u);
    return 0;
}

/**
 * @brief 测试 TX descriptor 不能接管尚未交给 RTOS 的 frame。
 *
 * @return 0 表示通过。
 */
static int test_tx_dequeue_rejects_unpublished_frame(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    uint32_t frame_id;            /**< 分配到的 frame_id。 */
    uint8_t *buffer;              /**< 分配到的 frame buffer。 */
    uint16_t capacity;            /**< frame buffer 容量。 */
    put_shm_descriptor_t descriptor; /**< 测试 descriptor。 */
    put_shm_descriptor_t output;   /**< 输出 descriptor。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(linux_shm_frame_alloc(&ipc, PUT_SHM_INTERFACE_CAN, &frame_id, &buffer, &capacity) ==
          UNIFIED_OK);
    descriptor = make_descriptor(frame_id, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    publish_tx_direct(&g_region.tx_rings[PUT_SHM_INTERFACE_RS485],
                      &g_region.tx_pending_bitmap,
                      &descriptor);
    CHECK(linux_shm_dequeue_tx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_RS485,
                                          &output,
                                          0,
                                          0) == UNIFIED_ERR_INVALID_ARG);
    CHECK(ipc.frames[frame_id].state == LINUX_SHM_FRAME_STATE_ALLOCATED);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].consumer.read_seq == 1u);
    return 0;
}

/**
 * @brief 测试 reclaim dequeue 释放 Frame Pool。
 *
 * @return 0 表示通过。
 */
static int test_reclaim_dequeue_releases_frame(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    uint32_t frame_id;            /**< 分配到的 frame_id。 */
    put_shm_reclaim_descriptor_t reclaim; /**< reclaim descriptor。 */
    put_shm_reclaim_descriptor_t output; /**< 输出 reclaim descriptor。 */
    linux_shm_ipc_stats_t stats;  /**< 统计快照。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(allocate_rx_queued_frame(&ipc,
                                   PUT_SHM_INTERFACE_CAN,
                                   PUT_SHM_INTERFACE_RS485,
                                   &frame_id) == 0);
    memset(&reclaim, 0, sizeof(reclaim));
    reclaim.frame_id = frame_id;
    reclaim.reason = PUT_SHM_RECLAIM_REASON_NO_ROUTE;
    reclaim.source_interface = (uint8_t)PUT_SHM_INTERFACE_CAN;
    reclaim.target_interface = (uint8_t)PUT_SHM_INTERFACE_RS485;
    reclaim.epoch = 11u;
    publish_reclaim_direct(&g_region, &reclaim);
    CHECK(linux_shm_dequeue_reclaim_descriptor(&ipc, &output) == UNIFIED_OK);
    CHECK(output.frame_id == frame_id);
    CHECK((ipc.allocation_bitmap & (uint64_t)(1ULL << frame_id)) == 0u);
    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.reclaim_ack_count == 1u);
    CHECK(stats.frame_pool.released == 1u);
    CHECK(stats.reclaim_reason_count[PUT_SHM_RECLAIM_REASON_NO_ROUTE] == 1u);
    return 0;
}

/**
 * @brief 测试 reclaim 不能释放未发布或接口元数据错配的 frame。
 *
 * @return 0 表示通过。
 */
static int test_reclaim_rejects_unpublished_and_mismatched_frame(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    uint32_t frame_id;            /**< 分配到的 frame_id。 */
    uint8_t *buffer;              /**< 分配到的 frame buffer。 */
    uint16_t capacity;            /**< frame buffer 容量。 */
    put_shm_reclaim_descriptor_t reclaim; /**< reclaim descriptor。 */
    put_shm_reclaim_descriptor_t output; /**< 输出 reclaim descriptor。 */
    linux_shm_ipc_stats_t stats;  /**< 统计快照。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(linux_shm_frame_alloc(&ipc, PUT_SHM_INTERFACE_CAN, &frame_id, &buffer, &capacity) ==
          UNIFIED_OK);
    memset(&reclaim, 0, sizeof(reclaim));
    reclaim.frame_id = frame_id;
    reclaim.reason = PUT_SHM_RECLAIM_REASON_NO_ROUTE;
    reclaim.source_interface = (uint8_t)PUT_SHM_INTERFACE_CAN;
    reclaim.target_interface = (uint8_t)PUT_SHM_INTERFACE_RS485;
    publish_reclaim_direct(&g_region, &reclaim);
    CHECK(linux_shm_dequeue_reclaim_descriptor(&ipc, &output) == UNIFIED_ERR_INVALID_ARG);
    CHECK(ipc.frames[frame_id].state == LINUX_SHM_FRAME_STATE_ALLOCATED);
    CHECK((ipc.allocation_bitmap & (uint64_t)(1ULL << frame_id)) != 0u);

    CHECK(linux_shm_frame_release(&ipc, frame_id, PUT_SHM_RECLAIM_REASON_NONE) == UNIFIED_OK);
    publish_reclaim_direct(&g_region, &reclaim);
    CHECK(linux_shm_dequeue_reclaim_descriptor(&ipc, &output) == UNIFIED_ERR_INVALID_ARG);

    CHECK(allocate_rx_queued_frame(&ipc,
                                   PUT_SHM_INTERFACE_CAN,
                                   PUT_SHM_INTERFACE_RS485,
                                   &frame_id) == 0);
    reclaim.frame_id = frame_id;
    reclaim.source_interface = (uint8_t)PUT_SHM_INTERFACE_RS485;
    reclaim.target_interface = (uint8_t)PUT_SHM_INTERFACE_RS485;
    publish_reclaim_direct(&g_region, &reclaim);
    CHECK(linux_shm_dequeue_reclaim_descriptor(&ipc, &output) == UNIFIED_ERR_INVALID_ARG);
    CHECK((ipc.allocation_bitmap & (uint64_t)(1ULL << frame_id)) != 0u);

    linux_shm_ipc_get_stats(&ipc, &stats);
    CHECK(stats.reclaim_ack_count == 0u);
    CHECK(stats.descriptor_format_error_count == 3u);
    return 0;
}

/**
 * @brief 测试 pending clear 跨接口并发 set 不丢。
 *
 * @return 0 表示通过。
 */
static int test_pending_cross_interface_set_survives_clear(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    uint32_t frame_id;            /**< 分配到的 frame_id。 */
    put_shm_descriptor_t descriptor; /**< 测试 descriptor。 */
    put_shm_descriptor_t output;   /**< 输出 descriptor。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(allocate_rx_queued_frame(&ipc,
                                   PUT_SHM_INTERFACE_CAN,
                                   PUT_SHM_INTERFACE_CAN,
                                   &frame_id) == 0);
    descriptor = make_descriptor(frame_id, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_CAN);
    publish_tx_direct(&g_region.tx_rings[PUT_SHM_INTERFACE_CAN],
                      &g_region.tx_pending_bitmap,
                      &descriptor);
    context.inject_rs485_set_during_can_clear = true;
    CHECK(linux_shm_dequeue_tx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_CAN,
                                          &output,
                                          0,
                                          0) == UNIFIED_OK);
    CHECK(context.injected_rs485_set_during_can_clear);
    CHECK(g_region.tx_pending_bitmap.bits == (1u << PUT_SHM_INTERFACE_RS485));
    return 0;
}

/**
 * @brief 测试 pending clear 后同接口新入队会置回 bit。
 *
 * @return 0 表示通过。
 */
static int test_pending_same_interface_requeue_after_clear(void)
{
    linux_shm_ipc_t ipc;          /**< IPC 上下文。 */
    mock_context_t context;       /**< mock 上下文。 */
    linux_shm_platform_ops_t ops; /**< mock 平台操作集合。 */
    uint32_t first_frame_id;      /**< 第一个 frame_id。 */
    uint32_t second_frame_id;     /**< 第二个 frame_id。 */
    put_shm_descriptor_t first;   /**< 第一个 descriptor。 */
    put_shm_descriptor_t second;  /**< 注入 descriptor。 */
    put_shm_descriptor_t output;  /**< 输出 descriptor。 */
    uint32_t atomic_or_before;    /**< 出队前 OR 次数。 */

    setup_ipc(&ipc, &context, &ops);
    CHECK(allocate_rx_queued_frame(&ipc,
                                   PUT_SHM_INTERFACE_CAN,
                                   PUT_SHM_INTERFACE_CAN,
                                   &first_frame_id) == 0);
    CHECK(allocate_rx_queued_frame(&ipc,
                                   PUT_SHM_INTERFACE_CAN,
                                   PUT_SHM_INTERFACE_CAN,
                                   &second_frame_id) == 0);
    first = make_descriptor(first_frame_id, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_CAN);
    second = make_descriptor(second_frame_id, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_CAN);
    publish_tx_direct(&g_region.tx_rings[PUT_SHM_INTERFACE_CAN],
                      &g_region.tx_pending_bitmap,
                      &first);
    atomic_or_before = context.atomic_or_count;
    context.inject_tx_enqueue_during_pending_clear = true;
    context.inject_ring = &g_region.tx_rings[PUT_SHM_INTERFACE_CAN];
    context.inject_pending = &g_region.tx_pending_bitmap;
    context.inject_descriptor = second;
    CHECK(linux_shm_dequeue_tx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_CAN,
                                          &output,
                                          0,
                                          0) == UNIFIED_OK);
    CHECK(context.injected_tx_enqueue_during_pending_clear);
    CHECK(context.atomic_or_count == (atomic_or_before + 1u));
    CHECK(g_region.tx_pending_bitmap.bits == (1u << PUT_SHM_INTERFACE_CAN));
    CHECK(linux_shm_dequeue_tx_descriptor(&ipc,
                                          PUT_SHM_INTERFACE_CAN,
                                          &output,
                                          0,
                                          0) == UNIFIED_OK);
    CHECK(output.frame_id == second_frame_id);
    CHECK(g_region.tx_pending_bitmap.bits == 0u);
    return 0;
}

/**
 * @brief 测试 devmem control ops 构造与未映射 cache 拒绝。
 *
 * @return 0 表示通过。
 */
static int test_devmem_control_ops_contract(void)
{
    linux_shm_platform_control_t control; /**< control ioctl 上下文。 */
    linux_shm_platform_ops_t ops;         /**< 生成的平台操作集合。 */
    uint8_t dummy_byte;                   /**< 未映射的临时字节。 */

    linux_shm_platform_control_init(&control);
    CHECK(linux_shm_platform_control_configure(&control, "") == UNIFIED_ERR_INVALID_ARG);
    CHECK(linux_shm_platform_control_configure(&control, "/dev/put_shm_ipc") ==
          UNIFIED_OK);
    CHECK(linux_shm_platform_make_devmem_control_ops(&ops, &control) == UNIFIED_OK);
    CHECK(ops.map_region != 0);
    CHECK(ops.unmap_region != 0);
    CHECK(ops.cache_flush != 0);
    CHECK(ops.cache_invalidate != 0);
    CHECK(ops.notify != 0);
    CHECK(ops.user_context == &control);
    CHECK(ops.cache_flush(&dummy_byte, sizeof(dummy_byte), ops.user_context) ==
          UNIFIED_ERR_IPC_NOT_READY);
    linux_shm_platform_control_close(&control);
    return 0;
}

/**
 * @brief 测试程序入口。
 *
 * @return 0 表示全部通过。
 */
int main(void)
{
    CHECK(test_format_and_attach() == 0);
    CHECK(test_ipc_init_clears_dirty_context() == 0);
    CHECK(test_host_map_unmap() == 0);
    CHECK(test_frame_pool_alloc_release_quota() == 0);
    CHECK(test_frame_pool_global_full() == 0);
    CHECK(test_rx_enqueue_and_notify_failure() == 0);
    CHECK(test_rx_enqueue_producer_flush_failure_partial_success() == 0);
    CHECK(test_rx_enqueue_pending_failure_partial_success() == 0);
    CHECK(test_rx_enqueue_rejects_duplicate_frame() == 0);
    CHECK(test_release_rejects_rx_queued_without_reclaim() == 0);
    CHECK(test_rx_ring_full_keeps_frame_allocated() == 0);
    CHECK(test_tx_dequeue_success() == 0);
    CHECK(test_tx_bad_descriptor_consumed() == 0);
    CHECK(test_tx_dequeue_rejects_unpublished_frame() == 0);
    CHECK(test_reclaim_dequeue_releases_frame() == 0);
    CHECK(test_reclaim_rejects_unpublished_and_mismatched_frame() == 0);
    CHECK(test_pending_cross_interface_set_survives_clear() == 0);
    CHECK(test_pending_same_interface_requeue_after_clear() == 0);
    CHECK(test_devmem_control_ops_contract() == 0);
    return 0;
}
