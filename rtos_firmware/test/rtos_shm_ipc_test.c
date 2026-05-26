/**
 * @file rtos_shm_ipc_test.c
 * @brief rtos_firmware 共享内存 IPC v2 host 单元测试。
 * @author Yukikaze
 */
#include "rtos_shm_ipc.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "crc16.h"

/** @brief 测试断言宏，失败时打印文件行号并返回非 0。 */
#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__,   \
                          #condition);                                                \
            return 1;                                                                 \
        }                                                                             \
    } while (0)

/**
 * @brief mock 平台操作计数上下文。
 */
typedef struct {
    uint32_t flush_count;                  /**< cache flush 调用次数。 */
    uint32_t invalidate_count;             /**< cache invalidate 调用次数。 */
    uint32_t barrier_count;                /**< memory barrier 调用次数。 */
    uint32_t notify_count;                 /**< notify 调用次数。 */
    uint32_t atomic_or_count;              /**< 原子 OR 调用次数。 */
    uint32_t atomic_and_count;             /**< 原子 AND 调用次数。 */
    uint32_t atomic_add_count;             /**< 原子 ADD 调用次数。 */
    const void *last_invalidate_address;   /**< 最近一次 invalidate 地址。 */
    size_t last_invalidate_length;         /**< 最近一次 invalidate 长度。 */
    put_shm_direction_t last_direction;    /**< 最近一次 notify 方向。 */
    bool fail_notify;                      /**< 是否强制 notify 失败。 */
    bool inject_enqueue_after_consumer_flush; /**< 是否在 consumer flush 后模拟并发入队。 */
    bool consumer_flush_seen;              /**< 是否已观察到 consumer read_seq flush。 */
    bool injected_enqueue_after_consumer_flush; /**< 是否已完成并发入队注入。 */
    bool inject_rs485_set_during_can_clear; /**< 是否在清 CAN pending 时并发设置 RS485。 */
    bool injected_rs485_set_during_can_clear; /**< 是否已完成 RS485 pending 注入。 */
    bool inject_enqueue_during_pending_clear; /**< 是否在 latest_write_seq 读取后、clear 前模拟入队。 */
    bool injected_enqueue_during_pending_clear; /**< 是否已完成 clear 前并发入队注入。 */
    put_shm_descriptor_ring_t *inject_ring; /**< 需要注入并发入队的 ring。 */
    put_shm_pending_line_t *inject_pending; /**< 需要注入 pending bit 的控制行。 */
    put_shm_descriptor_t inject_descriptor; /**< 注入的 descriptor。 */
} mock_platform_context_t;

/** @brief 测试共享内存 region，避免占用栈空间。 */
static put_shm_region_t g_region;

/**
 * @brief 计算测试 descriptor CRC。
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
 * @brief mock cache flush。
 *
 * @param address 待 flush 地址。
 * @param length 待 flush 长度。
 * @param user_context mock 上下文。
 * @return UNIFIED_OK 表示成功。
 */
