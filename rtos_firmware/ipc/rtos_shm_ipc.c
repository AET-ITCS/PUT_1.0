/**
 * @file rtos_shm_ipc.c
 * @brief rtos_firmware 共享内存 IPC v2 descriptor 搬运实现。
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
 * @brief 调用 32 位原子 OR 操作。
 *
 * @param ops 平台操作集合。
 * @param address 待原子更新的共享字段地址。
 * @param mask OR 操作使用的 bit mask。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t call_atomic_or_u32(const rtos_shm_platform_ops_t *ops,
                                          volatile uint32_t *address,
                                          uint32_t mask)
{
    if ((ops == 0) || (ops->atomic_or_u32 == 0)) {
        /* pending bitmap 必须使用原子 bit 操作，缺失时禁止继续。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    return ops->atomic_or_u32(address, mask, ops->user_context);
}

/**
 * @brief 调用 32 位原子 AND 操作。
 *
 * @param ops 平台操作集合。
 * @param address 待原子更新的共享字段地址。
 * @param mask AND 操作使用的 bit mask。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t call_atomic_and_u32(const rtos_shm_platform_ops_t *ops,
                                           volatile uint32_t *address,
                                           uint32_t mask)
{
    if ((ops == 0) || (ops->atomic_and_u32 == 0)) {
        /* pending bitmap 必须使用原子 bit 操作，缺失时禁止继续。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    return ops->atomic_and_u32(address, mask, ops->user_context);
}

/**
 * @brief 调用 32 位原子 ADD 操作。
 *
 * @param ops 平台操作集合。
 * @param address 待原子累加的共享字段地址。
 * @param value 需要累加的数值。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t call_atomic_add_u32(const rtos_shm_platform_ops_t *ops,
                                           volatile uint32_t *address,
                                           uint32_t value)
{
    if ((ops == 0) || (ops->atomic_add_u32 == 0)) {
        /* pending 诊断计数与 bit 同 cache line，也必须避免普通 RMW。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    return ops->atomic_add_u32(address, value, ops->user_context);
}

/**
 * @brief 调用 memory barrier 操作。
 *
 * @param ops 平台操作集合。
 */
static void call_memory_barrier(const rtos_shm_platform_ops_t *ops)
{
    if ((ops != 0) && (ops->memory_barrier != 0)) {
        /* 有平台屏障时显式调用，保证 descriptor 写入先于 write_seq 发布。 */
        ops->memory_barrier(ops->user_context);
    }
}

/**
 * @brief 判断平台通知操作是否可用。
 *
 * @param ops 平台操作集合。
 * @return true 表示 notify 回调可调用，false 表示平台操作未就绪。
 */
static bool is_notify_ready(const rtos_shm_platform_ops_t *ops)
{
    if ((ops == 0) || (ops->notify == 0)) {
        /* notify 缺失属于发布前可发现的配置错误。 */
        return false;
    }

    return true;
}

/**
 * @brief 判断 pending 原子操作是否可用。
 *
 * @param ops 平台操作集合。
 * @return true 表示 pending 原子操作完整，false 表示平台操作未就绪。
 */
static bool are_pending_ops_ready(const rtos_shm_platform_ops_t *ops)
{
    if ((ops == 0) ||
        (ops->atomic_or_u32 == 0) ||
        (ops->atomic_and_u32 == 0) ||
        (ops->atomic_add_u32 == 0)) {
        /* pending bitmap 是跨接口共享字段，不能退化为普通读改写。 */
        return false;
    }

    return true;
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

    if (!is_notify_ready(ops)) {
        /* 正常入队路径会在发布前检查 notify，这里保留防御式保护。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    notify_result = ops->notify(direction, ops->user_context);
    if (notify_result != UNIFIED_OK) {
        /* doorbell 失败只说明唤醒失败，不说明 descriptor 未写入。 */
        return UNIFIED_ERR_IPC_NOTIFY_FAILED;
    }

    return UNIFIED_OK;
}

/**
 * @brief 判断物理接口 ID 是否有效。
 *
 * @param interface_id 物理接口 ID。
 * @return true 表示有效，false 表示无效。
 */
static bool is_interface_valid(uint8_t interface_id)
{
    if (interface_id >= PUT_SHM_INTERFACE_COUNT) {
        /* 共享内存 v2 只冻结 6 类物理接口。 */
        return false;
    }

    return true;
}

/**
 * @brief 计算 descriptor CRC。
 *
 * @param descriptor descriptor 指针。
 * @return CRC-16/CCITT-FALSE 结果。
 */
static uint16_t calculate_descriptor_crc(const put_shm_descriptor_t *descriptor)
{
    size_t crc_length; /**< descriptor CRC 覆盖长度。 */

    crc_length = offsetof(put_shm_descriptor_t, descriptor_crc16);
    return unified_crc16_ccitt_false((const uint8_t *)descriptor, crc_length);
}

/**
 * @brief 计算 reclaim descriptor CRC。
 *
 * @param descriptor reclaim descriptor 指针。
 * @return CRC-16/CCITT-FALSE 结果。
 */
static uint16_t calculate_reclaim_crc(const put_shm_reclaim_descriptor_t *descriptor)
{
    size_t crc_length; /**< reclaim descriptor CRC 覆盖长度。 */

    crc_length = offsetof(put_shm_reclaim_descriptor_t, descriptor_crc16);
    return unified_crc16_ccitt_false((const uint8_t *)descriptor, crc_length);
}

/**
 * @brief 校验 descriptor 的 Frame Pool 边界和接口字段。
 *
 * @param descriptor descriptor 指针。
 * @return UNIFIED_OK 表示合法，否则返回公共错误码。
 */
