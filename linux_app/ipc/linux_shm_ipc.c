/**
 * @file linux_shm_ipc.c
 * @brief Linux 侧共享内存 IPC v2 Frame Pool 与 descriptor ring 实现。
 * @author Yukikaze
 */
#include "linux_shm_ipc.h"

#include <stddef.h>
#include <string.h>

#include "crc16.h"

/** @brief Frame Pool 分配 bitmap 的全集 mask。 */
#define LINUX_SHM_FRAME_BITMAP_FULL UINT64_MAX

/** @brief Linux IPC 上下文初始化 magic。 */
#define LINUX_SHM_IPC_CONTEXT_MAGIC 0x4C534849u

/**
 * @brief 判断 IPC 上下文是否已初始化。
 *
 * @param ipc IPC 上下文。
 * @return true 表示上下文已初始化。
 */
static bool is_context_initialized(const linux_shm_ipc_t *ipc);

/**
 * @brief 解析平台操作集合。
 *
 * @param ops 调用方传入的平台操作集合。
 * @return 可直接使用的平台操作集合。
 */
static const linux_shm_platform_ops_t *resolve_ops(const linux_shm_platform_ops_t *ops)
{
    if (ops == 0) {
        /* 未传入平台操作时使用 host/mock 默认实现。 */
        return linux_shm_platform_default_ops();
    }

    return ops;
}

/**
 * @brief 解析 format/attach 使用的平台操作集合。
 *
 * @param ipc IPC 上下文。
 * @param ops 调用方传入的平台操作集合。
 * @return 可直接使用的平台操作集合。
 */
static const linux_shm_platform_ops_t *resolve_binding_ops(const linux_shm_ipc_t *ipc,
                                                           const linux_shm_platform_ops_t *ops)
{
    if (ops != 0) {
        return ops;
    }

    if (is_context_initialized(ipc) && ipc->mapped) {
        /* map 后未显式传 ops 时沿用原映射后端，避免 unmap 后端被替换。 */
        return &ipc->ops;
    }

    return linux_shm_platform_default_ops();
}

/**
 * @brief 判断平台操作是否完整。
 *
 * @param ops 平台操作集合。
 * @param need_map 是否要求映射函数可用。
 * @return true 表示完整，false 表示缺失关键回调。
 */
static bool are_ops_ready(const linux_shm_platform_ops_t *ops, bool need_map)
{
    if (ops == 0) {
        /* 平台操作为空时不能跨核同步。 */
        return false;
    }

    if (need_map && ((ops->map_region == 0) || (ops->unmap_region == 0))) {
        /* 需要 map 时必须同时提供 map/unmap。 */
        return false;
    }

    if ((ops->cache_flush == 0) ||
        (ops->cache_invalidate == 0) ||
        (ops->memory_barrier == 0) ||
        (ops->notify == 0) ||
        (ops->atomic_or_u32 == 0) ||
        (ops->atomic_and_u32 == 0) ||
        (ops->atomic_add_u32 == 0)) {
        /* pending bitmap 和 ring 发布路径依赖这些同步回调。 */
        return false;
    }

    return true;
}

/**
 * @brief 判断 IPC 上下文是否已初始化。
 *
 * @param ipc IPC 上下文。
 * @return true 表示上下文已初始化。
 */
static bool is_context_initialized(const linux_shm_ipc_t *ipc)
{
    return (ipc != 0) && (ipc->context_magic == LINUX_SHM_IPC_CONTEXT_MAGIC);
}

/**
 * @brief 调用 cache flush。
 *
 * @param ipc IPC 上下文。
 * @param address 待 flush 地址。
 * @param length 待 flush 字节数。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t call_cache_flush(linux_shm_ipc_t *ipc,
                                        const void *address,
                                        size_t length)
{
    unified_error_t result; /**< 平台操作结果。 */

    result = ipc->ops.cache_flush(address, length, ipc->ops.user_context);
    if (result != UNIFIED_OK) {
        /* cache 同步失败需要进入完整性统计。 */
        ipc->stats.cache_sync_error_count++;
    }

    return result;
}

/**
 * @brief 调用 cache invalidate。
 *
 * @param ipc IPC 上下文。
 * @param address 待 invalidate 地址。
 * @param length 待 invalidate 字节数。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t call_cache_invalidate(linux_shm_ipc_t *ipc,
                                             const void *address,
                                             size_t length)
{
    unified_error_t result; /**< 平台操作结果。 */

    result = ipc->ops.cache_invalidate(address, length, ipc->ops.user_context);
    if (result != UNIFIED_OK) {
        /* cache 同步失败需要进入完整性统计。 */
        ipc->stats.cache_sync_error_count++;
    }

    return result;
}

/**
 * @brief 调用内存屏障。
 *
 * @param ipc IPC 上下文。
 */
static void call_memory_barrier(linux_shm_ipc_t *ipc)
{
    /* 平台操作在 attach/map 阶段已校验，这里直接调用。 */
    ipc->ops.memory_barrier(ipc->ops.user_context);
}

/**
 * @brief 计算 descriptor CRC。
 *
 * @param descriptor descriptor 指针。
 * @return CRC-16/CCITT-FALSE 结果。
 */
static uint16_t descriptor_crc(const put_shm_descriptor_t *descriptor)
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
static uint16_t reclaim_crc(const put_shm_reclaim_descriptor_t *descriptor)
{
    return unified_crc16_ccitt_false((const uint8_t *)descriptor,
                                     offsetof(put_shm_reclaim_descriptor_t, descriptor_crc16));
}

/**
 * @brief 判断接口 ID 是否有效。
 *
 * @param interface_id 接口 ID。
 * @return true 表示有效，false 表示无效。
 */
static bool is_interface_valid(uint8_t interface_id)
{
    return interface_id < PUT_SHM_INTERFACE_COUNT;
}

/**
 * @brief 获取接口 pending bit。
 *
 * @param interface_id 接口 ID。
 * @return pending bit mask。
 */
static uint32_t interface_pending_bit(uint8_t interface_id)
{
    return (uint32_t)(1u << interface_id);
}

/**
 * @brief 计算 ring 当前占用。
 *
 * @param write_seq 生产者写序号。
 * @param read_seq 消费者读序号。
 * @return ring 占用数量。
 */
static uint32_t ring_used(uint32_t write_seq, uint32_t read_seq)
{
    return (uint32_t)(write_seq - read_seq);
}

/**
 * @brief 初始化 pending 控制行。
 *
 * @param pending_line pending 控制行。
 */
static void format_pending_line(put_shm_pending_line_t *pending_line)
{
    memset(pending_line, 0, sizeof(*pending_line));
}

/**
 * @brief 初始化 descriptor ring。
 *
 * @param ring ring 指针。
 * @param interface_id 接口 ID。
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
    ring->producer.depth = PUT_SHM_DESCRIPTOR_RING_DEPTH;
    ring->producer.descriptor_size = PUT_SHM_DESCRIPTOR_SIZE;
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
    ring->producer.depth = PUT_SHM_RECLAIM_RING_DEPTH;
    ring->producer.descriptor_size = PUT_SHM_RECLAIM_DESCRIPTOR_SIZE;
}

/**
 * @brief 重置 Linux 本地 Frame Pool 管理状态。
 *
 * @param ipc IPC 上下文。
 */