static unified_error_t mock_cache_flush(const void *address, size_t length, void *user_context)
{
    mock_platform_context_t *context; /**< mock 平台上下文。 */

    (void)address;
    (void)length;

    context = (mock_platform_context_t *)user_context;
    if (context != 0) {
        /* 记录 flush 调用次数，用于验证写路径和 read_seq 发布路径。 */
        context->flush_count = context->flush_count + 1u;
        if ((context->inject_ring != 0) &&
            (address == &context->inject_ring->consumer)) {
            /* 记录消费者已发布 read_seq，用于模拟随后发生的生产者并发入队。 */
            context->consumer_flush_seen = true;
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
    mock_platform_context_t *context; /**< mock 平台上下文。 */
    uint32_t descriptor_index;        /**< 注入 descriptor 的 ring 下标。 */
    uint32_t write_seq;               /**< 注入前生产者写序号。 */

    (void)address;
    (void)length;

    context = (mock_platform_context_t *)user_context;
    if (context != 0) {
        /* 记录 invalidate 调用次数，用于验证共享控制行读路径。 */
        context->invalidate_count = context->invalidate_count + 1u;
        context->last_invalidate_address = address;
        context->last_invalidate_length = length;
        if (context->inject_enqueue_after_consumer_flush &&
            context->consumer_flush_seen &&
            !context->injected_enqueue_after_consumer_flush &&
            (context->inject_ring != 0) &&
            (context->inject_pending != 0) &&
            (address == &context->inject_ring->producer)) {
            /* 模拟消费者发布 read_seq 后，生产者刚好写入新 descriptor 并设置 pending。 */
            write_seq = context->inject_ring->producer.write_seq;
            descriptor_index = write_seq % context->inject_ring->producer.depth;
            context->inject_descriptor.descriptor_crc16 =
                test_descriptor_crc(&context->inject_descriptor);
            context->inject_ring->descriptors[descriptor_index] = context->inject_descriptor;
            context->inject_ring->producer.write_seq = write_seq + 1u;
            context->inject_ring->producer.enqueue_count =
                context->inject_ring->producer.enqueue_count + 1u;
            context->inject_pending->bits =
                context->inject_pending->bits |
                (uint32_t)(1u << context->inject_ring->header.interface_id);
            context->injected_enqueue_after_consumer_flush = true;
        }
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
    mock_platform_context_t *context; /**< mock 平台上下文。 */

    context = (mock_platform_context_t *)user_context;
    if (context != 0) {
        /* 记录 barrier 调用次数，用于验证 descriptor 发布顺序。 */
        context->barrier_count = context->barrier_count + 1u;
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
    mock_platform_context_t *context; /**< mock 平台上下文。 */

    context = (mock_platform_context_t *)user_context;
    if (context != 0) {
        /* 记录 notify 调用次数和方向，用于验证 empty -> non-empty doorbell。 */
        context->notify_count = context->notify_count + 1u;
        context->last_direction = direction;
        if (context->fail_notify) {
            /* 测试通知失败路径时返回明确错误。 */
            return UNIFIED_ERR_IPC_NOTIFY_FAILED;
        }
    }

    return UNIFIED_OK;
}

/**
 * @brief mock 32 位原子 OR。
 *
 * @param address 待原子更新的共享字段地址。
 * @param mask OR 操作使用的 bit mask。
 * @param user_context mock 上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t mock_atomic_or_u32(volatile uint32_t *address,
                                          uint32_t mask,
                                          void *user_context)
{
    mock_platform_context_t *context; /**< mock 平台上下文。 */

    if (address == 0) {
        /* 地址为空时不能模拟原子 OR。 */
        return UNIFIED_ERR_NULL;
    }

    context = (mock_platform_context_t *)user_context;
    if (context != 0) {
        /* 记录原子 OR 调用次数，用于验证 pending bit 设置路径。 */
        context->atomic_or_count = context->atomic_or_count + 1u;
    }

    *address = *address | mask;
    return UNIFIED_OK;
}

/**
 * @brief mock 32 位原子 AND。
 *
 * @param address 待原子更新的共享字段地址。
 * @param mask AND 操作使用的 bit mask。
 * @param user_context mock 上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t mock_atomic_and_u32(volatile uint32_t *address,
                                           uint32_t mask,
                                           void *user_context)
{
    mock_platform_context_t *context; /**< mock 平台上下文。 */

    if (address == 0) {
        /* 地址为空时不能模拟原子 AND。 */
        return UNIFIED_ERR_NULL;
    }

    context = (mock_platform_context_t *)user_context;
    if (context != 0) {
        /* 记录原子 AND 调用次数，用于验证 pending bit 清除路径。 */
        context->atomic_and_count = context->atomic_and_count + 1u;
        if (context->inject_rs485_set_during_can_clear &&
            !context->injected_rs485_set_during_can_clear &&
            (address == &g_region.rx_pending_bitmap.bits) &&
            (mask == ~(uint32_t)(1u << PUT_SHM_INTERFACE_CAN))) {
            /* 模拟清 CAN bit 的同时，生产者给 RS485 ring 设置 pending bit。 */
            *address = *address | (uint32_t)(1u << PUT_SHM_INTERFACE_RS485);
            context->injected_rs485_set_during_can_clear = true;
        }
        if (context->inject_enqueue_during_pending_clear &&
            !context->injected_enqueue_during_pending_clear &&
            (context->inject_ring != 0) &&
            (context->inject_pending != 0) &&
            (address == &context->inject_pending->bits) &&
            (mask == ~(uint32_t)(1u << context->inject_ring->header.interface_id))) {
            uint32_t descriptor_index; /**< 注入 descriptor 的 ring 下标。 */
            uint32_t write_seq;        /**< 注入前生产者写序号。 */

            /* 模拟清前最后一次读取 write_seq 后，同接口生产者刚好写入新 descriptor 并 set bit。 */
            write_seq = context->inject_ring->producer.write_seq;
            descriptor_index = write_seq % context->inject_ring->producer.depth;
            context->inject_descriptor.descriptor_crc16 =
                test_descriptor_crc(&context->inject_descriptor);
            context->inject_ring->descriptors[descriptor_index] = context->inject_descriptor;
            context->inject_ring->producer.write_seq = write_seq + 1u;
            context->inject_ring->producer.enqueue_count =
                context->inject_ring->producer.enqueue_count + 1u;
            *address = *address | (uint32_t)(1u << context->inject_ring->header.interface_id);
            context->injected_enqueue_during_pending_clear = true;
        }
    }

    *address = *address & mask;
    return UNIFIED_OK;
}

/**
 * @brief mock 32 位原子 ADD。
 *
 * @param address 待原子累加的共享字段地址。
 * @param value 需要累加的数值。
 * @param user_context mock 上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t mock_atomic_add_u32(volatile uint32_t *address,
                                           uint32_t value,
                                           void *user_context)
{
    mock_platform_context_t *context; /**< mock 平台上下文。 */

    if (address == 0) {
        /* 地址为空时不能模拟原子 ADD。 */
        return UNIFIED_ERR_NULL;
    }

    context = (mock_platform_context_t *)user_context;
    if (context != 0) {
        /* 记录原子 ADD 调用次数，用于验证 pending 诊断计数路径。 */
        context->atomic_add_count = context->atomic_add_count + 1u;
    }

    *address = *address + value;
    return UNIFIED_OK;
}

/**
 * @brief 构造 mock 平台操作集合。
 *
 * @param context mock 平台上下文。
 * @return mock 平台操作集合。
 */
static rtos_shm_platform_ops_t make_mock_ops(mock_platform_context_t *context)
{
    rtos_shm_platform_ops_t ops; /**< mock 平台操作集合。 */

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
 * @brief 初始化测试 region 和 mock ops。
 *
 * @param context mock 平台上下文。
 * @param ops 输出 mock 平台操作集合。
 */
static void setup_region(mock_platform_context_t *context, rtos_shm_platform_ops_t *ops)
{
    memset(context, 0, sizeof(*context));
    *ops = make_mock_ops(context);
    (void)rtos_shm_ipc_format_region(&g_region, 11u, 22u);
}

/**
 * @brief 构造测试 descriptor。
 *
 * @param frame_id Frame Pool block ID。
 * @param source_interface 来源物理接口。
 * @param target_interface 目标物理接口。
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
    descriptor.source_cid[1] = 0x01u;
    descriptor.destination_cid[0] = 0xC0u;
    descriptor.destination_cid[1] = 0x02u;
    descriptor.type = ANYMSG_TYPE_RAW_CAN;
    descriptor.priority = 2u;
    descriptor.ttl = 8u;
    descriptor.epoch = 11u;
    descriptor.flags = 0xA5A50000u | frame_id;
    return descriptor;
}

/**
 * @brief 直接向测试 ring 发布一个 descriptor。
 *
 * @param ring 测试 descriptor ring。
 * @param descriptor 待发布 descriptor。
 */
static void publish_descriptor_direct(put_shm_descriptor_ring_t *ring,
                                      const put_shm_descriptor_t *descriptor)
{
    put_shm_descriptor_t descriptor_copy; /**< 带 CRC 的 descriptor 副本。 */
    uint32_t descriptor_index;            /**< descriptor 写入下标。 */

    descriptor_copy = *descriptor;
    descriptor_copy.descriptor_crc16 = test_descriptor_crc(&descriptor_copy);
    descriptor_index = ring->producer.write_seq % ring->producer.depth;
    ring->descriptors[descriptor_index] = descriptor_copy;
    ring->producer.write_seq = ring->producer.write_seq + 1u;
    ring->producer.enqueue_count = ring->producer.enqueue_count + 1u;
}

/**
 * @brief 测试 v2 ABI 尺寸。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_abi_sizes(void)
{
    CHECK(sizeof(put_shm_region_t) == PUT_SHM_REGION_SIZE);
    CHECK(sizeof(put_shm_descriptor_t) == PUT_SHM_DESCRIPTOR_SIZE);
    CHECK(sizeof(put_shm_ring_header_t) == PUT_SHM_RING_HEADER_SIZE);
    CHECK(sizeof(put_shm_ring_producer_t) == PUT_SHM_RING_PRODUCER_SIZE);
    CHECK(sizeof(put_shm_ring_consumer_t) == PUT_SHM_RING_CONSUMER_SIZE);
    CHECK(sizeof(anymsg_header_t) == ANYMSG_HEADER_SIZE);
    return 0;
}

/**
 * @brief 测试 region format 后的 v2 ABI 字段。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_format_region(void)
{
    unified_error_t result; /**< format 返回值。 */

    result = rtos_shm_ipc_format_region(&g_region, 11u, 22u);
    CHECK(result == UNIFIED_OK);
    CHECK(g_region.header.magic == PUT_SHM_REGION_MAGIC);
    CHECK(g_region.header.version == PUT_SHM_IPC_VERSION);
    CHECK(g_region.header.region_size == PUT_SHM_REGION_SIZE);
    CHECK(g_region.header.frame_pool_offset == (uint32_t)offsetof(put_shm_region_t, frame_pool));
    CHECK(g_region.header.rx_rings_offset == (uint32_t)offsetof(put_shm_region_t, rx_rings));
    CHECK(g_region.header.tx_rings_offset == (uint32_t)offsetof(put_shm_region_t, tx_rings));
    CHECK(g_region.header.rx_pending_offset ==
          (uint32_t)offsetof(put_shm_region_t, rx_pending_bitmap));
    CHECK(g_region.header.tx_pending_offset ==
          (uint32_t)offsetof(put_shm_region_t, tx_pending_bitmap));
    CHECK(g_region.header.reclaim_ring_offset == (uint32_t)offsetof(put_shm_region_t, reclaim_ring));
    CHECK(g_region.header.frame_pool_block_count == PUT_SHM_FRAME_POOL_BLOCK_COUNT);
    CHECK(g_region.header.frame_pool_block_size == PUT_SHM_FRAME_POOL_BLOCK_SIZE);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].header.ring_kind ==
          (uint8_t)PUT_SHM_RING_KIND_RX);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_RS485].header.ring_kind ==
          (uint8_t)PUT_SHM_RING_KIND_TX);
    CHECK(g_region.reclaim_ring.header.ring_kind == (uint8_t)PUT_SHM_RING_KIND_RECLAIM);
    CHECK(g_region.rx_pending_bitmap.bits == 0u);
    CHECK(g_region.tx_pending_bitmap.bits == 0u);
    CHECK(g_region.reclaim_pending.bits == 0u);
    return 0;
}