static unified_error_t validate_descriptor_bounds(const put_shm_descriptor_t *descriptor)
{
    uint32_t expected_offset; /**< frame_id 对应的 Frame Pool block 起始偏移。 */

    if (descriptor == 0) {
        /* descriptor 为空时不能读取元数据。 */
        return UNIFIED_ERR_NULL;
    }

    if (!is_interface_valid(descriptor->source_interface) ||
        !is_interface_valid(descriptor->target_interface)) {
        /* descriptor 中的接口字段必须落在 v2 六类接口范围内。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (descriptor->frame_id >= PUT_SHM_FRAME_POOL_BLOCK_COUNT) {
        /* frame_id 越界会访问 Frame Pool 外部内存。 */
        return UNIFIED_ERR_LENGTH;
    }

    expected_offset = descriptor->frame_id * PUT_SHM_FRAME_POOL_BLOCK_SIZE;
    if (descriptor->frame_offset != expected_offset) {
        /* v2 默认一个 frame_id 对应一个固定 512B block。 */
        return UNIFIED_ERR_LENGTH;
    }

    if ((descriptor->frame_length < ANYMSG_HEADER_SIZE) ||
        (descriptor->frame_length > PUT_SHM_FRAME_POOL_BLOCK_SIZE)) {
        /* 小核只处理完整 anyMSG，长度必须能容纳 40B header 且不能越过 block。 */
        return UNIFIED_ERR_LENGTH;
    }

    return UNIFIED_OK;
}

/**
 * @brief 按 ring 类型校验 descriptor 的接口一致性。
 *
 * @param ring descriptor ring 指针。
 * @param descriptor descriptor 指针。
 * @return UNIFIED_OK 表示一致，否则返回公共错误码。
 */
static unified_error_t validate_descriptor_ring_interface(
    const put_shm_descriptor_ring_t *ring,
    const put_shm_descriptor_t *descriptor)
{
    if ((ring == 0) || (descriptor == 0)) {
        /* ring 或 descriptor 为空时不能校验接口归属。 */
        return UNIFIED_ERR_NULL;
    }

    if (ring->header.ring_kind == (uint8_t)PUT_SHM_RING_KIND_RX) {
        if (descriptor->source_interface != ring->header.interface_id) {
            /* RX ring 必须只承载来自本物理接口的 descriptor。 */
            return UNIFIED_ERR_INVALID_ARG;
        }

        return UNIFIED_OK;
    }

    if (ring->header.ring_kind == (uint8_t)PUT_SHM_RING_KIND_TX) {
        if (descriptor->target_interface != ring->header.interface_id) {
            /* TX ring 必须只承载发往本物理接口的 descriptor。 */
            return UNIFIED_ERR_INVALID_ARG;
        }

        return UNIFIED_OK;
    }

    /* 通用 descriptor 出队 helper 不接受 reclaim ring。 */
    return UNIFIED_ERR_INVALID_ARG;
}

/**
 * @brief 获取 ring 对应的 pending bit。
 *
 * @param ring_header ring header 指针。
 * @param out_bit 输出 pending bit。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t pending_bit_from_ring(const put_shm_ring_header_t *ring_header,
                                             uint32_t *out_bit)
{
    if ((ring_header == 0) || (out_bit == 0)) {
        /* ring header 或输出指针为空时不能计算 pending bit。 */
        return UNIFIED_ERR_NULL;
    }

    if (ring_header->ring_kind == (uint8_t)PUT_SHM_RING_KIND_RECLAIM) {
        /* reclaim pending 独立 cache line 只使用 bit0。 */
        *out_bit = 1u;
        return UNIFIED_OK;
    }

    if (!is_interface_valid(ring_header->interface_id)) {
        /* RX/TX ring 的 pending bit 按接口 ID 映射。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    *out_bit = (uint32_t)(1u << ring_header->interface_id);
    return UNIFIED_OK;
}

/**
 * @brief 设置 pending bit。
 *
 * @param pending_line pending bitmap 控制行。
 * @param bit 待设置 bit。
 * @param ops 平台操作集合。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t set_pending_bit(put_shm_pending_line_t *pending_line,
                                       uint32_t bit,
                                       const rtos_shm_platform_ops_t *ops)
{
    unified_error_t op_result; /**< 平台操作结果。 */

    if (pending_line == 0) {
        /* pending bitmap 为空时不能发布 ring 非空状态。 */
        return UNIFIED_ERR_NULL;
    }

    op_result = call_atomic_or_u32(ops, &pending_line->bits, bit);
    if (op_result != UNIFIED_OK) {
        /* pending bit 跨接口共享，置位必须由平台原子操作完成。 */
        return op_result;
    }

    /* 诊断计数与 bits 同 cache line，不能用普通 RMW 后 flush 整行。 */
    return call_atomic_add_u32(ops, &pending_line->set_count, 1u);
}

/**
 * @brief 清除 pending bit。
 *
 * @param pending_line pending bitmap 控制行。
 * @param bit 待清除 bit。
 * @param ops 平台操作集合。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t clear_pending_bit(put_shm_pending_line_t *pending_line,
                                         uint32_t bit,
                                         const rtos_shm_platform_ops_t *ops)
{
    unified_error_t op_result; /**< 平台操作结果。 */

    if (pending_line == 0) {
        /* pending bitmap 为空时不能清除 ring 状态。 */
        return UNIFIED_ERR_NULL;
    }

    op_result = call_atomic_and_u32(ops, &pending_line->bits, ~bit);
    if (op_result != UNIFIED_OK) {
        /* pending bit 跨接口共享，清位必须由平台原子操作完成。 */
        return op_result;
    }

    /* 诊断计数与 bits 同 cache line，不能用普通 RMW 后 flush 整行。 */
    return call_atomic_add_u32(ops, &pending_line->clear_count, 1u);
}

/**
 * @brief 初始化 pending bitmap。
 *
 * @param pending_line pending bitmap 控制行。
 */
static void format_pending_line(put_shm_pending_line_t *pending_line)
{
    memset(pending_line, 0, sizeof(*pending_line));
}