static void reset_local_state(linux_shm_ipc_t *ipc)
{
    uint32_t interface_index; /**< 接口循环索引。 */

    ipc->allocation_bitmap = 0u;
    ipc->allocation_sequence = 0u;
    memset(ipc->frames, 0, sizeof(ipc->frames));
    memset(&ipc->stats, 0, sizeof(ipc->stats));
    ipc->stats.frame_pool.capacity = PUT_SHM_FRAME_POOL_BLOCK_COUNT;

    for (interface_index = 0u; interface_index < PUT_SHM_INTERFACE_COUNT; ++interface_index) {
        /* 默认所有接口可使用整个 Frame Pool，后续可按配置收紧。 */
        ipc->stats.interface_quota[interface_index].quota = LINUX_SHM_INTERFACE_QUOTA_DEFAULT;
    }
}

/**
 * @brief 保存 map 生命周期字段并校验映射 region。
 *
 * @param ipc IPC 上下文。
 * @param region 即将绑定的 region。
 * @param out_mapped 输出是否已映射。
 * @param out_mapping_context 输出映射私有上下文。
 * @param out_mapped_size 输出映射大小。
 * @return UNIFIED_OK 表示可继续绑定，否则返回公共错误码。
 */
static unified_error_t preserve_mapping_fields(const linux_shm_ipc_t *ipc,
                                               const put_shm_region_t *region,
                                               bool *out_mapped,
                                               void **out_mapping_context,
                                               size_t *out_mapped_size)
{
    bool current_mapped;                 /**< 当前上下文是否持有映射。 */
    put_shm_region_t *current_region;    /**< 当前上下文记录的 region。 */
    void *current_mapping_context;       /**< 当前上下文记录的映射私有数据。 */
    size_t current_mapped_size;          /**< 当前上下文记录的映射大小。 */

    if (!is_context_initialized(ipc)) {
        /* 未初始化或旧栈内容按新上下文处理，不继承任何映射生命周期字段。 */
        *out_mapped = false;
        *out_mapping_context = 0;
        *out_mapped_size = 0u;
        return UNIFIED_OK;
    }

    current_mapped = ipc->mapped;
    current_region = ipc->region;
    current_mapping_context = ipc->mapping_context;
    current_mapped_size = ipc->mapped_size;

    if (current_mapped && (current_region != region)) {
        /* 已 map 的上下文只能 format/attach 自己映射出来的 region，避免 unmap 错地址。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    *out_mapped = current_mapped;
    *out_mapping_context = current_mapping_context;
    *out_mapped_size = current_mapped_size;
    return UNIFIED_OK;
}

/**
 * @brief 校验 region header。
 *
 * @param region region 指针。
 * @return UNIFIED_OK 表示合法，否则返回公共错误码。
 */
static unified_error_t validate_region_header(const put_shm_region_t *region)
{
    if (region == 0) {
        /* region 为空时无法校验。 */
        return UNIFIED_ERR_NULL;
    }

    if ((region->header.magic != PUT_SHM_REGION_MAGIC) ||
        (region->header.version != PUT_SHM_IPC_VERSION) ||
        (region->header.header_size != (uint16_t)sizeof(put_shm_region_header_t)) ||
        (region->header.region_size != PUT_SHM_REGION_SIZE)) {
        /* region 基础 ABI 不匹配。 */
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if ((region->header.frame_pool_offset != (uint32_t)offsetof(put_shm_region_t, frame_pool)) ||
        (region->header.rx_rings_offset != (uint32_t)offsetof(put_shm_region_t, rx_rings)) ||
        (region->header.tx_rings_offset != (uint32_t)offsetof(put_shm_region_t, tx_rings)) ||
        (region->header.rx_pending_offset != (uint32_t)offsetof(put_shm_region_t, rx_pending_bitmap)) ||
        (region->header.tx_pending_offset != (uint32_t)offsetof(put_shm_region_t, tx_pending_bitmap)) ||
        (region->header.reclaim_pending_offset != (uint32_t)offsetof(put_shm_region_t, reclaim_pending)) ||
        (region->header.reclaim_ring_offset != (uint32_t)offsetof(put_shm_region_t, reclaim_ring))) {
        /* offset 不一致意味着双方解析布局不同。 */
        return UNIFIED_ERR_LENGTH;
    }

    if ((region->header.frame_pool_block_count != PUT_SHM_FRAME_POOL_BLOCK_COUNT) ||
        (region->header.frame_pool_block_size != PUT_SHM_FRAME_POOL_BLOCK_SIZE)) {
        /* Frame Pool 配置不一致。 */
        return UNIFIED_ERR_LENGTH;
    }

    return UNIFIED_OK;
}

/**
 * @brief 校验 descriptor ring。
 *
 * @param ring ring 指针。
 * @param expected_kind 期望类型。
 * @param expected_interface 期望接口。
 * @param expected_direction 期望方向。
 * @return UNIFIED_OK 表示合法，否则返回公共错误码。
 */
static unified_error_t validate_descriptor_ring(const put_shm_descriptor_ring_t *ring,
                                                put_shm_ring_kind_t expected_kind,
                                                uint8_t expected_interface,
                                                put_shm_direction_t expected_direction)
{
    if (ring == 0) {
        /* ring 指针为空。 */
        return UNIFIED_ERR_NULL;
    }

    if ((ring->header.magic != PUT_SHM_RING_MAGIC) ||
        (ring->header.version != PUT_SHM_IPC_VERSION) ||
        (ring->header.header_size != (uint16_t)sizeof(put_shm_ring_header_t))) {
        /* ring header 不匹配。 */
        return UNIFIED_ERR_PROTOCOL_HEADER;
    }

    if ((ring->header.depth != PUT_SHM_DESCRIPTOR_RING_DEPTH) ||
        (ring->header.descriptor_size != PUT_SHM_DESCRIPTOR_SIZE) ||
        (ring->producer.depth != PUT_SHM_DESCRIPTOR_RING_DEPTH) ||
        (ring->producer.descriptor_size != PUT_SHM_DESCRIPTOR_SIZE)) {
        /* ring 深度或 descriptor 大小不匹配。 */
        return UNIFIED_ERR_LENGTH;
    }

    if ((ring->header.ring_kind != (uint8_t)expected_kind) ||
        (ring->header.interface_id != expected_interface) ||
        (ring->header.direction != (uint8_t)expected_direction)) {
        /* ring 类型、接口或方向不匹配。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

/**
 * @brief 校验 reclaim ring。
 *
 * @param ring reclaim ring 指针。
 * @return UNIFIED_OK 表示合法，否则返回公共错误码。
 */
static unified_error_t validate_reclaim_ring(const put_shm_reclaim_ring_t *ring)
{
    if (ring == 0) {
        /* ring 指针为空。 */
        return UNIFIED_ERR_NULL;
    }

    if ((ring->header.magic != PUT_SHM_RING_MAGIC) ||
        (ring->header.version != PUT_SHM_IPC_VERSION) ||
        (ring->header.header_size != (uint16_t)sizeof(put_shm_ring_header_t))) {
        /* reclaim ring header 不匹配。 */
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
 * @brief 校验完整 region。
 *
 * @param region region 指针。
 * @return UNIFIED_OK 表示合法，否则返回公共错误码。
 */
static unified_error_t validate_region(const put_shm_region_t *region)
{
    unified_error_t result;  /**< 当前校验结果。 */
    uint32_t interface_index; /**< 接口循环索引。 */

    result = validate_region_header(region);
    if (result != UNIFIED_OK) {
        return result;
    }

    for (interface_index = 0u; interface_index < PUT_SHM_INTERFACE_COUNT; ++interface_index) {
        result = validate_descriptor_ring(&region->rx_rings[interface_index],
                                          PUT_SHM_RING_KIND_RX,
                                          (uint8_t)interface_index,
                                          PUT_SHM_DIRECTION_LINUX_TO_RTOS);
        if (result != UNIFIED_OK) {
            return result;
        }

        result = validate_descriptor_ring(&region->tx_rings[interface_index],
                                          PUT_SHM_RING_KIND_TX,
                                          (uint8_t)interface_index,
                                          PUT_SHM_DIRECTION_RTOS_TO_LINUX);
        if (result != UNIFIED_OK) {
            return result;
        }
    }

    return validate_reclaim_ring(&region->reclaim_ring);
}

/**
 * @brief 校验 descriptor Frame Pool 边界。
 *
 * @param descriptor descriptor 指针。
 * @return UNIFIED_OK 表示合法，否则返回公共错误码。
 */
static unified_error_t validate_descriptor_bounds(const put_shm_descriptor_t *descriptor)
{
    uint32_t expected_offset; /**< frame_id 对应的固定 offset。 */

    if (descriptor == 0) {
        return UNIFIED_ERR_NULL;
    }

    if (!is_interface_valid(descriptor->source_interface) ||
        !is_interface_valid(descriptor->target_interface)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (descriptor->frame_id >= PUT_SHM_FRAME_POOL_BLOCK_COUNT) {
        return UNIFIED_ERR_LENGTH;
    }

    expected_offset = descriptor->frame_id * PUT_SHM_FRAME_POOL_BLOCK_SIZE;
    if (descriptor->frame_offset != expected_offset) {
        return UNIFIED_ERR_LENGTH;
    }

    if ((descriptor->frame_length < ANYMSG_HEADER_SIZE) ||
        (descriptor->frame_length > PUT_SHM_FRAME_POOL_BLOCK_SIZE)) {
        return UNIFIED_ERR_LENGTH;
    }

    return UNIFIED_OK;
}

/**
 * @brief 设置 pending bit。
 *
 * @param ipc IPC 上下文。
 * @param pending_line pending 控制行。
 * @param bit pending bit。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t set_pending_bit(linux_shm_ipc_t *ipc,
                                       put_shm_pending_line_t *pending_line,
                                       uint32_t bit)
{
    unified_error_t result; /**< 平台操作结果。 */

    result = ipc->ops.atomic_or_u32(&pending_line->bits, bit, ipc->ops.user_context);
    if (result != UNIFIED_OK) {
        return result;
    }

    return ipc->ops.atomic_add_u32(&pending_line->set_count, 1u, ipc->ops.user_context);
}

/**
 * @brief 清除 pending bit。
 *
 * @param ipc IPC 上下文。
 * @param pending_line pending 控制行。
 * @param bit pending bit。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t clear_pending_bit(linux_shm_ipc_t *ipc,
                                         put_shm_pending_line_t *pending_line,
                                         uint32_t bit)
{
    unified_error_t result; /**< 平台操作结果。 */

    result = ipc->ops.atomic_and_u32(&pending_line->bits, ~bit, ipc->ops.user_context);
    if (result != UNIFIED_OK) {
        return result;
    }

    return ipc->ops.atomic_add_u32(&pending_line->clear_count, 1u, ipc->ops.user_context);
}

/**
 * @brief 消费 descriptor ring 的一个元素并维护 pending bit。
 *
 * @param ipc IPC 上下文。
 * @param ring descriptor ring。
 * @param pending_line pending 控制行。
 * @param write_seq 本次读取前看到的 write_seq。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t consume_descriptor_ring(linux_shm_ipc_t *ipc,
                                               put_shm_descriptor_ring_t *ring,
                                               put_shm_pending_line_t *pending_line,
                                               uint32_t write_seq)
{
    unified_error_t result;   /**< 平台操作结果。 */
    uint32_t pending_bit;     /**< 当前接口 pending bit。 */
    uint32_t latest_write_seq; /**< 复核时读取到的 write_seq。 */

    ring->consumer.read_seq = ring->consumer.read_seq + 1u;
    result = call_cache_flush(ipc, &ring->consumer, sizeof(ring->consumer));
    if (result != UNIFIED_OK) {
        return result;
    }

    if (ring->consumer.read_seq != write_seq) {
        /* ring 仍然非空时 pending bit 必须保持置位。 */
        return UNIFIED_OK;
    }

    result = call_cache_invalidate(ipc, &ring->producer, sizeof(ring->producer));
    if (result != UNIFIED_OK) {
        return result;
    }

    latest_write_seq = ring->producer.write_seq;
    if (ring->consumer.read_seq != latest_write_seq) {
        /* 清 bit 前已经观察到生产者新写入。 */
        return UNIFIED_OK;
    }

    pending_bit = interface_pending_bit(ring->header.interface_id);
    result = clear_pending_bit(ipc, pending_line, pending_bit);
    if (result != UNIFIED_OK) {
        return result;
    }

    call_memory_barrier(ipc);
    result = call_cache_invalidate(ipc, &ring->producer, sizeof(ring->producer));
    if (result != UNIFIED_OK) {
        return result;
    }

    latest_write_seq = ring->producer.write_seq;
    if (ring->consumer.read_seq != latest_write_seq) {
        /* 清 bit 窗口内同接口又有新 descriptor，必须置回 pending bit。 */
        return set_pending_bit(ipc, pending_line, pending_bit);
    }

    return UNIFIED_OK;
}

/**
 * @brief 消费 reclaim ring 的一个元素并维护 pending bit。
 *
 * @param ipc IPC 上下文。
 * @param write_seq 本次读取前看到的 write_seq。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t consume_reclaim_ring(linux_shm_ipc_t *ipc, uint32_t write_seq)
{
    unified_error_t result;    /**< 平台操作结果。 */
    uint32_t latest_write_seq; /**< 复核时读取到的 write_seq。 */
    put_shm_reclaim_ring_t *ring; /**< reclaim ring 指针。 */

    ring = &ipc->region->reclaim_ring;
    ring->consumer.read_seq = ring->consumer.read_seq + 1u;
    result = call_cache_flush(ipc, &ring->consumer, sizeof(ring->consumer));
    if (result != UNIFIED_OK) {
        return result;
    }

    if (ring->consumer.read_seq != write_seq) {
        return UNIFIED_OK;
    }

    result = call_cache_invalidate(ipc, &ring->producer, sizeof(ring->producer));
    if (result != UNIFIED_OK) {
        return result;
    }

    latest_write_seq = ring->producer.write_seq;
    if (ring->consumer.read_seq != latest_write_seq) {
        return UNIFIED_OK;
    }

    result = clear_pending_bit(ipc, &ipc->region->reclaim_pending, 1u);
    if (result != UNIFIED_OK) {
        return result;
    }

    call_memory_barrier(ipc);
    result = call_cache_invalidate(ipc, &ring->producer, sizeof(ring->producer));
    if (result != UNIFIED_OK) {
        return result;
    }

    latest_write_seq = ring->producer.write_seq;
    if (ring->consumer.read_seq != latest_write_seq) {
        return set_pending_bit(ipc, &ipc->region->reclaim_pending, 1u);
    }

    return UNIFIED_OK;
}

/**
 * @brief 初始化 Linux 侧 IPC 上下文。
 *
 * @param ipc Linux 侧 IPC 上下文。
 */
void linux_shm_ipc_init(linux_shm_ipc_t *ipc)
{
    if (ipc == 0) {
        return;
    }

    memset(ipc, 0, sizeof(*ipc));
    ipc->context_magic = LINUX_SHM_IPC_CONTEXT_MAGIC;
}

/**
 * @brief 映射共享内存 reserved-memory 区域。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param physical_base reserved-memory 物理基地址，host 后端可忽略。
 * @param region_size 需要映射的字节数，必须等于 PUT_SHM_REGION_SIZE。
 * @param ops 平台操作集合；传 NULL 时使用 host/mock 默认实现。
 * @return UNIFIED_OK 表示映射成功，否则返回公共错误码。
 */
unified_error_t linux_shm_ipc_map(linux_shm_ipc_t *ipc,
                                  uintptr_t physical_base,
                                  size_t region_size,
                                  const linux_shm_platform_ops_t *ops)
{
    const linux_shm_platform_ops_t *resolved_ops; /**< 解析后的平台操作。 */
    void *address;                                /**< 映射得到的虚拟地址。 */
    void *mapping_context;                        /**< 映射私有上下文。 */
    unified_error_t result;                       /**< 操作结果。 */

    if (ipc == 0) {
        return UNIFIED_ERR_NULL;
    }

    if (region_size != PUT_SHM_REGION_SIZE) {
        return UNIFIED_ERR_LENGTH;
    }

    linux_shm_ipc_init(ipc);
    resolved_ops = resolve_ops(ops);
    if (!are_ops_ready(resolved_ops, true)) {
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    address = 0;
    mapping_context = 0;
    result = resolved_ops->map_region(physical_base, region_size, &address,
                                      &mapping_context, resolved_ops->user_context);
    if (result != UNIFIED_OK) {
        return result;
    }

    ipc->region = (put_shm_region_t *)address;
    ipc->ops = *resolved_ops;
    ipc->mapping_context = mapping_context;
    ipc->mapped_size = region_size;
    ipc->mapped = true;
    reset_local_state(ipc);
    return UNIFIED_OK;
}

/**
 * @brief 解除 linux_shm_ipc_map() 创建的共享内存映射。
 *
 * @param ipc Linux 侧 IPC 上下文。
 */
void linux_shm_ipc_unmap(linux_shm_ipc_t *ipc)
{
    if ((ipc != 0) && is_context_initialized(ipc) &&
        ipc->mapped && (ipc->ops.unmap_region != 0)) {
        /* 只解除本上下文主动 map 的区域。 */
        ipc->ops.unmap_region(ipc->region, ipc->mapped_size,
                              ipc->mapping_context, ipc->ops.user_context);
    }

    if (ipc != 0) {
        memset(ipc, 0, sizeof(*ipc));
    }
}

/**
 * @brief 格式化共享内存 IPC v2 region 并初始化 Linux 本地状态。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param region 待格式化的共享内存 region。
 * @param linux_epoch Linux 启动纪元。
 * @param rtos_epoch RTOS 启动纪元。
 * @param ops 平台操作集合；传 NULL 时使用 host/mock 默认实现。
 * @return UNIFIED_OK 表示格式化成功，否则返回公共错误码。
 */
unified_error_t linux_shm_ipc_format_region(linux_shm_ipc_t *ipc,
                                            put_shm_region_t *region,
                                            uint32_t linux_epoch,
                                            uint32_t rtos_epoch,
                                            const linux_shm_platform_ops_t *ops)
{
    const linux_shm_platform_ops_t *resolved_ops; /**< 解析后的平台操作。 */
    linux_shm_platform_ops_t selected_ops;        /**< 本次绑定保存的平台操作。 */
    uint32_t interface_index;                     /**< 接口循环索引。 */
    bool preserved_mapped;                        /**< 保存的映射状态。 */
    void *preserved_mapping_context;              /**< 保存的映射私有上下文。 */
    size_t preserved_mapped_size;                 /**< 保存的映射大小。 */
    unified_error_t result;                       /**< 操作结果。 */

    if ((ipc == 0) || (region == 0)) {
        return UNIFIED_ERR_NULL;
    }

    resolved_ops = resolve_binding_ops(ipc, ops);
    if (!are_ops_ready(resolved_ops, false)) {
        return UNIFIED_ERR_IPC_NOT_READY;
    }
    selected_ops = *resolved_ops;

    result = preserve_mapping_fields(ipc, region, &preserved_mapped,
                                     &preserved_mapping_context,
                                     &preserved_mapped_size);
    if (result != UNIFIED_OK) {
        return result;
    }

    linux_shm_ipc_init(ipc);
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

    ipc->region = region;
    ipc->ops = selected_ops;
    ipc->mapping_context = preserved_mapping_context;
    ipc->mapped_size = preserved_mapped_size;
    ipc->mapped = preserved_mapped;
    ipc->initialized = true;
    reset_local_state(ipc);
    return UNIFIED_OK;
}

/**
 * @brief 绑定并校验已有共享内存 IPC v2 region。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param region 已存在的共享内存 region。
 * @param ops 平台操作集合；传 NULL 时使用 host/mock 默认实现。
 * @return UNIFIED_OK 表示绑定成功，否则返回公共错误码。
 */
unified_error_t linux_shm_ipc_attach(linux_shm_ipc_t *ipc,
                                     put_shm_region_t *region,
                                     const linux_shm_platform_ops_t *ops)
{
    const linux_shm_platform_ops_t *resolved_ops; /**< 解析后的平台操作。 */
    linux_shm_platform_ops_t selected_ops;        /**< 本次绑定保存的平台操作。 */
    unified_error_t result;                       /**< 校验结果。 */
    bool preserved_mapped;                        /**< 保存的映射状态。 */
    void *preserved_mapping_context;              /**< 保存的映射私有上下文。 */
    size_t preserved_mapped_size;                 /**< 保存的映射大小。 */

    if ((ipc == 0) || (region == 0)) {
        return UNIFIED_ERR_NULL;
    }

    resolved_ops = resolve_binding_ops(ipc, ops);
    if (!are_ops_ready(resolved_ops, false)) {
        return UNIFIED_ERR_IPC_NOT_READY;
    }
    selected_ops = *resolved_ops;

    result = validate_region(region);
    if (result != UNIFIED_OK) {
        return result;
    }

    result = preserve_mapping_fields(ipc, region, &preserved_mapped,
                                     &preserved_mapping_context,
                                     &preserved_mapped_size);
    if (result != UNIFIED_OK) {
        return result;
    }

    linux_shm_ipc_init(ipc);
    ipc->region = region;
    ipc->ops = selected_ops;
    ipc->mapping_context = preserved_mapping_context;
    ipc->mapped_size = preserved_mapped_size;
    ipc->mapped = preserved_mapped;
    ipc->initialized = true;
    reset_local_state(ipc);
    return UNIFIED_OK;
}

/**
 * @brief 设置单接口 Frame Pool 配额。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param interface_id 接口 ID。
 * @param quota 允许该接口占用的 block 数。
 * @return UNIFIED_OK 表示设置成功，否则返回公共错误码。
 */
unified_error_t linux_shm_ipc_set_interface_quota(linux_shm_ipc_t *ipc,
                                                  put_shm_interface_t interface_id,
                                                  uint64_t quota)
{
    if (ipc == 0) {
        return UNIFIED_ERR_NULL;
    }

    if (!is_context_initialized(ipc)) {
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    if (!is_interface_valid((uint8_t)interface_id) ||
        (quota > PUT_SHM_FRAME_POOL_BLOCK_COUNT)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    ipc->stats.interface_quota[(uint32_t)interface_id].quota = quota;
    return UNIFIED_OK;
}

/**
 * @brief 从 Frame Pool 分配一个 block。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param source_interface 分配来源接口。
 * @param out_frame_id 输出 Frame Pool block ID。
 * @param out_buffer 输出 frame buffer 地址。
 * @param out_capacity 输出 frame buffer 容量。
 * @return UNIFIED_OK 表示分配成功，否则返回公共错误码。
 */
unified_error_t linux_shm_frame_alloc(linux_shm_ipc_t *ipc,
                                      put_shm_interface_t source_interface,
                                      uint32_t *out_frame_id,
                                      uint8_t **out_buffer,
                                      uint16_t *out_capacity)
{
    uint32_t frame_index; /**< Frame Pool 遍历索引。 */
    uint64_t frame_bit;   /**< 当前 frame 对应 bitmap。 */
    linux_shm_interface_quota_stats_t *quota_stats; /**< 当前接口配额统计。 */

    if ((ipc == 0) || (out_frame_id == 0) || (out_buffer == 0) || (out_capacity == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (!is_context_initialized(ipc) || !ipc->initialized || (ipc->region == 0)) {
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    if (!is_interface_valid((uint8_t)source_interface)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    quota_stats = &ipc->stats.interface_quota[(uint32_t)source_interface];
    if (quota_stats->used >= quota_stats->quota) {
        /* 单接口配额耗尽时不继续占用全局 Frame Pool。 */
        quota_stats->full_count++;
        ipc->stats.frame_pool.full_count++;
        return UNIFIED_ERR_IPC_FRAME_POOL_FULL;
    }

    if (ipc->allocation_bitmap == LINUX_SHM_FRAME_BITMAP_FULL) {
        /* 全局 Frame Pool 已满。 */
        ipc->stats.frame_pool.full_count++;
        return UNIFIED_ERR_IPC_FRAME_POOL_FULL;
    }

    for (frame_index = 0u; frame_index < PUT_SHM_FRAME_POOL_BLOCK_COUNT; ++frame_index) {
        frame_bit = (uint64_t)(1ULL << frame_index);
        if ((ipc->allocation_bitmap & frame_bit) == 0u) {
            ipc->allocation_bitmap |= frame_bit;
            ipc->frames[frame_index].state = LINUX_SHM_FRAME_STATE_ALLOCATED;
            ipc->frames[frame_index].source_interface = (uint8_t)source_interface;
            ipc->frames[frame_index].target_interface = 0u;
            ipc->frames[frame_index].pending_reclaim = false;
            ipc->frames[frame_index].allocation_sequence = ++ipc->allocation_sequence;
            ipc->stats.frame_pool.used++;
            ipc->stats.frame_pool.allocated++;
            quota_stats->used++;
            if (ipc->stats.frame_pool.used > ipc->stats.frame_pool.high_watermark) {
                ipc->stats.frame_pool.high_watermark = ipc->stats.frame_pool.used;
            }
            *out_frame_id = frame_index;
            *out_buffer = ipc->region->frame_pool[frame_index].bytes;
            *out_capacity = PUT_SHM_FRAME_POOL_BLOCK_SIZE;
            return UNIFIED_OK;
        }
    }

    ipc->stats.frame_pool.full_count++;
    return UNIFIED_ERR_IPC_FRAME_POOL_FULL;
}

/**
 * @brief 将已分配 frame 发布到来源接口 RX descriptor ring。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param frame_id Frame Pool block ID。
 * @param frame_length 完整 anyMSG 长度。
 * @param source_interface 来源接口。
 * @param target_interface 目标接口。
 * @param source_cid anyMSG source_cid。
 * @param destination_cid anyMSG destination_cid。
 * @param type anyMSG payload type。
 * @param priority 内部调度优先级。
 * @param ttl 内部转发 TTL。
 * @param epoch Linux 启动纪元。
 * @param flags descriptor 标志。
 * @return UNIFIED_OK 表示发布成功，否则返回公共错误码。
 */
unified_error_t linux_shm_frame_commit_rx(linux_shm_ipc_t *ipc,
                                          uint32_t frame_id,
                                          uint16_t frame_length,
                                          put_shm_interface_t source_interface,
                                          put_shm_interface_t target_interface,
                                          const uint8_t source_cid[ANYMSG_CID_LENGTH],
                                          const uint8_t destination_cid[ANYMSG_CID_LENGTH],
                                          uint8_t type,
                                          uint8_t priority,
                                          uint8_t ttl,
                                          uint32_t epoch,
                                          uint32_t flags)
{
    put_shm_descriptor_t descriptor; /**< 待写入 RX ring 的 descriptor。 */
    unified_error_t result;          /**< 操作结果。 */

    if ((ipc == 0) || (source_cid == 0) || (destination_cid == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (!is_context_initialized(ipc) || !ipc->initialized || (ipc->region == 0)) {
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    if ((frame_id >= PUT_SHM_FRAME_POOL_BLOCK_COUNT) ||
        !is_interface_valid((uint8_t)source_interface) ||
        !is_interface_valid((uint8_t)target_interface)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (((ipc->allocation_bitmap & (uint64_t)(1ULL << frame_id)) == 0u) ||
        (ipc->frames[frame_id].state != LINUX_SHM_FRAME_STATE_ALLOCATED) ||
        (ipc->frames[frame_id].source_interface != (uint8_t)source_interface)) {
        /* 只能发布刚由同一来源接口分配、尚未入队的 frame。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    if ((frame_length < ANYMSG_HEADER_SIZE) || (frame_length > PUT_SHM_FRAME_POOL_BLOCK_SIZE)) {
        return UNIFIED_ERR_LENGTH;
    }

    result = call_cache_flush(ipc, ipc->region->frame_pool[frame_id].bytes, frame_length);
    if (result != UNIFIED_OK) {
        return result;
    }

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.frame_id = frame_id;
    descriptor.frame_offset = frame_id * PUT_SHM_FRAME_POOL_BLOCK_SIZE;
    descriptor.frame_length = frame_length;
    descriptor.source_interface = (uint8_t)source_interface;
    descriptor.target_interface = (uint8_t)target_interface;
    memcpy(descriptor.source_cid, source_cid, ANYMSG_CID_LENGTH);
    memcpy(descriptor.destination_cid, destination_cid, ANYMSG_CID_LENGTH);
    descriptor.type = type;
    descriptor.priority = priority;
    descriptor.ttl = ttl;
    descriptor.epoch = epoch;
    descriptor.flags = flags;

    result = linux_shm_enqueue_rx_descriptor(ipc, source_interface, &descriptor);
    if (result == UNIFIED_OK) {
        ipc->frames[frame_id].state = LINUX_SHM_FRAME_STATE_RX_QUEUED;
        ipc->frames[frame_id].target_interface = (uint8_t)target_interface;
    }

    return result;
}

/**
 * @brief 释放 Frame Pool block 并更新统计。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param frame_id Frame Pool block ID。
 * @param reason 释放或 reclaim 原因。
 * @return UNIFIED_OK 表示释放成功，否则返回公共错误码。
 */
unified_error_t linux_shm_frame_release(linux_shm_ipc_t *ipc,
                                        uint32_t frame_id,
                                        put_shm_reclaim_reason_t reason)
{
    uint64_t frame_bit; /**< 当前 frame 对应 bitmap。 */
    uint8_t source_interface; /**< frame 来源接口。 */

    if (ipc == 0) {
        return UNIFIED_ERR_NULL;
    }

    if (!is_context_initialized(ipc) || !ipc->initialized || (ipc->region == 0)) {
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    if (frame_id >= PUT_SHM_FRAME_POOL_BLOCK_COUNT) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    frame_bit = (uint64_t)(1ULL << frame_id);
    if ((ipc->allocation_bitmap & frame_bit) == 0u) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if ((ipc->frames[frame_id].state == LINUX_SHM_FRAME_STATE_RX_QUEUED) &&
        !ipc->frames[frame_id].pending_reclaim) {
        /* RX ring 中仍可能有 descriptor 引用该 frame，必须等待 RTOS reclaim。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    if ((ipc->frames[frame_id].state != LINUX_SHM_FRAME_STATE_ALLOCATED) &&
        (ipc->frames[frame_id].state != LINUX_SHM_FRAME_STATE_TX_READY) &&
        (ipc->frames[frame_id].state != LINUX_SHM_FRAME_STATE_PENDING_RECLAIM) &&
        !ipc->frames[frame_id].pending_reclaim) {
        /* 其他状态不允许通过公开 release API 释放。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (ipc->frames[frame_id].pending_reclaim && (ipc->stats.frame_pool.pending_reclaim > 0u)) {
        ipc->stats.frame_pool.pending_reclaim--;
    }

    source_interface = ipc->frames[frame_id].source_interface;
    if (is_interface_valid(source_interface) &&
        (ipc->stats.interface_quota[source_interface].used > 0u)) {
        ipc->stats.interface_quota[source_interface].used--;
    }

    ipc->allocation_bitmap &= ~frame_bit;
    memset(&ipc->frames[frame_id], 0, sizeof(ipc->frames[frame_id]));
    if (ipc->stats.frame_pool.used > 0u) {
        ipc->stats.frame_pool.used--;
    }
    ipc->stats.frame_pool.released++;
    if ((uint32_t)reason <= PUT_SHM_RECLAIM_REASON_QUEUE_FULL) {
        ipc->stats.reclaim_reason_count[(uint32_t)reason]++;
    }

    return UNIFIED_OK;
}

/**
 * @brief 向指定 RX ring 写入 descriptor。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param interface_id RX ring 对应来源接口。
 * @param descriptor 待发布 descriptor。
 * @return UNIFIED_OK 表示入队成功，否则返回公共错误码。
 */
unified_error_t linux_shm_enqueue_rx_descriptor(linux_shm_ipc_t *ipc,
                                                put_shm_interface_t interface_id,
                                                const put_shm_descriptor_t *descriptor)
{
    put_shm_descriptor_ring_t *ring; /**< RX ring 指针。 */
    put_shm_descriptor_t descriptor_copy; /**< descriptor 副本。 */
    unified_error_t result;          /**< 操作结果。 */
    uint32_t write_seq;              /**< 当前生产者写序号。 */
    uint32_t read_seq;               /**< 当前消费者读序号。 */
    uint32_t descriptor_index;       /**< descriptor 写入下标。 */
    uint32_t used;                   /**< ring 当前占用。 */
    bool was_empty;                  /**< 写入前 ring 是否为空。 */

    if ((ipc == 0) || (descriptor == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (!is_context_initialized(ipc) || !ipc->initialized || (ipc->region == 0)) {
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    if (!is_interface_valid((uint8_t)interface_id)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    ring = &ipc->region->rx_rings[(uint32_t)interface_id];
    result = validate_descriptor_ring(ring, PUT_SHM_RING_KIND_RX,
                                      (uint8_t)interface_id,
                                      PUT_SHM_DIRECTION_LINUX_TO_RTOS);
    if (result != UNIFIED_OK) {
        return result;
    }

    result = validate_descriptor_bounds(descriptor);
    if (result != UNIFIED_OK) {
        return result;
    }

    if (descriptor->source_interface != (uint8_t)interface_id) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (((ipc->allocation_bitmap & (uint64_t)(1ULL << descriptor->frame_id)) == 0u) ||
        (ipc->frames[descriptor->frame_id].state != LINUX_SHM_FRAME_STATE_ALLOCATED) ||
        (ipc->frames[descriptor->frame_id].source_interface != (uint8_t)interface_id)) {
        /* RX descriptor 必须引用 Linux 已分配、尚未发布且来源一致的 frame。 */
        return UNIFIED_ERR_INVALID_ARG;
    }

    result = call_cache_invalidate(ipc, &ring->consumer, sizeof(ring->consumer));
    if (result != UNIFIED_OK) {
        return result;
    }

    write_seq = ring->producer.write_seq;
    read_seq = ring->consumer.read_seq;
    was_empty = (write_seq == read_seq);
    used = ring_used(write_seq, read_seq);
    if (used >= ring->producer.depth) {
        ring->producer.drop_count++;
        ipc->stats.rx_rings[(uint32_t)interface_id].full_count++;
        (void)call_cache_flush(ipc, &ring->producer, sizeof(ring->producer));
        return UNIFIED_ERR_IPC_QUEUE_FULL;
    }

    descriptor_copy = *descriptor;
    descriptor_copy.descriptor_crc16 = descriptor_crc(&descriptor_copy);
    descriptor_index = write_seq % ring->producer.depth;
    ring->descriptors[descriptor_index] = descriptor_copy;
    result = call_cache_flush(ipc, &ring->descriptors[descriptor_index],
                              sizeof(ring->descriptors[descriptor_index]));
    if (result != UNIFIED_OK) {
        return result;
    }

    call_memory_barrier(ipc);
    ring->producer.write_seq = write_seq + 1u;
    ring->producer.enqueue_count++;
    used++;
    if (used > ipc->stats.rx_rings[(uint32_t)interface_id].high_watermark) {
        ipc->stats.rx_rings[(uint32_t)interface_id].high_watermark = used;
    }

    /* write_seq 已经发布，Frame Pool 所有权立即转移给 RX ring。 */
    ipc->frames[descriptor_copy.frame_id].state = LINUX_SHM_FRAME_STATE_RX_QUEUED;
    ipc->frames[descriptor_copy.frame_id].target_interface = descriptor_copy.target_interface;

    result = call_cache_flush(ipc, &ring->producer, sizeof(ring->producer));
    if (result != UNIFIED_OK) {
        /* descriptor 已发布，producer line flush 失败只能按 partial-success 处理。 */
        return UNIFIED_OK;
    }

    result = set_pending_bit(ipc, &ipc->region->rx_pending_bitmap,
                             interface_pending_bit((uint8_t)interface_id));
    if (result != UNIFIED_OK) {
        /* descriptor 已发布，pending 失败不允许调用方重试同一 frame。 */
        ring->producer.notify_fail_count++;
        ipc->stats.mailbox.notify_fail_count++;
        (void)call_cache_flush(ipc, &ring->producer, sizeof(ring->producer));
        return UNIFIED_OK;
    }

    if (was_empty) {
        result = ipc->ops.notify(PUT_SHM_DIRECTION_LINUX_TO_RTOS, ipc->ops.user_context);
        if (result != UNIFIED_OK) {
            ring->producer.notify_fail_count++;
            ipc->stats.mailbox.notify_fail_count++;
            (void)call_cache_flush(ipc, &ring->producer, sizeof(ring->producer));
            return UNIFIED_OK;
        }
        ring->producer.notify_count++;
        ipc->stats.mailbox.rx_doorbell_count++;
        (void)call_cache_flush(ipc, &ring->producer, sizeof(ring->producer));
    }

    return UNIFIED_OK;
}

/**
 * @brief 从指定 TX ring 读取一个 descriptor。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param interface_id TX ring 对应目标接口。
 * @param out_descriptor 输出 descriptor。
 * @param out_frame 输出 frame buffer 只读地址，可传 NULL。
 * @param out_frame_length 输出 frame 长度，可传 NULL。
 * @return UNIFIED_OK 表示出队成功，否则返回公共错误码。
 */
unified_error_t linux_shm_dequeue_tx_descriptor(linux_shm_ipc_t *ipc,
                                                put_shm_interface_t interface_id,
                                                put_shm_descriptor_t *out_descriptor,
                                                const uint8_t **out_frame,
                                                uint16_t *out_frame_length)
{
    put_shm_descriptor_ring_t *ring; /**< TX ring 指针。 */
    put_shm_descriptor_t *descriptor; /**< 当前 descriptor 指针。 */
    unified_error_t result;          /**< 操作结果。 */
    uint32_t write_seq;              /**< 当前生产者写序号。 */
    uint32_t read_seq;               /**< 当前消费者读序号。 */
    uint32_t descriptor_index;       /**< descriptor 读取下标。 */
    uint16_t actual_crc;             /**< 实际 CRC。 */

    if ((ipc == 0) || (out_descriptor == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (!is_context_initialized(ipc) || !ipc->initialized || (ipc->region == 0)) {
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    if (!is_interface_valid((uint8_t)interface_id)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    ring = &ipc->region->tx_rings[(uint32_t)interface_id];
    result = validate_descriptor_ring(ring, PUT_SHM_RING_KIND_TX,
                                      (uint8_t)interface_id,
                                      PUT_SHM_DIRECTION_RTOS_TO_LINUX);
    if (result != UNIFIED_OK) {
        return result;
    }

    result = call_cache_invalidate(ipc, &ring->producer, sizeof(ring->producer));
    if (result != UNIFIED_OK) {
        return result;
    }

    write_seq = ring->producer.write_seq;
    read_seq = ring->consumer.read_seq;
    if (write_seq == read_seq) {
        return UNIFIED_ERR_IPC_QUEUE_EMPTY;
    }

    descriptor_index = read_seq % ring->header.depth;
    descriptor = &ring->descriptors[descriptor_index];
    result = call_cache_invalidate(ipc, descriptor, sizeof(*descriptor));
    if (result != UNIFIED_OK) {
        return result;
    }

    actual_crc = descriptor_crc(descriptor);
    if (actual_crc != descriptor->descriptor_crc16) {
        ring->consumer.crc_error_count++;
        ipc->stats.descriptor_crc_error_count++;
        (void)consume_descriptor_ring(ipc, ring, &ipc->region->tx_pending_bitmap, write_seq);
        return UNIFIED_ERR_CRC;
    }

    result = validate_descriptor_bounds(descriptor);
    if ((result != UNIFIED_OK) || (descriptor->target_interface != (uint8_t)interface_id)) {
        ring->consumer.format_error_count++;
        ipc->stats.descriptor_format_error_count++;
        (void)consume_descriptor_ring(ipc, ring, &ipc->region->tx_pending_bitmap, write_seq);
        return (result != UNIFIED_OK) ? result : UNIFIED_ERR_INVALID_ARG;
    }

    if ((ipc->allocation_bitmap & (uint64_t)(1ULL << descriptor->frame_id)) == 0u) {
        ring->consumer.format_error_count++;
        ipc->stats.descriptor_format_error_count++;
        (void)consume_descriptor_ring(ipc, ring, &ipc->region->tx_pending_bitmap, write_seq);
        return UNIFIED_ERR_INVALID_ARG;
    }

    if ((ipc->frames[descriptor->frame_id].state != LINUX_SHM_FRAME_STATE_RX_QUEUED) ||
        (ipc->frames[descriptor->frame_id].source_interface != descriptor->source_interface)) {
        /* TX 回写只能引用已经交给 RTOS 的 frame，且来源接口必须匹配本地元数据。 */
        ring->consumer.format_error_count++;
        ipc->stats.descriptor_format_error_count++;
        (void)consume_descriptor_ring(ipc, ring, &ipc->region->tx_pending_bitmap, write_seq);
        return UNIFIED_ERR_INVALID_ARG;
    }

    result = call_cache_invalidate(ipc,
                                   ipc->region->frame_pool[descriptor->frame_id].bytes,
                                   descriptor->frame_length);
    if (result != UNIFIED_OK) {
        return result;
    }

    *out_descriptor = *descriptor;
    if (out_frame != 0) {
        *out_frame = ipc->region->frame_pool[descriptor->frame_id].bytes;
    }
    if (out_frame_length != 0) {
        *out_frame_length = descriptor->frame_length;
    }
    ipc->frames[descriptor->frame_id].state = LINUX_SHM_FRAME_STATE_TX_READY;
    ring->consumer.dequeue_count++;
    return consume_descriptor_ring(ipc, ring, &ipc->region->tx_pending_bitmap, write_seq);
}

/**
 * @brief 从 reclaim ring 读取一个回收 descriptor 并释放对应 frame。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param out_descriptor 输出 reclaim descriptor。
 * @return UNIFIED_OK 表示处理成功，否则返回公共错误码。
 */
unified_error_t linux_shm_dequeue_reclaim_descriptor(linux_shm_ipc_t *ipc,
                                                     put_shm_reclaim_descriptor_t *out_descriptor)
{
    put_shm_reclaim_ring_t *ring;          /**< reclaim ring 指针。 */
    put_shm_reclaim_descriptor_t *descriptor; /**< 当前 reclaim descriptor 指针。 */
    unified_error_t result;                /**< 操作结果。 */
    uint32_t write_seq;                    /**< 当前生产者写序号。 */
    uint32_t read_seq;                     /**< 当前消费者读序号。 */
    uint32_t descriptor_index;             /**< descriptor 读取下标。 */
    uint16_t actual_crc;                   /**< 实际 CRC。 */

    if ((ipc == 0) || (out_descriptor == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (!is_context_initialized(ipc) || !ipc->initialized || (ipc->region == 0)) {
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    ring = &ipc->region->reclaim_ring;
    result = validate_reclaim_ring(ring);
    if (result != UNIFIED_OK) {
        return result;
    }

    result = call_cache_invalidate(ipc, &ring->producer, sizeof(ring->producer));
    if (result != UNIFIED_OK) {
        return result;
    }

    write_seq = ring->producer.write_seq;
    read_seq = ring->consumer.read_seq;
    if (write_seq == read_seq) {
        return UNIFIED_ERR_IPC_QUEUE_EMPTY;
    }

    descriptor_index = read_seq % ring->header.depth;
    descriptor = &ring->descriptors[descriptor_index];
    result = call_cache_invalidate(ipc, descriptor, sizeof(*descriptor));
    if (result != UNIFIED_OK) {
        return result;
    }

    actual_crc = reclaim_crc(descriptor);
    if (actual_crc != descriptor->descriptor_crc16) {
        ring->consumer.crc_error_count++;
        ipc->stats.descriptor_crc_error_count++;
        (void)consume_reclaim_ring(ipc, write_seq);
        return UNIFIED_ERR_CRC;
    }

    if ((descriptor->frame_id >= PUT_SHM_FRAME_POOL_BLOCK_COUNT) ||
        ((uint32_t)descriptor->reason > PUT_SHM_RECLAIM_REASON_QUEUE_FULL)) {
        ring->consumer.format_error_count++;
        ipc->stats.descriptor_format_error_count++;
        (void)consume_reclaim_ring(ipc, write_seq);
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (((ipc->allocation_bitmap & (uint64_t)(1ULL << descriptor->frame_id)) == 0u) ||
        (ipc->frames[descriptor->frame_id].state != LINUX_SHM_FRAME_STATE_RX_QUEUED) ||
        (ipc->frames[descriptor->frame_id].source_interface != descriptor->source_interface) ||
        (ipc->frames[descriptor->frame_id].target_interface != descriptor->target_interface)) {
        /* reclaim 只能回收 RTOS 已接管且接口元数据一致的 frame。 */
        ring->consumer.format_error_count++;
        ipc->stats.descriptor_format_error_count++;
        (void)consume_reclaim_ring(ipc, write_seq);
        return UNIFIED_ERR_INVALID_ARG;
    }

    *out_descriptor = *descriptor;
    ipc->frames[descriptor->frame_id].pending_reclaim = true;
    ipc->frames[descriptor->frame_id].state = LINUX_SHM_FRAME_STATE_PENDING_RECLAIM;
    ipc->stats.frame_pool.pending_reclaim++;
    (void)linux_shm_frame_release(ipc, descriptor->frame_id,
                                  (put_shm_reclaim_reason_t)descriptor->reason);

    ipc->stats.reclaim_ack_count++;
    ring->consumer.dequeue_count++;
    return consume_reclaim_ring(ipc, write_seq);
}

/**
 * @brief 获取 Linux 侧 IPC 统计快照。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param out_stats 输出统计快照。
 */
void linux_shm_ipc_get_stats(const linux_shm_ipc_t *ipc,
                             linux_shm_ipc_stats_t *out_stats)
{
    uint32_t interface_index; /**< 接口循环索引。 */
    uint32_t write_seq;       /**< 当前 ring 写序号。 */
    uint32_t read_seq;        /**< 当前 ring 读序号。 */

    if ((ipc == 0) || (out_stats == 0)) {
        return;
    }

    if (!is_context_initialized(ipc)) {
        /* 未初始化上下文没有可信统计，输出全 0 快照。 */
        memset(out_stats, 0, sizeof(*out_stats));
        return;
    }

    *out_stats = ipc->stats;
    if (ipc->region == 0) {
        return;
    }

    out_stats->rx_pending_bits = ipc->region->rx_pending_bitmap.bits;
    out_stats->tx_pending_bits = ipc->region->tx_pending_bitmap.bits;
    out_stats->reclaim_pending_bits = ipc->region->reclaim_pending.bits;

    for (interface_index = 0u; interface_index < PUT_SHM_INTERFACE_COUNT; ++interface_index) {
        write_seq = ipc->region->rx_rings[interface_index].producer.write_seq;
        read_seq = ipc->region->rx_rings[interface_index].consumer.read_seq;
        out_stats->rx_rings[interface_index].used = ring_used(write_seq, read_seq);
        out_stats->rx_rings[interface_index].capacity = PUT_SHM_DESCRIPTOR_RING_DEPTH;
        out_stats->rx_rings[interface_index].enqueue_count =
            ipc->region->rx_rings[interface_index].producer.enqueue_count;
        out_stats->rx_rings[interface_index].dequeue_count =
            ipc->region->rx_rings[interface_index].consumer.dequeue_count;
        out_stats->rx_rings[interface_index].crc_error_count =
            ipc->region->rx_rings[interface_index].consumer.crc_error_count;
        out_stats->rx_rings[interface_index].format_error_count =
            ipc->region->rx_rings[interface_index].consumer.format_error_count;

        write_seq = ipc->region->tx_rings[interface_index].producer.write_seq;
        read_seq = ipc->region->tx_rings[interface_index].consumer.read_seq;
        out_stats->tx_rings[interface_index].used = ring_used(write_seq, read_seq);
        out_stats->tx_rings[interface_index].capacity = PUT_SHM_DESCRIPTOR_RING_DEPTH;
        out_stats->tx_rings[interface_index].enqueue_count =
            ipc->region->tx_rings[interface_index].producer.enqueue_count;
        out_stats->tx_rings[interface_index].dequeue_count =
            ipc->region->tx_rings[interface_index].consumer.dequeue_count;
        out_stats->tx_rings[interface_index].full_count =
            ipc->region->tx_rings[interface_index].producer.drop_count;
        out_stats->tx_rings[interface_index].crc_error_count =
            ipc->region->tx_rings[interface_index].consumer.crc_error_count;
        out_stats->tx_rings[interface_index].format_error_count =
            ipc->region->tx_rings[interface_index].consumer.format_error_count;
        out_stats->mailbox.tx_doorbell_count +=
            ipc->region->tx_rings[interface_index].producer.notify_count;
        out_stats->mailbox.notify_fail_count +=
            ipc->region->tx_rings[interface_index].producer.notify_fail_count;
    }

    write_seq = ipc->region->reclaim_ring.producer.write_seq;
    read_seq = ipc->region->reclaim_ring.consumer.read_seq;
    out_stats->reclaim_ring_used = ring_used(write_seq, read_seq);
}

/**
 * @brief 记录一次 Linux 出口周期兜底 drain。
 *
 * @param ipc Linux 侧 IPC 上下文。
 */
void linux_shm_ipc_record_periodic_drain(linux_shm_ipc_t *ipc)
{
    if ((ipc == 0) || !is_context_initialized(ipc) || !ipc->initialized) {
        return;
    }

    ipc->stats.mailbox.periodic_drain_count++;
}