/**
 * @brief 测试 attach 参数和 header 校验。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_attach_validation(void)
{
    mock_platform_context_t context; /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;     /**< mock 平台操作集合。 */
    rtos_shm_ipc_t ipc;              /**< IPC 上下文。 */

    setup_region(&context, &ops);
    CHECK(rtos_shm_ipc_attach(0, &g_region, &ops) == UNIFIED_ERR_NULL);
    CHECK(rtos_shm_ipc_attach(&ipc, 0, &ops) == UNIFIED_ERR_NULL);
    CHECK(rtos_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_OK);

    g_region.header.magic = 0u;
    CHECK(rtos_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_ERR_PROTOCOL_HEADER);

    (void)rtos_shm_ipc_format_region(&g_region, 11u, 22u);
    g_region.header.version = 0u;
    CHECK(rtos_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_ERR_PROTOCOL_HEADER);

    (void)rtos_shm_ipc_format_region(&g_region, 11u, 22u);
    g_region.rx_rings[PUT_SHM_INTERFACE_CAN].header.descriptor_size = 0u;
    CHECK(rtos_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_ERR_LENGTH);
    return 0;
}

/**
 * @brief 测试空 RX ring 出队。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_empty_dequeue(void)
{
    mock_platform_context_t context; /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;     /**< mock 平台操作集合。 */
    rtos_shm_ipc_t ipc;              /**< IPC 上下文。 */
    put_shm_descriptor_t descriptor; /**< 输出 descriptor。 */

    setup_region(&context, &ops);
    CHECK(rtos_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_OK);
    CHECK(rtos_shm_ipc_dequeue_rx_descriptor(&ipc,
                                             PUT_SHM_INTERFACE_CAN,
                                             &descriptor) == UNIFIED_ERR_IPC_QUEUE_EMPTY);
    CHECK(context.invalidate_count > 0u);
    return 0;
}