/**
 * @brief 初始化 descriptor ring。
 *
 * @param ring descriptor ring 指针。
 * @param interface_id 物理接口 ID。
 * @param ring_kind ring 类型。
 * @param direction 通知方向。
 */
static void format_descriptor_ring(put_shm_descriptor_ring_t *ring,
                                   uint8_t interface_id,
                                   put_shm_ring_kind_t ring_kind,
                                   put_shm_direction_t direction)
{
    memset(ring, 0, sizeof(*ring));

    ring->header.magic = PUT_SHM_RING_MAGIC;
    ring->header.version = PUT_SHM_IPC_VERSION;
    ring->header.header_size = (uint16_t)sizeof(put_shm_ring_header_t);
    ring->header.depth = PUT_SHM_DESCRIPTOR_RING_DEPTH;
    ring->header.descriptor_size = PUT_SHM_DESCRIPTOR_SIZE;
    ring->header.direction = (uint8_t)direction;
    ring->header.interface_id = interface_id;
    ring->header.ring_kind = (uint8_t)ring_kind;

    ring->producer.write_seq = 0u;
    ring->producer.depth = PUT_SHM_DESCRIPTOR_RING_DEPTH;
    ring->producer.descriptor_size = PUT_SHM_DESCRIPTOR_SIZE;

    ring->consumer.read_seq = 0u;
}

/**
 * @brief 初始化 reclaim ring。
 *
 * @param ring reclaim ring 指针。
 */
static void format_reclaim_ring(put_shm_reclaim_ring_t *ring)
{
    memset(ring, 0, sizeof(*ring));

    ring->header.magic = PUT_SHM_RING_MAGIC;
    ring->header.version = PUT_SHM_IPC_VERSION;
    ring->header.header_size = (uint16_t)sizeof(put_shm_ring_header_t);
    ring->header.depth = PUT_SHM_RECLAIM_RING_DEPTH;
    ring->header.descriptor_size = PUT_SHM_RECLAIM_DESCRIPTOR_SIZE;
    ring->header.direction = (uint8_t)PUT_SHM_DIRECTION_RTOS_TO_LINUX;
    ring->header.interface_id = 0u;
    ring->header.ring_kind = (uint8_t)PUT_SHM_RING_KIND_RECLAIM;

    ring->producer.write_seq = 0u;
    ring->producer.depth = PUT_SHM_RECLAIM_RING_DEPTH;
    ring->producer.descriptor_size = PUT_SHM_RECLAIM_DESCRIPTOR_SIZE;

    ring->consumer.read_seq = 0u;
}

/**
 * @brief 校验 descriptor ring header 和控制行。
 *
 * @param ring descriptor ring 指针。
 * @param expected_kind 期望 ring 类型。
 * @param expected_interface 期望物理接口 ID。
 * @param expected_direction 期望通知方向。
 * @return UNIFIED_OK 表示合法，否则返回公共错误码。
 */
