/**
 * @file rtos_shm_ipc_test.c
 * @brief rtos_firmware 共享内存 IPC host 单元测试。
 * @author Yukikaze
 */
#include "rtos_shm_ipc.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

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
    uint32_t flush_count;      /**< cache flush 调用次数。 */
    uint32_t invalidate_count; /**< cache invalidate 调用次数。 */
    uint32_t barrier_count;    /**< memory barrier 调用次数。 */
    uint32_t notify_count;     /**< notify 调用次数。 */
    bool fail_notify;          /**< 是否强制 notify 失败。 */
} mock_platform_context_t;

/** @brief 测试共享内存 region，避免占用栈空间。 */
static put_shm_region_t g_region;

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

    (void)address;
    (void)length;

    context = (mock_platform_context_t *)user_context;
    if (context != 0) {
        /* 记录 invalidate 调用次数，用于验证读路径和满队列判断。 */
        context->invalidate_count = context->invalidate_count + 1u;
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
        /* 记录 barrier 调用次数，用于验证写路径发布顺序。 */
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

    (void)direction;

    context = (mock_platform_context_t *)user_context;
    if (context != 0) {
        /* 记录 notify 调用次数，用于验证入队后触发了 doorbell 抽象。 */
        context->notify_count = context->notify_count + 1u;
        if (context->fail_notify) {
            /* 测试通知失败路径时返回明确错误。 */
            return UNIFIED_ERR_IPC_NOTIFY_FAILED;
        }
    }

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
 * @brief 测试 region format 后的 ABI 字段。
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
    CHECK(g_region.header.l2r_offset == (uint32_t)offsetof(put_shm_region_t, linux_to_rtos));
    CHECK(g_region.header.r2l_offset == (uint32_t)offsetof(put_shm_region_t, rtos_to_linux));
    CHECK(g_region.header.linux_epoch == 11u);
    CHECK(g_region.header.rtos_epoch == 22u);
    CHECK(g_region.linux_to_rtos.header.direction == (uint8_t)PUT_SHM_DIRECTION_LINUX_TO_RTOS);
    CHECK(g_region.rtos_to_linux.header.direction == (uint8_t)PUT_SHM_DIRECTION_RTOS_TO_LINUX);
    CHECK(g_region.linux_to_rtos.header.depth == PUT_SHM_L2R_DEPTH);
    CHECK(g_region.linux_to_rtos.header.slot_size == PUT_SHM_SLOT_SIZE);
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
    return 0;
}

/**
 * @brief 测试空 ring 出队。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_empty_dequeue(void)
{
    mock_platform_context_t context; /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;     /**< mock 平台操作集合。 */
    rtos_shm_message_t message;      /**< 输出消息。 */

    setup_region(&context, &ops);
    CHECK(rtos_shm_ring_dequeue(&g_region.linux_to_rtos, &message, &ops) ==
          UNIFIED_ERR_IPC_QUEUE_EMPTY);
    CHECK(context.invalidate_count > 0u);
    return 0;
}

/**
 * @brief 测试入队和出队保持 payload 不变。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_enqueue_dequeue(void)
{
    mock_platform_context_t context; /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;     /**< mock 平台操作集合。 */
    uint8_t payload[4];              /**< 测试 payload。 */
    rtos_shm_message_t message;      /**< 输出消息。 */

    setup_region(&context, &ops);
    payload[0] = 0x11u;
    payload[1] = 0x22u;
    payload[2] = 0x33u;
    payload[3] = 0x44u;

    CHECK(rtos_shm_ring_enqueue(&g_region.linux_to_rtos,
                                (uint16_t)PUT_SHM_MESSAGE_TYPE_HEARTBEAT,
                                payload,
                                sizeof(payload),
                                77u,
                                &ops) == UNIFIED_OK);
    CHECK(rtos_shm_ring_dequeue(&g_region.linux_to_rtos, &message, &ops) == UNIFIED_OK);
    CHECK(message.message_type == (uint16_t)PUT_SHM_MESSAGE_TYPE_HEARTBEAT);
    CHECK(message.sequence == 0u);
    CHECK(message.epoch == 77u);
    CHECK(message.payload_length == sizeof(payload));
    CHECK(memcmp(message.payload, payload, sizeof(payload)) == 0);
    CHECK(context.flush_count > 0u);
    CHECK(context.invalidate_count > 0u);
    CHECK(context.barrier_count > 0u);
    CHECK(context.notify_count > 0u);
    return 0;
}

/**
 * @brief 测试满 ring 不覆盖旧消息。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_ring_full_preserves_oldest(void)
{
    mock_platform_context_t context; /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;     /**< mock 平台操作集合。 */
    uint8_t payload;                 /**< 测试 payload 单字节。 */
    uint32_t index;                  /**< ring 填充循环索引。 */
    rtos_shm_message_t message;      /**< 输出消息。 */

    setup_region(&context, &ops);
    for (index = 0u; index < PUT_SHM_L2R_DEPTH; ++index) {
        payload = (uint8_t)index;
        CHECK(rtos_shm_ring_enqueue(&g_region.linux_to_rtos,
                                    (uint16_t)PUT_SHM_MESSAGE_TYPE_HEARTBEAT,
                                    &payload,
                                    sizeof(payload),
                                    1u,
                                    &ops) == UNIFIED_OK);
    }

    payload = 0xFFu;
    CHECK(rtos_shm_ring_enqueue(&g_region.linux_to_rtos,
                                (uint16_t)PUT_SHM_MESSAGE_TYPE_HEARTBEAT,
                                &payload,
                                sizeof(payload),
                                1u,
                                &ops) == UNIFIED_ERR_IPC_QUEUE_FULL);
    CHECK(g_region.linux_to_rtos.producer.drop_count == 1u);

    CHECK(rtos_shm_ring_dequeue(&g_region.linux_to_rtos, &message, &ops) == UNIFIED_OK);
    CHECK(message.payload_length == 1u);
    CHECK(message.payload[0] == 0u);
    return 0;
}