/**
 * @brief 测试 RX descriptor 入队和出队保持元数据不变。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_rx_descriptor_enqueue_dequeue(void)
{
    mock_platform_context_t context;       /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;           /**< mock 平台操作集合。 */
    rtos_shm_ipc_t ipc;                    /**< IPC 上下文。 */
    put_shm_descriptor_t input_descriptor; /**< 输入 descriptor。 */
    put_shm_descriptor_t output_descriptor;/**< 输出 descriptor。 */

    setup_region(&context, &ops);
    input_descriptor = make_descriptor(0u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    CHECK(rtos_shm_descriptor_ring_enqueue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &input_descriptor,
                                           PUT_SHM_DIRECTION_LINUX_TO_RTOS,
                                           &ops) == UNIFIED_OK);
    CHECK(g_region.rx_pending_bitmap.bits == (1u << PUT_SHM_INTERFACE_CAN));
    CHECK(context.notify_count == 1u);
    CHECK(context.last_direction == PUT_SHM_DIRECTION_LINUX_TO_RTOS);

    CHECK(rtos_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_OK);
    CHECK(rtos_shm_ipc_dequeue_rx_descriptor(&ipc,
                                             PUT_SHM_INTERFACE_CAN,
                                             &output_descriptor) == UNIFIED_OK);
    CHECK(output_descriptor.frame_id == input_descriptor.frame_id);
    CHECK(output_descriptor.frame_offset == input_descriptor.frame_offset);
    CHECK(output_descriptor.frame_length == input_descriptor.frame_length);
    CHECK(output_descriptor.source_interface == input_descriptor.source_interface);
    CHECK(output_descriptor.target_interface == input_descriptor.target_interface);
    CHECK(output_descriptor.type == input_descriptor.type);
    CHECK(output_descriptor.priority == input_descriptor.priority);
    CHECK(output_descriptor.ttl == input_descriptor.ttl);
    CHECK(output_descriptor.epoch == input_descriptor.epoch);
    CHECK(g_region.rx_pending_bitmap.bits == 0u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.dequeue_count == 1u);
    return 0;
}

