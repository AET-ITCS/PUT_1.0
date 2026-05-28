/**
 * @file linux_shm_ipc.h
 * @brief Linux 侧共享内存 IPC v2 Frame Pool 与 descriptor ring 接口。
 * @author Yukikaze
 */
#ifndef LINUX_SHM_IPC_H
#define LINUX_SHM_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linux_shm_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Linux 侧 Frame Pool 单接口默认配额。 */
#define LINUX_SHM_INTERFACE_QUOTA_DEFAULT PUT_SHM_FRAME_POOL_BLOCK_COUNT

/**
 * @brief Frame Pool block 本地状态。
 */
typedef enum {
    LINUX_SHM_FRAME_STATE_FREE = 0u,             /**< block 空闲。 */
    LINUX_SHM_FRAME_STATE_ALLOCATED = 1u,        /**< Linux 已分配但尚未成功入 RX ring。 */
    LINUX_SHM_FRAME_STATE_RX_QUEUED = 2u,        /**< 已发布到 RX ring，等待小核消费。 */
    LINUX_SHM_FRAME_STATE_PENDING_RECLAIM = 3u,  /**< 小核已请求 reclaim，等待 Linux 释放。 */
    LINUX_SHM_FRAME_STATE_TX_READY = 4u,         /**< 小核写入 TX ring，等待 Linux 出口发送。 */
} linux_shm_frame_state_t;

/**
 * @brief 单个 Frame Pool block 的 Linux 本地元数据。
 */
typedef struct {
    linux_shm_frame_state_t state; /**< block 当前状态。 */
    uint8_t source_interface;      /**< 来源接口 ID。 */
    uint8_t target_interface;      /**< 目标接口 ID。 */
    bool pending_reclaim;          /**< 是否处于待 reclaim 状态。 */
    uint64_t allocation_sequence;  /**< 分配序号，用于泄漏排查。 */
} linux_shm_frame_meta_t;

/**
 * @brief Frame Pool 统计。
 */
typedef struct {
    uint64_t capacity;        /**< Frame Pool block 总数。 */
    uint64_t used;            /**< 当前已分配 block 数。 */
    uint64_t high_watermark;  /**< 启动以来最高占用。 */
    uint64_t full_count;      /**< Frame Pool 满次数。 */
    uint64_t allocated;       /**< 成功分配次数。 */
    uint64_t released;        /**< 成功释放次数。 */
    uint64_t pending_reclaim; /**< 等待 Linux 最终释放的 block 数。 */
    uint64_t leaked_suspect;  /**< 疑似泄漏数量。 */
} linux_shm_frame_pool_stats_t;

/**
 * @brief 单接口 Frame Pool 配额统计。
 */
typedef struct {
    uint64_t quota;      /**< 当前接口可占用 block 上限。 */
    uint64_t used;       /**< 当前接口已占用 block 数。 */
    uint64_t full_count; /**< 当前接口配额耗尽次数。 */
} linux_shm_interface_quota_stats_t;

/**
 * @brief ring 快照统计。
 */
typedef struct {
    uint64_t used;               /**< 当前 ring 占用数量。 */
    uint64_t capacity;           /**< ring 容量。 */
    uint64_t high_watermark;     /**< ring 最高占用。 */
    uint64_t enqueue_count;      /**< 生产者成功入队计数。 */
    uint64_t dequeue_count;      /**< 消费者成功出队计数。 */
    uint64_t full_count;         /**< ring full/drop 计数。 */
    uint64_t crc_error_count;    /**< CRC 错误计数。 */
    uint64_t format_error_count; /**< 格式错误计数。 */
} linux_shm_ring_stats_t;

/**
 * @brief mailbox/doorbell 统计。
 */
typedef struct {
    uint64_t rx_doorbell_count; /**< Linux 通知 RTOS 的 doorbell 成功次数。 */
    uint64_t tx_doorbell_count; /**< RTOS 通知 Linux 的 doorbell 成功次数快照。 */
    uint64_t notify_fail_count; /**< 通知失败计数。 */
    uint64_t periodic_drain_count; /**< 周期 drain 兜底计数。 */
} linux_shm_mailbox_stats_t;

/**
 * @brief 完整 IPC 统计快照。
 */
typedef struct {
    linux_shm_frame_pool_stats_t frame_pool; /**< Frame Pool 统计。 */
    linux_shm_interface_quota_stats_t interface_quota[PUT_SHM_INTERFACE_COUNT]; /**< 每接口配额统计。 */
    linux_shm_ring_stats_t rx_rings[PUT_SHM_INTERFACE_COUNT]; /**< RX ring 统计。 */
    linux_shm_ring_stats_t tx_rings[PUT_SHM_INTERFACE_COUNT]; /**< TX ring 统计。 */
    uint32_t rx_pending_bits;      /**< RX pending bitmap 快照。 */
    uint32_t tx_pending_bits;      /**< TX pending bitmap 快照。 */
    uint32_t reclaim_pending_bits; /**< reclaim pending bitmap 快照。 */
    linux_shm_mailbox_stats_t mailbox; /**< mailbox/doorbell 统计。 */
    uint64_t descriptor_crc_error_count; /**< descriptor CRC 错误总数。 */
    uint64_t descriptor_format_error_count; /**< descriptor 格式错误总数。 */
    uint64_t cache_sync_error_count; /**< cache 同步错误总数。 */
    uint64_t reclaim_reason_count[PUT_SHM_RECLAIM_REASON_QUEUE_FULL + 1u]; /**< reclaim reason 统计。 */
    uint64_t reclaim_ack_count; /**< 成功处理 reclaim descriptor 数。 */
    uint64_t reclaim_ring_used; /**< reclaim ring 当前占用数量。 */
} linux_shm_ipc_stats_t;