/**
 * @brief 测试 CRC 错误 slot 会被消费。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_crc_error_consumes_slot(void)
{
    mock_platform_context_t context; /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;     /**< mock 平台操作集合。 */
    uint8_t payload;                 /**< 测试 payload 单字节。 */
    rtos_shm_message_t message;      /**< 输出消息。 */

    setup_region(&context, &ops);
    payload = 0x5Au;
    CHECK(rtos_shm_ring_enqueue(&g_region.linux_to_rtos,
                                (uint16_t)PUT_SHM_MESSAGE_TYPE_HEARTBEAT,
                                &payload,
                                sizeof(payload),
                                1u,
                                &ops) == UNIFIED_OK);

    g_region.linux_to_rtos.slots[0].payload[0] ^= 0xFFu;
    CHECK(rtos_shm_ring_dequeue(&g_region.linux_to_rtos, &message, &ops) == UNIFIED_ERR_CRC);
    CHECK(g_region.linux_to_rtos.consumer.read_seq == 1u);
    CHECK(g_region.linux_to_rtos.consumer.crc_error_count == 1u);
    CHECK(rtos_shm_ring_dequeue(&g_region.linux_to_rtos, &message, &ops) ==
          UNIFIED_ERR_IPC_QUEUE_EMPTY);
    return 0;
}

/**
 * @brief 测试非法 payload length slot 会被消费。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_bad_length_consumes_slot(void)
{
    mock_platform_context_t context; /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;     /**< mock 平台操作集合。 */
    uint8_t payload;                 /**< 测试 payload 单字节。 */
    rtos_shm_message_t message;      /**< 输出消息。 */

    setup_region(&context, &ops);
    payload = 0x6Bu;
    CHECK(rtos_shm_ring_enqueue(&g_region.linux_to_rtos,
                                (uint16_t)PUT_SHM_MESSAGE_TYPE_HEARTBEAT,
                                &payload,
                                sizeof(payload),
                                1u,
                                &ops) == UNIFIED_OK);

    g_region.linux_to_rtos.slots[0].header.payload_length =
        (uint16_t)(PUT_SHM_PAYLOAD_MAX_LEN + 1u);
    CHECK(rtos_shm_ring_dequeue(&g_region.linux_to_rtos, &message, &ops) ==
          UNIFIED_ERR_PAYLOAD_LENGTH);
    CHECK(g_region.linux_to_rtos.consumer.read_seq == 1u);
    CHECK(g_region.linux_to_rtos.consumer.format_error_count == 1u);
    CHECK(rtos_shm_ring_dequeue(&g_region.linux_to_rtos, &message, &ops) ==
          UNIFIED_ERR_IPC_QUEUE_EMPTY);
    return 0;
}

/**
 * @brief 测试 IPC attach 后的 RTOS -> Linux 发送路径。
 *
 * @return 0 表示通过，非 0 表示失败。
 */
static int test_ipc_send_to_linux(void)
{
    mock_platform_context_t context; /**< mock 平台上下文。 */
    rtos_shm_platform_ops_t ops;     /**< mock 平台操作集合。 */
    rtos_shm_ipc_t ipc;              /**< IPC 上下文。 */
    uint8_t payload[2];              /**< 测试 payload。 */
    rtos_shm_message_t message;      /**< 输出消息。 */

    setup_region(&context, &ops);
    payload[0] = 0xAAu;
    payload[1] = 0x55u;
    CHECK(rtos_shm_ipc_attach(&ipc, &g_region, &ops) == UNIFIED_OK);
    CHECK(rtos_shm_ipc_send_to_linux(&ipc,
                                     (uint16_t)PUT_SHM_MESSAGE_TYPE_EVENT,
                                     payload,
                                     sizeof(payload),
                                     22u) == UNIFIED_OK);
    CHECK(rtos_shm_ring_dequeue(&g_region.rtos_to_linux, &message, &ops) == UNIFIED_OK);
    CHECK(message.message_type == (uint16_t)PUT_SHM_MESSAGE_TYPE_EVENT);
    CHECK(message.epoch == 22u);
    CHECK(message.payload_length == sizeof(payload));
    CHECK(memcmp(message.payload, payload, sizeof(payload)) == 0);
    return 0;
}

/**
 * @brief 测试程序入口。
 *
 * @return 0 表示所有测试通过，非 0 表示存在失败。
 */
int main(void)
{
    CHECK(test_format_region() == 0);
    CHECK(test_attach_validation() == 0);
    CHECK(test_empty_dequeue() == 0);
    CHECK(test_enqueue_dequeue() == 0);
    CHECK(test_ring_full_preserves_oldest() == 0);
    CHECK(test_crc_error_consumes_slot() == 0);
    CHECK(test_bad_length_consumes_slot() == 0);
    CHECK(test_ipc_send_to_linux() == 0);
    return 0;
}