/**
 * @brief 测试 TX descriptor 入队设置 pending 且只在 empty->non-empty 时通知。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_tx_descriptor_enqueue_pending(void)
{
    mock_platform_context_t context;        /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;            /**< mock 平台操作集合。 */
    rtos_shm_ipc_t ipc;                     /**< IPC 上下文。 */
    put_shm_descriptor_t first_descriptor;  /**< 第一个 TX descriptor。 */
    put_shm_descriptor_t second_descriptor; /**< 第二个 TX descriptor。 */

    setup_region(&context, &ops);
    first_descriptor = make_descriptor(1u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_WIFI);
    second_descriptor = make_descriptor(2u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_WIFI);
    CHECK(rtos_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_OK);

    CHECK(rtos_shm_ipc_enqueue_tx_descriptor(&ipc,
                                             PUT_SHM_INTERFACE_WIFI,
                                             &first_descriptor) == UNIFIED_OK);
    CHECK(g_region.tx_pending_bitmap.bits == (1u << PUT_SHM_INTERFACE_WIFI));
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_WIFI].producer.write_seq == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_WIFI].producer.notify_count == 1u);
    CHECK(context.notify_count == 1u);
    CHECK(context.last_direction == PUT_SHM_DIRECTION_RTOS_TO_LINUX);

    CHECK(rtos_shm_ipc_enqueue_tx_descriptor(&ipc,
                                             PUT_SHM_INTERFACE_WIFI,
                                             &second_descriptor) == UNIFIED_OK);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_WIFI].producer.write_seq == 2u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_WIFI].producer.notify_count == 1u);
    CHECK(context.notify_count == 1u);
    return 0;
}

/**
 * @brief 测试 ring 满时不覆盖旧 descriptor。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_ring_full_preserves_oldest(void)
{
    mock_platform_context_t context;       /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;           /**< mock 平台操作集合。 */
    uint32_t index;                        /**< ring 填充循环索引。 */
    put_shm_descriptor_t input_descriptor; /**< 输入 descriptor。 */
    put_shm_descriptor_t output_descriptor;/**< 输出 descriptor。 */

    setup_region(&context, &ops);
    for (index = 0u; index < PUT_SHM_DESCRIPTOR_RING_DEPTH; ++index) {
        input_descriptor = make_descriptor(index,
                                           PUT_SHM_INTERFACE_CAN,
                                           PUT_SHM_INTERFACE_RS485);
        CHECK(rtos_shm_descriptor_ring_enqueue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                               &g_region.rx_pending_bitmap,
                                               &input_descriptor,
                                               PUT_SHM_DIRECTION_LINUX_TO_RTOS,
                                               &ops) == UNIFIED_OK);
    }

    input_descriptor = make_descriptor(PUT_SHM_DESCRIPTOR_RING_DEPTH,
                                       PUT_SHM_INTERFACE_CAN,
                                       PUT_SHM_INTERFACE_RS485);
    CHECK(rtos_shm_descriptor_ring_enqueue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &input_descriptor,
                                           PUT_SHM_DIRECTION_LINUX_TO_RTOS,
                                           &ops) == UNIFIED_ERR_IPC_QUEUE_FULL);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].producer.drop_count == 1u);

    CHECK(rtos_shm_descriptor_ring_dequeue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &output_descriptor,
                                           &ops) == UNIFIED_OK);
    CHECK(output_descriptor.frame_id == 0u);
    return 0;
}

/**
 * @brief 测试 descriptor CRC 错误会被消费。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_descriptor_crc_error_consumes_slot(void)
{
    mock_platform_context_t context;       /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;           /**< mock 平台操作集合。 */
    put_shm_descriptor_t input_descriptor; /**< 输入 descriptor。 */
    put_shm_descriptor_t output_descriptor;/**< 输出 descriptor。 */

    setup_region(&context, &ops);
    input_descriptor = make_descriptor(3u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    CHECK(rtos_shm_descriptor_ring_enqueue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &input_descriptor,
                                           PUT_SHM_DIRECTION_LINUX_TO_RTOS,
                                           &ops) == UNIFIED_OK);

    g_region.rx_rings[PUT_SHM_INTERFACE_CAN].descriptors[0].type ^= 0x01u;
    CHECK(rtos_shm_descriptor_ring_dequeue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &output_descriptor,
                                           &ops) == UNIFIED_ERR_CRC);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.read_seq == 1u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.crc_error_count == 1u);
    CHECK(rtos_shm_descriptor_ring_dequeue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &output_descriptor,
                                           &ops) == UNIFIED_ERR_IPC_QUEUE_EMPTY);
    return 0;
}

