/**
 * @file rtos_shm_ipc.c
 * @brief rtos_firmware 共享内存 IPC ring 搬运实现。
 * @author Yukikaze
 */
#include "rtos_shm_ipc.h"

#include <stddef.h>
#include <string.h>

#include "crc16.h"

/**
 * @brief 解析平台操作集合。
 *
 * @param ops 调用方传入的平台操作集合。
 * @return 可直接使用的平台操作集合指针。
 */
static const rtos_shm_platform_ops_t *resolve_ops(const rtos_shm_platform_ops_t *ops)
{
    const rtos_shm_platform_ops_t *default_ops; /**< 默认平台操作集合。 */

    default_ops = rtos_shm_platform_default_ops();
    if (ops == 0) {
        /* 调用方未传入平台操作时使用默认 no-op 实现。 */
        return default_ops;
    }

    return ops;
}

/**
 * @brief 调用 cache flush 操作。
 *
 * @param ops 平台操作集合。
 * @param address 待 flush 地址。
 * @param length 待 flush 长度。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t call_cache_flush(const rtos_shm_platform_ops_t *ops,
                                        const void *address,
                                        size_t length)
{
    if ((ops == 0) || (ops->cache_flush == 0)) {
        /* 平台操作缺失说明 IPC 尚不可安全运行。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    return ops->cache_flush(address, length, ops->user_context);
}

/**
 * @brief 调用 cache invalidate 操作。
 *
 * @param ops 平台操作集合。
 * @param address 待 invalidate 地址。
 * @param length 待 invalidate 长度。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t call_cache_invalidate(const rtos_shm_platform_ops_t *ops,
                                             const void *address,
                                             size_t length)
{
    if ((ops == 0) || (ops->cache_invalidate == 0)) {
        /* 平台操作缺失说明 IPC 尚不可安全运行。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    return ops->cache_invalidate(address, length, ops->user_context);
}

/**
 * @brief 调用 memory barrier 操作。
 *
 * @param ops 平台操作集合。
 */
static void call_memory_barrier(const rtos_shm_platform_ops_t *ops)
{
    if ((ops != 0) && (ops->memory_barrier != 0)) {
        /* 有平台屏障时显式调用，保证 slot 写入先于 write_seq 发布。 */
        ops->memory_barrier(ops->user_context);
    }
}

/**
 * @brief 调用 doorbell/mailbox 通知操作。
 *
 * @param ops 平台操作集合。
 * @param direction 通知方向。
 * @return UNIFIED_OK 表示成功，否则返回 IPC 通知失败错误。
 */
static unified_error_t call_notify(const rtos_shm_platform_ops_t *ops,
                                   put_shm_direction_t direction)
{
    unified_error_t notify_result; /**< 平台通知结果。 */

    if ((ops == 0) || (ops->notify == 0)) {
        /* 平台通知缺失说明 IPC 尚不可安全运行。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    notify_result = ops->notify(direction, ops->user_context);
    if (notify_result != UNIFIED_OK) {
        /* ring 已发布但通知失败，调用方需要通过错误码感知 doorbell 异常。 */
        return UNIFIED_ERR_IPC_NOTIFY_FAILED;
    }

    return UNIFIED_OK;
}

/**
 * @brief 初始化单个 ring。
 *
 * @param ring ring 指针。
 * @param direction ring 方向。
 */
static void format_ring(put_shm_ring_t *ring, put_shm_direction_t direction)
{
    memset(ring, 0, sizeof(*ring));

    ring->header.magic = PUT_SHM_RING_MAGIC;
    ring->header.version = PUT_SHM_IPC_VERSION;
    ring->header.header_size = (uint16_t)sizeof(put_shm_ring_header_t);
    ring->header.depth = PUT_SHM_L2R_DEPTH;
    ring->header.slot_size = PUT_SHM_SLOT_SIZE;
    ring->header.direction = (uint8_t)direction;

    ring->producer.write_seq = 0u;
    ring->producer.depth = PUT_SHM_L2R_DEPTH;
    ring->producer.slot_size = PUT_SHM_SLOT_SIZE;
    ring->producer.drop_count = 0u;
    ring->producer.notify_count = 0u;

    ring->consumer.read_seq = 0u;
    ring->consumer.dequeue_count = 0u;
    ring->consumer.crc_error_count = 0u;
    ring->consumer.format_error_count = 0u;
}