static unified_error_t validate_descriptor_ring(const put_shm_descriptor_ring_t *ring,
                                                put_shm_ring_kind_t expected_kind,
                                                uint8_t expected_interface,
                                                put_shm_direction_t expected_direction)
{
    if (ring == 0) {
        /* ring 指针为空时无法校验。 */
        return UNIFIED_ERR_NULL;
    }

    if ((expected_kind != PUT_SHM_RING_KIND_RX) &&
        (expected_kind != PUT_SHM_RING_KIND_TX)) {
        /* descriptor ring helper 只允许处理 RX/TX ring，不处理 reclaim ring。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    if ((ring->header.magic != PUT_SHM_RING_MAGIC) ||
        (ring->header.version != PUT_SHM_IPC_VERSION) ||
        (ring->header.header_size != (uint16_t)sizeof(put_shm_ring_header_t))) {
        /* ring header 基础字段不匹配，说明 region 未格式化或 ABI 不兼容。 */
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if ((ring->header.depth != PUT_SHM_DESCRIPTOR_RING_DEPTH) ||
        (ring->header.descriptor_size != PUT_SHM_DESCRIPTOR_SIZE) ||
        (ring->producer.depth != PUT_SHM_DESCRIPTOR_RING_DEPTH) ||
        (ring->producer.descriptor_size != PUT_SHM_DESCRIPTOR_SIZE)) {
        /* ring 深度或 descriptor 大小错误会破坏索引计算。 */
        return UNIFIED_ERR_LENGTH;
    }

    if ((ring->header.ring_kind != (uint8_t)expected_kind) ||
        (ring->header.interface_id != expected_interface) ||
        (ring->header.direction != (uint8_t)expected_direction)) {
        /* ring 类型、接口或方向不匹配时禁止读写。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

/**
 * @brief 校验 reclaim ring header 和控制行。
 *
 * @param ring reclaim ring 指针。
 * @return UNIFIED_OK 表示合法，否则返回公共错误码。
 */
static unified_error_t validate_reclaim_ring(const put_shm_reclaim_ring_t *ring)
{
    if (ring == 0) {
        /* ring 指针为空时无法校验。 */
        return UNIFIED_ERR_NULL;
    }

    if ((ring->header.magic != PUT_SHM_RING_MAGIC) ||
        (ring->header.version != PUT_SHM_IPC_VERSION) ||
        (ring->header.header_size != (uint16_t)sizeof(put_shm_ring_header_t))) {
        /* reclaim ring header 不匹配时拒绝继续写入。 */
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if ((ring->header.depth != PUT_SHM_RECLAIM_RING_DEPTH) ||
        (ring->header.descriptor_size != PUT_SHM_RECLAIM_DESCRIPTOR_SIZE) ||
        (ring->producer.depth != PUT_SHM_RECLAIM_RING_DEPTH) ||
        (ring->producer.descriptor_size != PUT_SHM_RECLAIM_DESCRIPTOR_SIZE)) {
        /* reclaim ring 深度或 descriptor 大小不匹配。 */
        return UNIFIED_ERR_LENGTH;
    }

    if ((ring->header.ring_kind != (uint8_t)PUT_SHM_RING_KIND_RECLAIM) ||
        (ring->header.direction != (uint8_t)PUT_SHM_DIRECTION_RTOS_TO_LINUX)) {
        /* reclaim ring 必须由 RTOS 写、Linux 读。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

/**
 * @brief 消费 descriptor ring 当前一个元素。
 *
 * @param ring descriptor ring 指针。
 * @param pending_line pending bitmap 控制行。
 * @param write_seq 已读取的生产者写序号。
 * @param ops 平台操作集合。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t consume_descriptor(put_shm_descriptor_ring_t *ring,
                                          put_shm_pending_line_t *pending_line,
                                          uint32_t write_seq,
                                          const rtos_shm_platform_ops_t *ops)
{
    unified_error_t op_result; /**< 平台操作结果。 */
    uint32_t pending_bit;      /**< 当前 ring 对应 pending bit。 */
    uint32_t latest_write_seq; /**< 重新读取的生产者写序号。 */

    ring->consumer.read_seq = ring->consumer.read_seq + 1u;
    op_result = call_cache_flush(ops, &ring->consumer, sizeof(ring->consumer));
    if (op_result != UNIFIED_OK) {
        /* read_seq 发布失败时返回底层错误。 */
        return op_result;
    }

    if (ring->consumer.read_seq == write_seq) {
        op_result = call_cache_invalidate(ops, &ring->producer, sizeof(ring->producer));
        if (op_result != UNIFIED_OK) {
            /* 清 pending 前必须重新读取生产者状态，避免丢掉并发入队。 */
            return op_result;
        }

        latest_write_seq = ring->producer.write_seq;
        if (ring->consumer.read_seq != latest_write_seq) {
            /* 生产者已并发写入新 descriptor，pending bit 必须保持置位。 */
            return UNIFIED_OK;
        }

        /* 确认本次消费后 ring 仍为空，清除 pending bit 作为兜底状态。 */
        op_result = pending_bit_from_ring(&ring->header, &pending_bit);
        if (op_result != UNIFIED_OK) {
            return op_result;
        }

        op_result = clear_pending_bit(pending_line, pending_bit, ops);
        if (op_result != UNIFIED_OK) {
            /* pending bit 清除失败时返回平台错误。 */
            return op_result;
        }

        call_memory_barrier(ops);

        op_result = call_cache_invalidate(ops, &ring->producer, sizeof(ring->producer));
        if (op_result != UNIFIED_OK) {
            /* 清 bit 后必须再次读取生产者状态，覆盖清前读到清 bit 之间的竞态窗口。 */
            return op_result;
        }

        latest_write_seq = ring->producer.write_seq;
        if (ring->consumer.read_seq != latest_write_seq) {
            /* 生产者在清 bit 窗口内写入了新 descriptor，必须把 pending bit 原子置回。 */
            return set_pending_bit(pending_line, pending_bit, ops);
        }

        return UNIFIED_OK;
    }

    return UNIFIED_OK;
}

/**
 * @brief 初始化共享内存 v2 region。
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
    uint32_t interface_index; /**< ring 初始化循环索引。 */

    if (region == 0) {
        /* region 为空时无法初始化共享内存 ABI。 */
        return UNIFIED_ERR_NULL;
    }

    memset(region, 0, sizeof(*region));

    region->header.magic = PUT_SHM_REGION_MAGIC;
    region->header.version = PUT_SHM_IPC_VERSION;
    region->header.header_size = (uint16_t)sizeof(put_shm_region_header_t);
    region->header.region_size = PUT_SHM_REGION_SIZE;
    region->header.frame_pool_offset = (uint32_t)offsetof(put_shm_region_t, frame_pool);
    region->header.frame_pool_block_count = PUT_SHM_FRAME_POOL_BLOCK_COUNT;
    region->header.frame_pool_block_size = PUT_SHM_FRAME_POOL_BLOCK_SIZE;
    region->header.rx_rings_offset = (uint32_t)offsetof(put_shm_region_t, rx_rings);
    region->header.tx_rings_offset = (uint32_t)offsetof(put_shm_region_t, tx_rings);
    region->header.rx_pending_offset = (uint32_t)offsetof(put_shm_region_t, rx_pending_bitmap);
    region->header.tx_pending_offset = (uint32_t)offsetof(put_shm_region_t, tx_pending_bitmap);
    region->header.reclaim_pending_offset = (uint32_t)offsetof(put_shm_region_t, reclaim_pending);
    region->header.reclaim_ring_offset = (uint32_t)offsetof(put_shm_region_t, reclaim_ring);
    region->header.linux_epoch = linux_epoch;
    region->header.rtos_epoch = rtos_epoch;

    for (interface_index = 0u; interface_index < PUT_SHM_INTERFACE_COUNT; ++interface_index) {
        /* 每个物理接口都有独立 RX ring 和 TX ring。 */
        format_descriptor_ring(&region->rx_rings[interface_index],
                               (uint8_t)interface_index,
                               PUT_SHM_RING_KIND_RX,
                               PUT_SHM_DIRECTION_LINUX_TO_RTOS);
        format_descriptor_ring(&region->tx_rings[interface_index],
                               (uint8_t)interface_index,
                               PUT_SHM_RING_KIND_TX,
                               PUT_SHM_DIRECTION_RTOS_TO_LINUX);
    }

    format_pending_line(&region->rx_pending_bitmap);
    format_pending_line(&region->tx_pending_bitmap);
    format_pending_line(&region->reclaim_pending);
    format_reclaim_ring(&region->reclaim_ring);
    return UNIFIED_OK;
}

/**
 * @brief 绑定并校验共享内存 IPC v2 region。
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
    uint32_t interface_index;                    /**< ring 校验循环索引。 */

    if ((ipc == 0) || (region == 0)) {
        /* IPC 上下文或 region 为空时无法建立绑定关系。 */
        return UNIFIED_ERR_NULL;
    }

    if ((region->header.magic != PUT_SHM_REGION_MAGIC) ||
        (region->header.version != PUT_SHM_IPC_VERSION) ||
        (region->header.header_size != (uint16_t)sizeof(put_shm_region_header_t)) ||
        (region->header.region_size != PUT_SHM_REGION_SIZE)) {
        /* region header 不匹配时说明共享内存未格式化或 ABI 不兼容。 */
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if ((region->header.frame_pool_offset != (uint32_t)offsetof(put_shm_region_t, frame_pool)) ||
        (region->header.rx_rings_offset != (uint32_t)offsetof(put_shm_region_t, rx_rings)) ||
        (region->header.tx_rings_offset != (uint32_t)offsetof(put_shm_region_t, tx_rings)) ||
        (region->header.rx_pending_offset !=
         (uint32_t)offsetof(put_shm_region_t, rx_pending_bitmap)) ||
        (region->header.tx_pending_offset !=
         (uint32_t)offsetof(put_shm_region_t, tx_pending_bitmap)) ||
        (region->header.reclaim_pending_offset !=
         (uint32_t)offsetof(put_shm_region_t, reclaim_pending)) ||
        (region->header.reclaim_ring_offset !=
         (uint32_t)offsetof(put_shm_region_t, reclaim_ring))) {
        /* offset 不一致会导致双方解析到不同共享内存对象。 */
        return UNIFIED_ERR_LENGTH;
    }

    if ((region->header.frame_pool_block_count != PUT_SHM_FRAME_POOL_BLOCK_COUNT) ||
        (region->header.frame_pool_block_size != PUT_SHM_FRAME_POOL_BLOCK_SIZE)) {
        /* Frame Pool 配置不一致时不能安全访问 anyMSG。 */
        return UNIFIED_ERR_LENGTH;
    }

    for (interface_index = 0u; interface_index < PUT_SHM_INTERFACE_COUNT; ++interface_index) {
        /* 逐个校验每个接口 RX/TX ring，避免局部 ABI 损坏。 */
        validate_result = validate_descriptor_ring(&region->rx_rings[interface_index],
                                                   PUT_SHM_RING_KIND_RX,
                                                   (uint8_t)interface_index,
                                                   PUT_SHM_DIRECTION_LINUX_TO_RTOS);
        if (validate_result != UNIFIED_OK) {
            return validate_result;
        }

        validate_result = validate_descriptor_ring(&region->tx_rings[interface_index],
                                                   PUT_SHM_RING_KIND_TX,
                                                   (uint8_t)interface_index,
                                                   PUT_SHM_DIRECTION_RTOS_TO_LINUX);
        if (validate_result != UNIFIED_OK) {
            return validate_result;
        }
    }

    validate_result = validate_reclaim_ring(&region->reclaim_ring);
    if (validate_result != UNIFIED_OK) {
        /* reclaim ring 损坏时无法闭合 Frame Pool 回收路径。 */
        return validate_result;
    }

    resolved_ops = resolve_ops(ops);
    if ((resolved_ops->cache_flush == 0) ||
        (resolved_ops->cache_invalidate == 0) ||
        (resolved_ops->memory_barrier == 0) ||
        (resolved_ops->notify == 0) ||
        !are_pending_ops_ready(resolved_ops)) {
        /* 平台操作不完整时，真实跨核同步不可靠。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    ipc->region = region;
    ipc->platform_ops = *resolved_ops;
    ipc->initialized = true;
    return UNIFIED_OK;
}

/**
 * @brief 从指定物理接口 RX ring 读取一个 descriptor。
 *
 * @param ipc IPC 上下文。
 * @param interface_id 物理接口 ID，取值见 put_shm_interface_t。
 * @param out_descriptor 输出 descriptor。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_dequeue_rx_descriptor(rtos_shm_ipc_t *ipc,
                                                   put_shm_interface_t interface_id,
                                                   put_shm_descriptor_t *out_descriptor)
{
    if ((ipc == 0) || (out_descriptor == 0)) {
        /* 参数为空时不能访问 IPC 上下文或输出 descriptor。 */
        return UNIFIED_ERR_NULL;
    }

    if (!ipc->initialized || (ipc->region == 0)) {
        /* attach 成功前禁止读共享内存。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    if (!is_interface_valid((uint8_t)interface_id)) {
        /* 物理接口 ID 必须落在 v2 六类接口范围内。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    return rtos_shm_descriptor_ring_dequeue(&ipc->region->rx_rings[(uint32_t)interface_id],
                                            &ipc->region->rx_pending_bitmap,
                                            out_descriptor,
                                            &ipc->platform_ops);
}

/**
 * @brief 向指定物理接口 TX ring 写入一个 descriptor。
 *
 * @param ipc IPC 上下文。
 * @param interface_id 目标物理接口 ID，取值见 put_shm_interface_t。
 * @param descriptor 待写入 descriptor。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_enqueue_tx_descriptor(rtos_shm_ipc_t *ipc,
                                                   put_shm_interface_t interface_id,
                                                   const put_shm_descriptor_t *descriptor)
{
    unified_error_t validate_result; /**< descriptor 校验结果。 */

    if ((ipc == 0) || (descriptor == 0)) {
        /* 参数为空时不能访问 IPC 上下文或输入 descriptor。 */
        return UNIFIED_ERR_NULL;
    }

    if (!ipc->initialized || (ipc->region == 0)) {
        /* attach 成功前禁止写共享内存。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    if (!is_interface_valid((uint8_t)interface_id)) {
        /* 目标接口必须是 v2 六类接口之一。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (descriptor->target_interface != (uint8_t)interface_id) {
        /* descriptor 元数据必须和目标 TX ring 保持一致。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    validate_result = validate_descriptor_bounds(descriptor);
    if (validate_result != UNIFIED_OK) {
        /* Frame Pool 边界错误会导致 Linux 出口层读取错误数据。 */
        return validate_result;
    }

    return rtos_shm_descriptor_ring_enqueue(&ipc->region->tx_rings[(uint32_t)interface_id],
                                            &ipc->region->tx_pending_bitmap,
                                            descriptor,
                                            PUT_SHM_DIRECTION_RTOS_TO_LINUX,
                                            &ipc->platform_ops);
}

/**
 * @brief 写入 Frame Pool 回收 descriptor。
 *
 * @param ipc IPC 上下文。
 * @param frame_id 需要 Linux 回收的 Frame Pool block ID。
 * @param reason 回收原因。
 * @param source_interface 原始来源物理接口。
 * @param target_interface 原始目标物理接口。
 * @param epoch Linux 启动纪元。
 * @param flags 附加标志。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_reclaim_frame(rtos_shm_ipc_t *ipc,
                                           uint32_t frame_id,
                                           put_shm_reclaim_reason_t reason,
                                           put_shm_interface_t source_interface,
                                           put_shm_interface_t target_interface,
                                           uint32_t epoch,
                                           uint32_t flags)
{
    put_shm_reclaim_descriptor_t descriptor; /**< 本次写入的回收 descriptor。 */
    const rtos_shm_platform_ops_t *ops;      /**< IPC 上下文中的平台操作集合。 */
    unified_error_t op_result;              /**< 平台操作结果。 */
    uint32_t write_seq;                     /**< 当前生产者写序号。 */
    uint32_t read_seq;                      /**< 当前消费者读序号。 */
    uint32_t descriptor_index;              /**< 本次写入的 descriptor 下标。 */
    bool was_empty;                         /**< 写入前 ring 是否为空。 */

    if (ipc == 0) {
        /* IPC 上下文为空时无法写 reclaim ring。 */
        return UNIFIED_ERR_NULL;
    }

    if (!ipc->initialized || (ipc->region == 0)) {
        /* attach 成功前禁止写共享内存。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    if ((frame_id >= PUT_SHM_FRAME_POOL_BLOCK_COUNT) ||
        !is_interface_valid((uint8_t)source_interface) ||
        !is_interface_valid((uint8_t)target_interface)) {
        /* 回收 descriptor 必须指向合法 Frame Pool block 和接口。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (validate_reclaim_ring(&ipc->region->reclaim_ring) != UNIFIED_OK) {
        /* reclaim ring ABI 损坏时不能继续写入。 */
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    ops = &ipc->platform_ops;
    if (!is_notify_ready(ops) || !are_pending_ops_ready(ops)) {
        /* notify 或 pending 原子操作配置错误在写 descriptor 前即可发现。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    op_result = call_cache_invalidate(ops,
                                      &ipc->region->reclaim_ring.consumer,
                                      sizeof(ipc->region->reclaim_ring.consumer));
    if (op_result != UNIFIED_OK) {
        /* 写入前需要读取 Linux 消费者 read_seq。 */
        return op_result;
    }

    write_seq = ipc->region->reclaim_ring.producer.write_seq;
    read_seq = ipc->region->reclaim_ring.consumer.read_seq;
    was_empty = (write_seq == read_seq);
    if ((uint32_t)(write_seq - read_seq) >= ipc->region->reclaim_ring.producer.depth) {
        /* reclaim ring 满时只能记录 drop，不覆盖旧回收请求。 */
        ipc->region->reclaim_ring.producer.drop_count =
            ipc->region->reclaim_ring.producer.drop_count + 1u;
        (void)call_cache_flush(ops,
                               &ipc->region->reclaim_ring.producer,
                               sizeof(ipc->region->reclaim_ring.producer));
        return UNIFIED_ERR_IPC_QUEUE_FULL;
    }

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.frame_id = frame_id;
    descriptor.reason = (uint32_t)reason;
    descriptor.source_interface = (uint8_t)source_interface;
    descriptor.target_interface = (uint8_t)target_interface;
    descriptor.epoch = epoch;
    descriptor.flags = flags;
    descriptor.descriptor_crc16 = calculate_reclaim_crc(&descriptor);

    descriptor_index = write_seq % ipc->region->reclaim_ring.producer.depth;
    ipc->region->reclaim_ring.descriptors[descriptor_index] = descriptor;
    op_result = call_cache_flush(ops,
                                 &ipc->region->reclaim_ring.descriptors[descriptor_index],
                                 sizeof(ipc->region->reclaim_ring.descriptors[descriptor_index]));
    if (op_result != UNIFIED_OK) {
        /* descriptor 未 flush 前不能发布 write_seq。 */
        return op_result;
    }

    call_memory_barrier(ops);

    ipc->region->reclaim_ring.producer.write_seq = write_seq + 1u;
    ipc->region->reclaim_ring.producer.enqueue_count =
        ipc->region->reclaim_ring.producer.enqueue_count + 1u;
    op_result = call_cache_flush(ops,
                                 &ipc->region->reclaim_ring.producer,
                                 sizeof(ipc->region->reclaim_ring.producer));
    if (op_result != UNIFIED_OK) {
        /* write_seq 发布失败时返回底层错误。 */
        return op_result;
    }

    op_result = set_pending_bit(&ipc->region->reclaim_pending, 1u, ops);
    if (op_result != UNIFIED_OK) {
        /* pending bit 发布失败时返回底层错误。 */
        return op_result;
    }

    if (was_empty) {
        /* 只有 empty -> non-empty 才触发 doorbell。 */
        op_result = call_notify(ops, PUT_SHM_DIRECTION_RTOS_TO_LINUX);
        if (op_result != UNIFIED_OK) {
            ipc->region->reclaim_ring.producer.notify_fail_count =
                ipc->region->reclaim_ring.producer.notify_fail_count + 1u;
            (void)call_cache_flush(ops,
                                   &ipc->region->reclaim_ring.producer,
                                   sizeof(ipc->region->reclaim_ring.producer));
            return UNIFIED_OK;
        }

        ipc->region->reclaim_ring.producer.notify_count =
            ipc->region->reclaim_ring.producer.notify_count + 1u;
        (void)call_cache_flush(ops,
                               &ipc->region->reclaim_ring.producer,
                               sizeof(ipc->region->reclaim_ring.producer));
    }

    return UNIFIED_OK;
}

/**
 * @brief 根据 descriptor 获取 Frame Pool 中的只读完整 anyMSG。
 *
 * @param ipc IPC 上下文。
 * @param descriptor descriptor 指针。
 * @param out_frame 输出完整 anyMSG 起始地址。
 * @param out_frame_length 输出完整 anyMSG 字节数。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_ipc_get_frame_const(const rtos_shm_ipc_t *ipc,
                                             const put_shm_descriptor_t *descriptor,
                                             const uint8_t **out_frame,
                                             uint16_t *out_frame_length)
{
    unified_error_t validate_result; /**< descriptor 边界校验结果。 */
    unified_error_t op_result;       /**< cache 同步操作结果。 */

    if ((ipc == 0) || (descriptor == 0) || (out_frame == 0) || (out_frame_length == 0)) {
        /* 任一参数为空时不能返回 Frame Pool 指针。 */
        return UNIFIED_ERR_NULL;
    }

    if (!ipc->initialized || (ipc->region == 0)) {
        /* attach 成功前禁止访问 Frame Pool。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    validate_result = validate_descriptor_bounds(descriptor);
    if (validate_result != UNIFIED_OK) {
        /* descriptor 指向 Frame Pool 外部时拒绝返回指针。 */
        return validate_result;
    }

    op_result = call_cache_invalidate(&ipc->platform_ops,
                                      ipc->region->frame_pool[descriptor->frame_id].bytes,
                                      descriptor->frame_length);
    if (op_result != UNIFIED_OK) {
        /* 返回 frame 指针前必须同步 Linux 写入的 Frame Pool 内容。 */
        return op_result;
    }

    *out_frame = ipc->region->frame_pool[descriptor->frame_id].bytes;
    *out_frame_length = descriptor->frame_length;
    return UNIFIED_OK;
}

/**
 * @brief 向 descriptor ring 写入一个 descriptor。
 *
 * @param ring descriptor ring 指针。
 * @param pending_line pending bitmap 控制行。
 * @param descriptor 待写入 descriptor。
 * @param notify_direction doorbell 通知方向。
 * @param ops 平台操作集合；NULL 时使用默认 no-op 平台操作。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_descriptor_ring_enqueue(put_shm_descriptor_ring_t *ring,
                                                 put_shm_pending_line_t *pending_line,
                                                 const put_shm_descriptor_t *descriptor,
                                                 put_shm_direction_t notify_direction,
                                                 const rtos_shm_platform_ops_t *ops)
{
    const rtos_shm_platform_ops_t *resolved_ops; /**< 解析后的平台操作集合。 */
    put_shm_descriptor_t descriptor_copy;        /**< 写入 ring 的 descriptor 副本。 */
    unified_error_t op_result;                   /**< 平台操作结果。 */
    uint32_t write_seq;                          /**< 当前生产者写序号。 */
    uint32_t read_seq;                           /**< 当前消费者读序号。 */
    uint32_t descriptor_index;                   /**< 本次写入的 descriptor 下标。 */
    uint32_t pending_bit;                        /**< 当前 ring 对应 pending bit。 */
    bool was_empty;                              /**< 写入前 ring 是否为空。 */

    if ((ring == 0) || (pending_line == 0) || (descriptor == 0)) {
        /* ring、pending 或 descriptor 为空时无法入队。 */
        return UNIFIED_ERR_NULL;
    }

    if (validate_descriptor_ring(ring,
                                 (put_shm_ring_kind_t)ring->header.ring_kind,
                                 ring->header.interface_id,
                                 (put_shm_direction_t)ring->header.direction) != UNIFIED_OK) {
        /* ring 自身 ABI 不合法时禁止发布新 descriptor。 */
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if (notify_direction != (put_shm_direction_t)ring->header.direction) {
        /* 调用方通知方向必须与 ring header 一致。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    op_result = validate_descriptor_bounds(descriptor);
    if (op_result != UNIFIED_OK) {
        /* descriptor 必须指向合法 Frame Pool block。 */
        return op_result;
    }

    op_result = validate_descriptor_ring_interface(ring, descriptor);
    if (op_result != UNIFIED_OK) {
        /* descriptor 元数据必须和目标 ring 保持一致。 */
        return op_result;
    }

    resolved_ops = resolve_ops(ops);
    if (!is_notify_ready(resolved_ops) || !are_pending_ops_ready(resolved_ops)) {
        /* notify 或 pending 原子操作配置错误在写 descriptor 前即可发现。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    op_result = pending_bit_from_ring(&ring->header, &pending_bit);
    if (op_result != UNIFIED_OK) {
        /* pending bit 计算失败说明 ring header 不合法。 */
        return op_result;
    }

    op_result = call_cache_invalidate(resolved_ops, &ring->consumer, sizeof(ring->consumer));
    if (op_result != UNIFIED_OK) {
        /* 生产者需要读取消费者 read_seq，读取前必须先 invalidate。 */
        return op_result;
    }

    write_seq = ring->producer.write_seq;
    read_seq = ring->consumer.read_seq;
    was_empty = (write_seq == read_seq);
    if ((uint32_t)(write_seq - read_seq) >= ring->producer.depth) {
        /* ring 满时丢弃最新 descriptor，不覆盖旧 descriptor。 */
        ring->producer.drop_count = ring->producer.drop_count + 1u;
        (void)call_cache_flush(resolved_ops, &ring->producer, sizeof(ring->producer));
        return UNIFIED_ERR_IPC_QUEUE_FULL;
    }

    descriptor_copy = *descriptor;
    descriptor_copy.descriptor_crc16 = calculate_descriptor_crc(&descriptor_copy);
    descriptor_index = write_seq % ring->producer.depth;
    ring->descriptors[descriptor_index] = descriptor_copy;

    op_result = call_cache_flush(resolved_ops,
                                 &ring->descriptors[descriptor_index],
                                 sizeof(ring->descriptors[descriptor_index]));
    if (op_result != UNIFIED_OK) {
        /* descriptor 未成功 flush 前不能发布 write_seq。 */
        return op_result;
    }

    call_memory_barrier(resolved_ops);

    ring->producer.write_seq = write_seq + 1u;
    ring->producer.enqueue_count = ring->producer.enqueue_count + 1u;
    op_result = call_cache_flush(resolved_ops, &ring->producer, sizeof(ring->producer));
    if (op_result != UNIFIED_OK) {
        /* write_seq 发布失败时返回底层错误。 */
        return op_result;
    }

    op_result = set_pending_bit(pending_line, pending_bit, resolved_ops);
    if (op_result != UNIFIED_OK) {
        /* pending bitmap 发布失败时返回底层错误。 */
        return op_result;
    }

    if (was_empty) {
        /* 只有 ring 从 empty 变为 non-empty 才触发 doorbell。 */
        op_result = call_notify(resolved_ops, notify_direction);
        if (op_result != UNIFIED_OK) {
            ring->producer.notify_fail_count = ring->producer.notify_fail_count + 1u;
            (void)call_cache_flush(resolved_ops, &ring->producer, sizeof(ring->producer));
            return UNIFIED_OK;
        }

        ring->producer.notify_count = ring->producer.notify_count + 1u;
        (void)call_cache_flush(resolved_ops, &ring->producer, sizeof(ring->producer));
    }

    return UNIFIED_OK;
}

/**
 * @brief 从 descriptor ring 读取一个 descriptor。
 *
 * @param ring descriptor ring 指针。
 * @param pending_line pending bitmap 控制行。
 * @param out_descriptor 输出 descriptor。
 * @param ops 平台操作集合；NULL 时使用默认 no-op 平台操作。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_shm_descriptor_ring_dequeue(put_shm_descriptor_ring_t *ring,
                                                 put_shm_pending_line_t *pending_line,
                                                 put_shm_descriptor_t *out_descriptor,
                                                 const rtos_shm_platform_ops_t *ops)
{
    const rtos_shm_platform_ops_t *resolved_ops; /**< 解析后的平台操作集合。 */
    unified_error_t op_result;                   /**< 平台操作结果。 */
    uint32_t write_seq;                          /**< 当前生产者写序号。 */
    uint32_t read_seq;                           /**< 当前消费者读序号。 */
    uint32_t descriptor_index;                   /**< 本次读取的 descriptor 下标。 */
    put_shm_descriptor_t *descriptor;            /**< 本次读取的 descriptor 指针。 */
    uint16_t actual_crc;                         /**< descriptor 实际 CRC。 */

    if ((ring == 0) || (pending_line == 0) || (out_descriptor == 0)) {
        /* ring、pending 或输出 descriptor 为空时无法出队。 */
        return UNIFIED_ERR_NULL;
    }

    if (validate_descriptor_ring(ring,
                                 (put_shm_ring_kind_t)ring->header.ring_kind,
                                 ring->header.interface_id,
                                 (put_shm_direction_t)ring->header.direction) != UNIFIED_OK) {
        /* ring 自身 ABI 不合法时禁止读取。 */
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    resolved_ops = resolve_ops(ops);
    if (!are_pending_ops_ready(resolved_ops)) {
        /* 出队可能需要清 pending bit，缺少原子操作时不能消费 descriptor。 */
        return UNIFIED_ERR_IPC_NOT_READY;
    }

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

    descriptor_index = read_seq % ring->header.depth;
    descriptor = &ring->descriptors[descriptor_index];

    op_result = call_cache_invalidate(resolved_ops, descriptor, sizeof(*descriptor));
    if (op_result != UNIFIED_OK) {
        /* descriptor 内容读取前必须先 invalidate。 */
        return op_result;
    }

    actual_crc = calculate_descriptor_crc(descriptor);
    if (actual_crc != descriptor->descriptor_crc16) {
        /* CRC 错误说明 descriptor 搬运损坏，消费坏 descriptor 后返回错误。 */
        ring->consumer.crc_error_count = ring->consumer.crc_error_count + 1u;
        (void)consume_descriptor(ring, pending_line, write_seq, resolved_ops);
        return UNIFIED_ERR_CRC;
    }

    op_result = validate_descriptor_bounds(descriptor);
    if (op_result != UNIFIED_OK) {
        /* descriptor 格式错误必须消费，避免 ring 因坏元素永久卡住。 */
        ring->consumer.format_error_count = ring->consumer.format_error_count + 1u;
        (void)consume_descriptor(ring, pending_line, write_seq, resolved_ops);
        return op_result;
    }

    op_result = validate_descriptor_ring_interface(ring, descriptor);
    if (op_result != UNIFIED_OK) {
        /* descriptor 放错接口 ring 时必须消费，避免上层误判物理入口。 */
        ring->consumer.format_error_count = ring->consumer.format_error_count + 1u;
        (void)consume_descriptor(ring, pending_line, write_seq, resolved_ops);
        return op_result;
    }

    *out_descriptor = *descriptor;
    ring->consumer.dequeue_count = ring->consumer.dequeue_count + 1u;
    op_result = consume_descriptor(ring, pending_line, write_seq, resolved_ops);
    if (op_result != UNIFIED_OK) {
        /* read_seq 发布失败时返回底层错误。 */
        return op_result;
    }

    return UNIFIED_OK;
}