/**
 * @brief 测试 Frame Pool 边界校验。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_frame_bounds_validation(void)
{
    mock_platform_context_t context;      /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;          /**< mock 平台操作集合。 */
    rtos_shm_ipc_t ipc;                   /**< IPC 上下文。 */
    put_shm_descriptor_t descriptor;      /**< 测试 descriptor。 */
    const uint8_t *frame;                 /**< 输出 frame 指针。 */
    uint16_t frame_length;                /**< 输出 frame 长度。 */

    setup_region(&context, &ops);
    CHECK(rtos_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_OK);

    descriptor = make_descriptor(4u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    CHECK(rtos_shm_ipc_get_frame_const(&ipc, &descriptor, &frame, &frame_length) == UNIFIED_OK);
    CHECK(frame == g_region.frame_pool[4].bytes);
    CHECK(frame_length == ANYMSG_HEADER_SIZE);
    CHECK(context.last_invalidate_address == g_region.frame_pool[4].bytes);
    CHECK(context.last_invalidate_length == ANYMSG_HEADER_SIZE);

    descriptor.frame_id = PUT_SHM_FRAME_POOL_BLOCK_COUNT;
    CHECK(rtos_shm_ipc_get_frame_const(&ipc, &descriptor, &frame, &frame_length) ==
          UNIFIED_ERR_LENGTH);

    descriptor = make_descriptor(4u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    descriptor.frame_offset = 1u;
    CHECK(rtos_shm_ipc_get_frame_const(&ipc, &descriptor, &frame, &frame_length) ==
          UNIFIED_ERR_LENGTH);

    descriptor = make_descriptor(4u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    descriptor.frame_length = (uint16_t)(PUT_SHM_FRAME_POOL_BLOCK_SIZE + 1u);
    CHECK(rtos_shm_ipc_get_frame_const(&ipc, &descriptor, &frame, &frame_length) ==
          UNIFIED_ERR_LENGTH);
    return 0;
}

/**
 * @brief 测试 reclaim ring 能记录 Frame Pool 回收请求。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_reclaim_frame(void)
{
    mock_platform_context_t context;                 /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;                     /**< mock 平台操作集合。 */
    rtos_shm_ipc_t ipc;                              /**< IPC 上下文。 */
    put_shm_reclaim_descriptor_t *reclaim_descriptor;/**< 回收 descriptor 指针。 */

    setup_region(&context, &ops);
    CHECK(rtos_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_OK);
    CHECK(rtos_shm_ipc_reclaim_frame(&ipc,
                                     5u,
                                     PUT_SHM_RECLAIM_REASON_NO_ROUTE,
                                     PUT_SHM_INTERFACE_CAN,
                                     PUT_SHM_INTERFACE_RS485,
                                     11u,
                                     0x55u) == UNIFIED_OK);

    reclaim_descriptor = &g_region.reclaim_ring.descriptors[0];
    CHECK(g_region.reclaim_ring.producer.write_seq == 1u);
    CHECK(g_region.reclaim_pending.bits == 1u);
    CHECK(reclaim_descriptor->frame_id == 5u);
    CHECK(reclaim_descriptor->reason == (uint32_t)PUT_SHM_RECLAIM_REASON_NO_ROUTE);
    CHECK(reclaim_descriptor->source_interface == (uint8_t)PUT_SHM_INTERFACE_CAN);
    CHECK(reclaim_descriptor->target_interface == (uint8_t)PUT_SHM_INTERFACE_RS485);
    CHECK(reclaim_descriptor->epoch == 11u);
    CHECK(reclaim_descriptor->flags == 0x55u);
    CHECK(context.notify_count == 1u);
    CHECK(context.last_direction == PUT_SHM_DIRECTION_RTOS_TO_LINUX);
    return 0;
}

/**
 * @brief 测试 notify 失败后 descriptor 仍保持已入队状态。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_notify_failure_keeps_descriptor_enqueued(void)
{
    mock_platform_context_t context;       /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;           /**< mock 平台操作集合。 */
    rtos_shm_ipc_t ipc;                    /**< IPC 上下文。 */
    put_shm_descriptor_t input_descriptor; /**< 输入 descriptor。 */
    put_shm_descriptor_t output_descriptor;/**< 输出 descriptor。 */

    setup_region(&context, &ops);
    context.fail_notify = true;
    input_descriptor = make_descriptor(6u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_ETHERNET);
    CHECK(rtos_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_OK);

    CHECK(rtos_shm_ipc_enqueue_tx_descriptor(&ipc,
                                             PUT_SHM_INTERFACE_ETHERNET,
                                             &input_descriptor) == UNIFIED_OK);
    CHECK(context.notify_count == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_ETHERNET].producer.write_seq == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_ETHERNET].producer.notify_count == 0u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_ETHERNET].producer.notify_fail_count == 1u);

    CHECK(rtos_shm_descriptor_ring_dequeue(&g_region.tx_rings[PUT_SHM_INTERFACE_ETHERNET],
                                           &g_region.tx_pending_bitmap,
                                           &output_descriptor,
                                           &ops) == UNIFIED_OK);
    CHECK(output_descriptor.frame_id == input_descriptor.frame_id);
    CHECK(output_descriptor.target_interface == (uint8_t)PUT_SHM_INTERFACE_ETHERNET);
    return 0;
}