/**
 * @brief 校验单个 ring header 和控制行。
 *
 * @param ring ring 指针。
 * @param expected_direction 期望方向。
 * @return UNIFIED_OK 表示 ring ABI 合法，否则返回公共错误码。
 */
static unified_error_t validate_ring(const put_shm_ring_t *ring,
                                     put_shm_direction_t expected_direction)
{
    if (ring == 0) {
        /* ring 指针为空时无法继续校验。 */
        return UNIFIED_ERR_NULL;
    }

    if ((ring->header.magic != PUT_SHM_RING_MAGIC) ||
        (ring->header.version != PUT_SHM_IPC_VERSION) ||
        (ring->header.header_size != (uint16_t)sizeof(put_shm_ring_header_t))) {
        /* ring header 基础字段不匹配，说明共享内存未初始化或 ABI 不兼容。 */
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if ((ring->header.depth != PUT_SHM_L2R_DEPTH) ||
        (ring->header.slot_size != PUT_SHM_SLOT_SIZE) ||
        (ring->producer.depth != PUT_SHM_L2R_DEPTH) ||
        (ring->producer.slot_size != PUT_SHM_SLOT_SIZE)) {
        /* ring 深度或 slot 大小不匹配会破坏索引计算。 */
        return UNIFIED_ERR_LENGTH;
    }

    if ((expected_direction != PUT_SHM_DIRECTION_LINUX_TO_RTOS) &&
        (expected_direction != PUT_SHM_DIRECTION_RTOS_TO_LINUX)) {
        /* ring 方向必须是 ABI 定义的两个有效方向之一。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (ring->header.direction != (uint8_t)expected_direction) {
        /* ring 方向不符时禁止继续读写，避免把两个方向的语义混用。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

/**
 * @brief 消费当前 ring 的一个 slot。
 *
 * @param ring ring 指针。
 * @param ops 平台操作集合。
 * @return UNIFIED_OK 表示 read_seq 已发布，否则返回公共错误码。
 */
static unified_error_t consume_one_slot(put_shm_ring_t *ring,
                                        const rtos_shm_platform_ops_t *ops)
{
    unified_error_t flush_result; /**< consumer cache line flush 结果。 */

    ring->consumer.read_seq = ring->consumer.read_seq + 1u;
    flush_result = call_cache_flush(ops, &ring->consumer, sizeof(ring->consumer));
    if (flush_result != UNIFIED_OK) {
        /* read_seq 发布失败时返回底层错误，调用方可决定是否重试。 */
        return flush_result;
    }

    return UNIFIED_OK;
}

/**
 * @brief 初始化共享内存 region 和两个 ring。
 *
 * @param region 共享内存区域指针。
 * @param linux_epoch Linux 启动纪元。
 * @param rtos_epoch RTOS 启动纪元。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_format_region(put_shm_region_t *region,
                                           uint32_t linux_epoch,
                                           uint32_t rtos_epoch)
{
    if (region == 0) {
        /* region 为空时无法初始化共享内存 ABI。 */
        return UNIFIED_ERR_NULL;
    }

    memset(region, 0, sizeof(*region));

    region->header.magic = PUT_SHM_REGION_MAGIC;
    region->header.version = PUT_SHM_IPC_VERSION;
    region->header.header_size = (uint16_t)sizeof(put_shm_region_header_t);
    region->header.region_size = PUT_SHM_REGION_SIZE;
    region->header.l2r_offset = (uint32_t)offsetof(put_shm_region_t, linux_to_rtos);
    region->header.r2l_offset = (uint32_t)offsetof(put_shm_region_t, rtos_to_linux);
    region->header.linux_epoch = linux_epoch;
    region->header.rtos_epoch = rtos_epoch;

    format_ring(&region->linux_to_rtos, PUT_SHM_DIRECTION_LINUX_TO_RTOS);
    format_ring(&region->rtos_to_linux, PUT_SHM_DIRECTION_RTOS_TO_LINUX);

    return UNIFIED_OK;
}

/**
 * @brief 绑定并校验共享内存 IPC region。
 *
 * @param ipc IPC 上下文。
 * @param region 共享内存区域指针。
 * @param ops 平台操作集合；NULL 时使用默认 no-op 平台操作。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_attach(rtos_shm_ipc_t *ipc,
                                    put_shm_region_t *region,
                                    const rtos_shm_platform_ops_t *ops)
{
    const rtos_shm_platform_ops_t *resolved_ops; /**< 解析后的平台操作集合。 */
    unified_error_t validate_result;             /**< 当前 ABI 校验结果。 */

    if ((ipc == 0) || (region == 0)) {
        /* IPC 上下文或 region 为空时无法建立绑定关系。 */
        return UNIFIED_ERR_NULL;
    }

    if ((region->header.magic != PUT_SHM_REGION_MAGIC) ||
        (region->header.version != PUT_SHM_IPC_VERSION) ||
        (region->header.header_size != (uint16_t)sizeof(put_shm_region_header_t)) ||
        (region->header.region_size != PUT_SHM_REGION_SIZE)) {
        /* region header 不匹配时说明共享内存还未格式化或 ABI 不兼容。 */
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if ((region->header.l2r_offset != (uint32_t)offsetof(put_shm_region_t, linux_to_rtos)) ||
        (region->header.r2l_offset != (uint32_t)offsetof(put_shm_region_t, rtos_to_linux))) {
        /* offset 不一致会导致双方解析到不同 ring。 */
        return UNIFIED_ERR_LENGTH;
    }

    validate_result = validate_ring(&region->linux_to_rtos, PUT_SHM_DIRECTION_LINUX_TO_RTOS);
    if (validate_result != UNIFIED_OK) {
        /* Linux -> RTOS ring ABI 不合法时拒绝 attach。 */
        return validate_result;
    }

    validate_result = validate_ring(&region->rtos_to_linux, PUT_SHM_DIRECTION_RTOS_TO_LINUX);
    if (validate_result != UNIFIED_OK) {
        /* RTOS -> Linux ring ABI 不合法时拒绝 attach。 */
        return validate_result;
    }

    resolved_ops = resolve_ops(ops);
    if ((resolved_ops->cache_flush == 0) ||
        (resolved_ops->cache_invalidate == 0) ||
        (resolved_ops->memory_barrier == 0) ||
        (resolved_ops->notify == 0)) {
        /* 平台操作不完整时，真实跨核同步不可靠。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    ipc->region = region;
    ipc->platform_ops = *resolved_ops;
    ipc->initialized = true;
    return UNIFIED_OK;
}

/**
 * @brief 从 Linux -> RTOS ring 接收一个 opaque payload。
 *
 * @param ipc IPC 上下文。
 * @param out_message 输出消息。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_receive_from_linux(rtos_shm_ipc_t *ipc,
                                                rtos_shm_message_t *out_message)
{
    if ((ipc == 0) || (out_message == 0)) {
        /* 参数为空时不能访问 IPC 上下文或输出消息。 */
        return UNIFIED_ERR_NULL;
    }

    if (!ipc->initialized || (ipc->region == 0)) {
        /* attach 成功前禁止读共享内存。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    return rtos_shm_ring_dequeue(&ipc->region->linux_to_rtos,
                                 out_message,
                                 &ipc->platform_ops);
}

/**
 * @brief 向 RTOS -> Linux ring 发送一个 opaque payload。
 *
 * @param ipc IPC 上下文。
 * @param message_type 消息类型。
 * @param payload payload 字节指针；payload_length 为 0 时可为 NULL。
 * @param payload_length payload 长度。
 * @param epoch 发送方启动纪元。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_send_to_linux(rtos_shm_ipc_t *ipc,
                                           uint16_t message_type,
                                           const uint8_t *payload,
                                           uint16_t payload_length,
                                           uint32_t epoch)
{
    if (ipc == 0) {
        /* IPC 上下文为空时无法发送。 */
        return UNIFIED_ERR_NULL;
    }

    if (!ipc->initialized || (ipc->region == 0)) {
        /* attach 成功前禁止写共享内存。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    return rtos_shm_ring_enqueue(&ipc->region->rtos_to_linux,
                                 message_type,
                                 payload,
                                 payload_length,
                                 epoch,
                                 &ipc->platform_ops);
}

/**
 * @brief 向指定 SPSC ring 写入一个 opaque payload。
 *
 * @param ring ring 指针。
 * @param message_type 消息类型。
 * @param payload payload 字节指针；payload_length 为 0 时可为 NULL。
 * @param payload_length payload 长度。
 * @param epoch 发送方启动纪元。
 * @param ops 平台操作集合；NULL 时使用默认 no-op 平台操作。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ring_enqueue(put_shm_ring_t *ring,
                                      uint16_t message_type,
                                      const uint8_t *payload,
                                      uint16_t payload_length,
                                      uint32_t epoch,
                                      const rtos_shm_platform_ops_t *ops)
{
    const rtos_shm_platform_ops_t *resolved_ops; /**< 解析后的平台操作集合。 */
    unified_error_t op_result;                   /**< 平台操作结果。 */
    uint32_t write_seq;                          /**< 当前生产者写序号。 */
    uint32_t read_seq;                           /**< 当前消费者读序号。 */
    uint32_t slot_index;                         /**< 本次写入的 slot 下标。 */
    put_shm_slot_t *slot;                        /**< 本次写入的 slot 指针。 */

    if (ring == 0) {
        /* ring 为空时无法入队。 */
        return UNIFIED_ERR_NULL;
    }

    if ((payload == 0) && (payload_length > 0u)) {
        /* 非空 payload 长度必须配套有效指针。 */
        return UNIFIED_ERR_NULL;
    }

    if (payload_length > PUT_SHM_PAYLOAD_MAX_LEN) {
        /* payload 超过 ABI 固定长度时拒绝写入。 */
        return UNIFIED_ERR_PAYLOAD_LENGTH;
    }

    if (validate_ring(ring, (put_shm_direction_t)ring->header.direction) != UNIFIED_OK) {
        /* ring 自身 ABI 不合法时禁止发布新消息。 */
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    resolved_ops = resolve_ops(ops);

    op_result = call_cache_invalidate(resolved_ops, &ring->consumer, sizeof(ring->consumer));
    if (op_result != UNIFIED_OK) {
        /* 生产者需要读取消费者 read_seq，读取前必须先 invalidate。 */
        return op_result;
    }

    write_seq = ring->producer.write_seq;
    read_seq = ring->consumer.read_seq;
    if ((uint32_t)(write_seq - read_seq) >= ring->producer.depth) {
        /* ring 满时丢弃最新消息，不覆盖旧 slot。 */
        ring->producer.drop_count = ring->producer.drop_count + 1u;
        (void)call_cache_flush(resolved_ops, &ring->producer, sizeof(ring->producer));
        return UNIFIED_ERR_IPC_QUEUE_FULL;
    }

    slot_index = write_seq % ring->producer.depth;
    slot = &ring->slots[slot_index];
    memset(slot, 0, sizeof(*slot));

    slot->header.magic = PUT_SHM_SLOT_MAGIC;
    slot->header.version = PUT_SHM_IPC_VERSION;
    slot->header.header_size = (uint16_t)sizeof(put_shm_slot_header_t);
    slot->header.sequence = write_seq;
    slot->header.epoch = epoch;
    slot->header.message_type = message_type;
    slot->header.payload_length = payload_length;
    slot->header.flags = 0u;

    if (payload_length > 0u) {
        /* IPC 层只搬运 opaque payload，不解释 message_type 对应的业务内容。 */
        memcpy(slot->payload, payload, payload_length);
    }
    slot->header.payload_crc16 = unified_crc16_ccitt_false(slot->payload, payload_length);

    op_result = call_cache_flush(resolved_ops, slot, sizeof(*slot));
    if (op_result != UNIFIED_OK) {
        /* slot 未成功 flush 前不能发布 write_seq。 */
        return op_result;
    }

    call_memory_barrier(resolved_ops);

    ring->producer.write_seq = write_seq + 1u;
    op_result = call_cache_flush(resolved_ops, &ring->producer, sizeof(ring->producer));
    if (op_result != UNIFIED_OK) {
        /* write_seq 发布失败时返回底层错误。 */
        return op_result;
    }

    op_result = call_notify(resolved_ops, (put_shm_direction_t)ring->header.direction);
    if (op_result != UNIFIED_OK) {
        /* 消息已入队但通知失败，返回明确 IPC notify 错误。 */
        return op_result;
    }

    ring->producer.notify_count = ring->producer.notify_count + 1u;
    (void)call_cache_flush(resolved_ops, &ring->producer, sizeof(ring->producer));
    return UNIFIED_OK;
}

/**
 * @brief 从指定 SPSC ring 读取一个 opaque payload。
 *
 * @param ring ring 指针。
 * @param out_message 输出消息。
 * @param ops 平台操作集合；NULL 时使用默认 no-op 平台操作。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ring_dequeue(put_shm_ring_t *ring,
                                      rtos_shm_message_t *out_message,
                                      const rtos_shm_platform_ops_t *ops)
{
    const rtos_shm_platform_ops_t *resolved_ops; /**< 解析后的平台操作集合。 */
    unified_error_t op_result;                   /**< 平台操作结果。 */
    uint32_t write_seq;                          /**< 当前生产者写序号。 */
    uint32_t read_seq;                           /**< 当前消费者读序号。 */
    uint32_t slot_index;                         /**< 本次读取的 slot 下标。 */
    put_shm_slot_t *slot;                        /**< 本次读取的 slot 指针。 */
    uint16_t actual_crc;                         /**< payload 实际计算 CRC。 */

    if ((ring == 0) || (out_message == 0)) {
        /* ring 或输出消息为空时无法出队。 */
        return UNIFIED_ERR_NULL;
    }

    if (validate_ring(ring, (put_shm_direction_t)ring->header.direction) != UNIFIED_OK) {
        /* ring 自身 ABI 不合法时禁止读取。 */
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    resolved_ops = resolve_ops(ops);

    op_result = call_cache_invalidate(resolved_ops, &ring->producer, sizeof(ring->producer));
    if (op_result != UNIFIED_OK) {
        /* 消费者需要读取生产者 write_seq，读取前必须先 invalidate。 */
        return op_result;
    }

    write_seq = ring->producer.write_seq;
    read_seq = ring->consumer.read_seq;
    if (write_seq == read_seq) {
        /* read_seq 与 write_seq 相等表示 ring 当前为空。 */
        return UNIFIED_ERR_IPC_QUEUE_EMPTY;
    }

    slot_index = read_seq % ring->header.depth;
    slot = &ring->slots[slot_index];

    op_result = call_cache_invalidate(resolved_ops, slot, sizeof(*slot));
    if (op_result != UNIFIED_OK) {
        /* slot 内容读取前必须先 invalidate。 */
        return op_result;
    }

    if ((slot->header.magic != PUT_SHM_SLOT_MAGIC) ||
        (slot->header.version != PUT_SHM_IPC_VERSION) ||
        (slot->header.header_size != (uint16_t)sizeof(put_shm_slot_header_t))) {
        /* 坏 slot 必须被消费，避免 ring 因单个格式错误永久卡住。 */
        ring->consumer.format_error_count = ring->consumer.format_error_count + 1u;
        (void)consume_one_slot(ring, resolved_ops);
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if (slot->header.payload_length > PUT_SHM_PAYLOAD_MAX_LEN) {
        /* 长度非法的 slot 同样消费掉，避免后续消息被阻塞。 */
        ring->consumer.format_error_count = ring->consumer.format_error_count + 1u;
        (void)consume_one_slot(ring, resolved_ops);
        return UNIFIED_ERR_PAYLOAD_LENGTH;
    }

    actual_crc = unified_crc16_ccitt_false(slot->payload, slot->header.payload_length);
    if (actual_crc != slot->header.payload_crc16) {
        /* CRC 错误说明 payload 搬运损坏，消费坏 slot 后返回错误。 */
        ring->consumer.crc_error_count = ring->consumer.crc_error_count + 1u;
        (void)consume_one_slot(ring, resolved_ops);
        return UNIFIED_ERR_CRC;
    }

    memset(out_message, 0, sizeof(*out_message));
    out_message->message_type = slot->header.message_type;
    out_message->sequence = slot->header.sequence;
    out_message->epoch = slot->header.epoch;
    out_message->payload_length = slot->header.payload_length;
    if (slot->header.payload_length > 0u) {
        /* IPC 层只复制 opaque payload，不解释具体消息类型。 */
        memcpy(out_message->payload, slot->payload, slot->header.payload_length);
    }

    ring->consumer.dequeue_count = ring->consumer.dequeue_count + 1u;
    op_result = consume_one_slot(ring, resolved_ops);
    if (op_result != UNIFIED_OK) {
        /* read_seq 发布失败时返回底层错误。 */
        return op_result;
    }

    return UNIFIED_OK;
}