/**
 * @brief Linux 侧共享内存 IPC 上下文。
 */
typedef struct {
    uint32_t context_magic;             /**< Linux IPC 上下文初始化 magic。 */
    put_shm_region_t *region;          /**< 绑定的共享内存 v2 region。 */
    linux_shm_platform_ops_t ops;      /**< 当前平台操作集合。 */
    void *mapping_context;             /**< 映射私有上下文。 */
    size_t mapped_size;                /**< 当前映射大小。 */
    bool mapped;                       /**< 是否由本上下文完成映射。 */
    bool initialized;                  /**< 是否已完成 format 或 attach。 */
    uint64_t allocation_bitmap;        /**< Frame Pool 分配 bitmap。 */
    linux_shm_frame_meta_t frames[PUT_SHM_FRAME_POOL_BLOCK_COUNT]; /**< 每个 block 的本地元数据。 */
    linux_shm_ipc_stats_t stats;       /**< IPC 统计。 */
    uint64_t allocation_sequence;      /**< 全局分配序号。 */
} linux_shm_ipc_t;

/**
 * @brief 初始化 Linux 侧 IPC 上下文。
 *
 * @param ipc Linux 侧 IPC 上下文。
 */
void linux_shm_ipc_init(linux_shm_ipc_t *ipc);
/**
 * @brief 映射共享内存 reserved-memory 区域。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param physical_base reserved-memory 物理基地址，host 后端可忽略。
 * @param region_size 需要映射的字节数。
 * @param ops 平台操作集合；传 NULL 时使用 host/mock 默认实现。
 * @return UNIFIED_OK 表示映射成功，否则返回公共错误码。
 */
unified_error_t linux_shm_ipc_map(linux_shm_ipc_t *ipc,
                                  uintptr_t physical_base,
                                  size_t region_size,
                                  const linux_shm_platform_ops_t *ops);
/**
 * @brief 解除 linux_shm_ipc_map() 创建的映射。
 *
 * @param ipc Linux 侧 IPC 上下文。
 */
void linux_shm_ipc_unmap(linux_shm_ipc_t *ipc);
/**
 * @brief 格式化共享内存 IPC v2 region。
 *
 * 调用方应先使用 linux_shm_ipc_init() 初始化上下文，或使用
 * linux_shm_ipc_map() 获得已初始化并带映射生命周期的上下文。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param region 待格式化 region。
 * @param linux_epoch Linux 启动纪元。
 * @param rtos_epoch RTOS 启动纪元。
 * @param ops 平台操作集合；传 NULL 时使用 host/mock 默认实现。
 * @return UNIFIED_OK 表示格式化成功，否则返回公共错误码。
 */
unified_error_t linux_shm_ipc_format_region(linux_shm_ipc_t *ipc,
                                            put_shm_region_t *region,
                                            uint32_t linux_epoch,
                                            uint32_t rtos_epoch,
                                            const linux_shm_platform_ops_t *ops);
/**
 * @brief 绑定并校验已有共享内存 IPC v2 region。
 *
 * 调用方应先使用 linux_shm_ipc_init() 初始化上下文，或使用
 * linux_shm_ipc_map() 获得已初始化并带映射生命周期的上下文。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param region 已存在 region。
 * @param ops 平台操作集合；传 NULL 时使用 host/mock 默认实现。
 * @return UNIFIED_OK 表示绑定成功，否则返回公共错误码。
 */
unified_error_t linux_shm_ipc_attach(linux_shm_ipc_t *ipc,
                                     put_shm_region_t *region,
                                     const linux_shm_platform_ops_t *ops);
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
                                                  uint64_t quota);
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
                                      uint16_t *out_capacity);
/**
 * @brief 将已分配 frame 发布到来源接口 RX ring。
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
                                          uint32_t flags);
/**
 * @brief 释放 Frame Pool block。
 *
 * 公开释放只允许未发布的 ALLOCATED frame、Linux 已读出的 TX_READY frame，
 * 或已经由 reclaim 标记的 PENDING_RECLAIM frame。RX_QUEUED frame 必须等待
 * RTOS 通过 reclaim ring 明确回收，避免 ring 内 descriptor 引用悬空 block。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param frame_id Frame Pool block ID。
 * @param reason 释放或 reclaim 原因。
 * @return UNIFIED_OK 表示释放成功，否则返回公共错误码。
 */
unified_error_t linux_shm_frame_release(linux_shm_ipc_t *ipc,
                                        uint32_t frame_id,
                                        put_shm_reclaim_reason_t reason);
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
                                                const put_shm_descriptor_t *descriptor);
/**
 * @brief 从指定 TX ring 读取 descriptor。
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
                                                uint16_t *out_frame_length);
/**
 * @brief 从 reclaim ring 读取 descriptor 并释放对应 frame。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param out_descriptor 输出 reclaim descriptor。
 * @return UNIFIED_OK 表示处理成功，否则返回公共错误码。
 */
unified_error_t linux_shm_dequeue_reclaim_descriptor(linux_shm_ipc_t *ipc,
                                                     put_shm_reclaim_descriptor_t *out_descriptor);
/**
 * @brief 获取 Linux 侧 IPC 统计快照。
 *
 * @param ipc Linux 侧 IPC 上下文。
 * @param out_stats 输出统计快照。
 */
void linux_shm_ipc_get_stats(const linux_shm_ipc_t *ipc,
                             linux_shm_ipc_stats_t *out_stats);
/**
 * @brief 记录一次 Linux 出口周期兜底 drain。
 *
 * @param ipc Linux 侧 IPC 上下文。
 */
void linux_shm_ipc_record_periodic_drain(linux_shm_ipc_t *ipc);

#ifdef __cplusplus
}
#endif

#endif /* LINUX_SHM_IPC_H */