/**
 * @brief 测试消费最后一个 descriptor 时不会清掉并发新入队的 pending bit。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_pending_bit_survives_concurrent_enqueue(void)
{
    mock_platform_context_t context;        /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;            /**< mock 平台操作集合。 */
    put_shm_descriptor_t first_descriptor;  /**< 第一个 RX descriptor。 */
    put_shm_descriptor_t second_descriptor; /**< 并发注入的 RX descriptor。 */
    put_shm_descriptor_t output_descriptor; /**< 输出 descriptor。 */

    setup_region(&context, &ops);
    first_descriptor = make_descriptor(7u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    second_descriptor = make_descriptor(8u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    CHECK(rtos_shm_descriptor_ring_enqueue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &first_descriptor,
                                           PUT_SHM_DIRECTION_LINUX_TO_RTOS,
                                           &ops) == UNIFIED_OK);

    context.inject_enqueue_after_consumer_flush = true;
    context.inject_ring = &g_region.rx_rings[PUT_SHM_INTERFACE_CAN];
    context.inject_pending = &g_region.rx_pending_bitmap;
    context.inject_descriptor = second_descriptor;
    CHECK(rtos_shm_descriptor_ring_dequeue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &output_descriptor,
                                           &ops) == UNIFIED_OK);
    CHECK(output_descriptor.frame_id == first_descriptor.frame_id);
    CHECK(context.injected_enqueue_after_consumer_flush);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].producer.write_seq == 2u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.read_seq == 1u);
    CHECK(g_region.rx_pending_bitmap.bits == (1u << PUT_SHM_INTERFACE_CAN));

    context.inject_enqueue_after_consumer_flush = false;
    CHECK(rtos_shm_descriptor_ring_dequeue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &output_descriptor,
                                           &ops) == UNIFIED_OK);
    CHECK(output_descriptor.frame_id == second_descriptor.frame_id);
    CHECK(g_region.rx_pending_bitmap.bits == 0u);
    return 0;
}

/**
 * @brief 测试 latest_write_seq 读取后、clear 前并发入队会恢复 pending bit。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_pending_bit_restored_after_clear_window_enqueue(void)
{
    mock_platform_context_t context;        /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;            /**< mock 平台操作集合。 */
    put_shm_descriptor_t first_descriptor;  /**< 第一个 RX descriptor。 */
    put_shm_descriptor_t second_descriptor; /**< clear 前并发注入的 RX descriptor。 */
    put_shm_descriptor_t output_descriptor; /**< 输出 descriptor。 */
    uint32_t atomic_or_before_dequeue;      /**< 出队前原子 OR 调用次数。 */

    setup_region(&context, &ops);
    first_descriptor = make_descriptor(12u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    second_descriptor = make_descriptor(13u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    CHECK(rtos_shm_descriptor_ring_enqueue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &first_descriptor,
                                           PUT_SHM_DIRECTION_LINUX_TO_RTOS,
                                           &ops) == UNIFIED_OK);

    atomic_or_before_dequeue = context.atomic_or_count;
    context.inject_enqueue_during_pending_clear = true;
    context.inject_ring = &g_region.rx_rings[PUT_SHM_INTERFACE_CAN];
    context.inject_pending = &g_region.rx_pending_bitmap;
    context.inject_descriptor = second_descriptor;
    CHECK(rtos_shm_descriptor_ring_dequeue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &output_descriptor,
                                           &ops) == UNIFIED_OK);
    CHECK(output_descriptor.frame_id == first_descriptor.frame_id);
    CHECK(context.injected_enqueue_during_pending_clear);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].producer.write_seq == 2u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.read_seq == 1u);
    CHECK(context.atomic_or_count == (atomic_or_before_dequeue + 1u));
    CHECK(g_region.rx_pending_bitmap.bits == (1u << PUT_SHM_INTERFACE_CAN));

    context.inject_enqueue_during_pending_clear = false;
    CHECK(rtos_shm_descriptor_ring_dequeue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &output_descriptor,
                                           &ops) == UNIFIED_OK);
    CHECK(output_descriptor.frame_id == second_descriptor.frame_id);
    CHECK(g_region.rx_pending_bitmap.bits == 0u);
    return 0;
}

/**
 * @brief 测试清 CAN pending 时不会覆盖并发设置的 RS485 pending。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_pending_clear_preserves_other_interface_set(void)
{
    mock_platform_context_t context;       /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;           /**< mock 平台操作集合。 */
    put_shm_descriptor_t input_descriptor; /**< 输入 descriptor。 */
    put_shm_descriptor_t output_descriptor;/**< 输出 descriptor。 */

    setup_region(&context, &ops);
    input_descriptor = make_descriptor(9u, PUT_SHM_INTERFACE_CAN, PUT_SHM_INTERFACE_RS485);
    CHECK(rtos_shm_descriptor_ring_enqueue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &input_descriptor,
                                           PUT_SHM_DIRECTION_LINUX_TO_RTOS,
                                           &ops) == UNIFIED_OK);
    CHECK(g_region.rx_pending_bitmap.bits == (1u << PUT_SHM_INTERFACE_CAN));

    context.inject_rs485_set_during_can_clear = true;
    CHECK(rtos_shm_descriptor_ring_dequeue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &output_descriptor,
                                           &ops) == UNIFIED_OK);
    CHECK(output_descriptor.frame_id == input_descriptor.frame_id);
    CHECK(context.injected_rs485_set_during_can_clear);
    CHECK(context.atomic_and_count > 0u);
    CHECK(g_region.rx_pending_bitmap.bits == (1u << PUT_SHM_INTERFACE_RS485));
    return 0;
}

/**
 * @brief 测试 RX/TX ring 出队时校验 descriptor 接口归属。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_descriptor_ring_interface_mismatch_consumes_slot(void)
{
    mock_platform_context_t context;          /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;              /**< mock 平台操作集合。 */
    put_shm_descriptor_t rx_bad_descriptor;   /**< 放错 RX ring 的 descriptor。 */
    put_shm_descriptor_t tx_bad_descriptor;   /**< 放错 TX ring 的 descriptor。 */
    put_shm_descriptor_t output_descriptor;   /**< 输出 descriptor。 */

    setup_region(&context, &ops);

    rx_bad_descriptor = make_descriptor(10u,
                                        PUT_SHM_INTERFACE_RS485,
                                        PUT_SHM_INTERFACE_WIFI);
    publish_descriptor_direct(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN], &rx_bad_descriptor);
    g_region.rx_pending_bitmap.bits = (1u << PUT_SHM_INTERFACE_CAN);
    CHECK(rtos_shm_descriptor_ring_dequeue(&g_region.rx_rings[PUT_SHM_INTERFACE_CAN],
                                           &g_region.rx_pending_bitmap,
                                           &output_descriptor,
                                           &ops) == UNIFIED_ERR_INVALID_ARG);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.read_seq == 1u);
    CHECK(g_region.rx_rings[PUT_SHM_INTERFACE_CAN].consumer.format_error_count == 1u);
    CHECK(g_region.rx_pending_bitmap.bits == 0u);

    tx_bad_descriptor = make_descriptor(11u,
                                        PUT_SHM_INTERFACE_CAN,
                                        PUT_SHM_INTERFACE_ETHERNET);
    publish_descriptor_direct(&g_region.tx_rings[PUT_SHM_INTERFACE_WIFI], &tx_bad_descriptor);
    g_region.tx_pending_bitmap.bits = (1u << PUT_SHM_INTERFACE_WIFI);
    CHECK(rtos_shm_descriptor_ring_dequeue(&g_region.tx_rings[PUT_SHM_INTERFACE_WIFI],
                                           &g_region.tx_pending_bitmap,
                                           &output_descriptor,
                                           &ops) == UNIFIED_ERR_INVALID_ARG);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_WIFI].consumer.read_seq == 1u);
    CHECK(g_region.tx_rings[PUT_SHM_INTERFACE_WIFI].consumer.format_error_count == 1u);
    CHECK(g_region.tx_pending_bitmap.bits == 0u);
    return 0;
}

/**
 * @brief 测试程序入口。
 *
 * @return 0 表示所有测试通过，非 0 表示存在失败。
 */
int main(void)
{
    CHECK(test_abi_sizes() == 0);
    CHECK(test_format_region() == 0);
    CHECK(test_attach_validation() == 0);
    CHECK(test_empty_dequeue() == 0);
    CHECK(test_rx_descriptor_enqueue_dequeue() == 0);
    CHECK(test_tx_descriptor_enqueue_pending() == 0);
    CHECK(test_ring_full_preserves_oldest() == 0);
    CHECK(test_descriptor_crc_error_consumes_slot() == 0);
    CHECK(test_frame_bounds_validation() == 0);
    CHECK(test_reclaim_frame() == 0);
    CHECK(test_notify_failure_keeps_descriptor_enqueued() == 0);
    CHECK(test_pending_bit_survives_concurrent_enqueue() == 0);
    CHECK(test_pending_bit_restored_after_clear_window_enqueue() == 0);
    CHECK(test_pending_clear_preserves_other_interface_set() == 0);
    CHECK(test_descriptor_ring_interface_mismatch_consumes_slot() == 0);
    return 0;
}
