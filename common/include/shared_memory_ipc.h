/**
 * @file shared_memory_ipc.h
 * @brief 大小核共享内存 IPC v2 公共 ABI 定义。
 * @author Yukikaze
 *
 * v2 ABI 使用 Frame Pool + 每接口 Descriptor Ring + Pending Bitmap。
 * 共享内存层只搬运完整 anyMSG 的描述符，不解释业务 payload。
 */
#ifndef SHARED_MEMORY_IPC_H
#define SHARED_MEMORY_IPC_H

#include <stdint.h>

#include "anymsg_frame.h"
#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PUT_SHM_PACKED __attribute__((packed))
#define PUT_SHM_PACKED_ALIGNED __attribute__((packed, aligned(PUT_SHM_CACHE_LINE_SIZE)))
#else
#define PUT_SHM_PACKED
#define PUT_SHM_PACKED_ALIGNED
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define PUT_SHM_STATIC_ASSERT(condition, message) _Static_assert((condition), message)
#else
#define PUT_SHM_CONCAT_INNER(a, b) a##b
#define PUT_SHM_CONCAT(a, b) PUT_SHM_CONCAT_INNER(a, b)
#define PUT_SHM_STATIC_ASSERT(condition, message) \
    typedef char PUT_SHM_CONCAT(put_shm_static_assert_, __LINE__)[(condition) ? 1 : -1]
#endif

/** @brief 共享内存 IPC ABI 版本，v2 固定为 Frame Pool + Descriptor Ring。 */
#define PUT_SHM_IPC_VERSION 2u

/** @brief 共享内存区域 magic，用于区分 v1 历史布局和 v2 正式布局。 */
#define PUT_SHM_REGION_MAGIC 0x50555432u

/** @brief 共享内存 descriptor ring magic。 */
#define PUT_SHM_RING_MAGIC 0x4452494Eu

/** @brief 共享内存 cache line 对齐长度。 */
#define PUT_SHM_CACHE_LINE_SIZE 64u

/** @brief 共享内存 reserved-memory 总大小。 */
#define PUT_SHM_REGION_SIZE (64u * 1024u)

/** @brief v2 支持的物理接口数量。 */
#define PUT_SHM_INTERFACE_COUNT 6u

/** @brief Frame Pool 固定 block 数量。 */
#define PUT_SHM_FRAME_POOL_BLOCK_COUNT 64u

/** @brief Frame Pool 单 block 字节数。 */
#define PUT_SHM_FRAME_POOL_BLOCK_SIZE 512u

/** @brief Frame Pool 总字节数。 */
#define PUT_SHM_FRAME_POOL_SIZE \
    (PUT_SHM_FRAME_POOL_BLOCK_COUNT * PUT_SHM_FRAME_POOL_BLOCK_SIZE)

/** @brief descriptor 固定长度。 */
#define PUT_SHM_DESCRIPTOR_SIZE PUT_SHM_CACHE_LINE_SIZE

/** @brief reclaim descriptor 固定长度。 */
#define PUT_SHM_RECLAIM_DESCRIPTOR_SIZE PUT_SHM_CACHE_LINE_SIZE

/** @brief 每个 descriptor ring 的深度。 */
#define PUT_SHM_DESCRIPTOR_RING_DEPTH 8u

/** @brief 每个 reclaim ring 的深度。 */
#define PUT_SHM_RECLAIM_RING_DEPTH 8u

/** @brief region header 固定占用一个 cache line。 */
#define PUT_SHM_REGION_HEADER_SIZE PUT_SHM_CACHE_LINE_SIZE

/** @brief descriptor ring header 固定占用一个 cache line。 */
#define PUT_SHM_RING_HEADER_SIZE PUT_SHM_CACHE_LINE_SIZE

/** @brief ring 生产者控制行固定占用一个 cache line。 */
#define PUT_SHM_RING_PRODUCER_SIZE PUT_SHM_CACHE_LINE_SIZE

/** @brief ring 消费者控制行固定占用一个 cache line。 */
#define PUT_SHM_RING_CONSUMER_SIZE PUT_SHM_CACHE_LINE_SIZE

/** @brief pending bitmap 控制行固定占用一个 cache line。 */
#define PUT_SHM_PENDING_LINE_SIZE PUT_SHM_CACHE_LINE_SIZE

/** @brief 单个 descriptor ring 固定大小。 */
#define PUT_SHM_DESCRIPTOR_RING_SIZE \
    (PUT_SHM_RING_HEADER_SIZE + PUT_SHM_RING_PRODUCER_SIZE + \
     PUT_SHM_RING_CONSUMER_SIZE + (PUT_SHM_DESCRIPTOR_SIZE * PUT_SHM_DESCRIPTOR_RING_DEPTH))

/** @brief 单个 reclaim ring 固定大小。 */
#define PUT_SHM_RECLAIM_RING_SIZE \
    (PUT_SHM_RING_HEADER_SIZE + PUT_SHM_RING_PRODUCER_SIZE + \
     PUT_SHM_RING_CONSUMER_SIZE + \
     (PUT_SHM_RECLAIM_DESCRIPTOR_SIZE * PUT_SHM_RECLAIM_RING_DEPTH))

/** @brief region 尾部保留区长度。 */
#define PUT_SHM_REGION_RESERVED_LEN \
    (PUT_SHM_REGION_SIZE - PUT_SHM_REGION_HEADER_SIZE - PUT_SHM_FRAME_POOL_SIZE - \
     (PUT_SHM_DESCRIPTOR_RING_SIZE * PUT_SHM_INTERFACE_COUNT * 2u) - \
     (PUT_SHM_PENDING_LINE_SIZE * 3u) - PUT_SHM_RECLAIM_RING_SIZE)

/**
 * @brief 共享内存跨核通知方向。
 */
typedef enum {
    PUT_SHM_DIRECTION_NONE = 0u,          /**< 未初始化方向。 */
    PUT_SHM_DIRECTION_LINUX_TO_RTOS = 1u, /**< Linux 写入后通知 RTOS。 */
    PUT_SHM_DIRECTION_RTOS_TO_LINUX = 2u, /**< RTOS 写入后通知 Linux。 */
} put_shm_direction_t;

/**
 * @brief v2 支持的物理接口 ID。
 */
typedef enum {
    PUT_SHM_INTERFACE_CAN = 0u,       /**< CAN 物理接口。 */
    PUT_SHM_INTERFACE_ETHERNET = 1u,  /**< Ethernet 物理接口。 */
    PUT_SHM_INTERFACE_WIFI = 2u,      /**< Wi-Fi 物理接口。 */
    PUT_SHM_INTERFACE_BLUETOOTH = 3u, /**< Bluetooth 物理接口。 */
    PUT_SHM_INTERFACE_4G = 4u,        /**< 4G 蜂窝物理接口。 */
    PUT_SHM_INTERFACE_RS485 = 5u,     /**< RS485 物理接口。 */
} put_shm_interface_t;

/**
 * @brief descriptor ring 类型。
 */
typedef enum {
    PUT_SHM_RING_KIND_NONE = 0u,    /**< 未初始化 ring。 */
    PUT_SHM_RING_KIND_RX = 1u,      /**< Linux 写、RTOS 读的 RX ring。 */
    PUT_SHM_RING_KIND_TX = 2u,      /**< RTOS 写、Linux 读的 TX ring。 */
    PUT_SHM_RING_KIND_RECLAIM = 3u, /**< RTOS 写、Linux 读的回收 ring。 */
} put_shm_ring_kind_t;

/**
 * @brief Frame Pool 回收原因。
 */
typedef enum {
    PUT_SHM_RECLAIM_REASON_NONE = 0u,               /**< 未指定回收原因。 */
    PUT_SHM_RECLAIM_REASON_HEARTBEAT_CONSUMED = 1u, /**< 小核消费端到网关心跳。 */
    PUT_SHM_RECLAIM_REASON_NO_ROUTE = 2u,           /**< 小核未找到目标路由。 */
    PUT_SHM_RECLAIM_REASON_TTL_EXPIRED = 3u,        /**< descriptor TTL 已过期。 */
    PUT_SHM_RECLAIM_REASON_EPOCH_MISMATCH = 4u,     /**< Linux epoch 不匹配。 */
    PUT_SHM_RECLAIM_REASON_INVALID_FRAME = 5u,      /**< anyMSG 基础校验失败。 */
    PUT_SHM_RECLAIM_REASON_QUEUE_FULL = 6u,         /**< 本地队列或目标 ring 已满。 */
} put_shm_reclaim_reason_t;

/** @brief Linux 已完成入口鉴权，外部入口可被小核视为 AUTH_OK。 */
#define PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK (1u << 0u)

/** @brief Linux 已完成业务完整性校验，外部入口可被小核视为 INTEGRITY_OK。 */
#define PUT_SHM_DESCRIPTOR_FLAG_INTEGRITY_OK (1u << 1u)

/** @brief Linux 已完成重放保护检查，外部入口可被小核视为 REPLAY_OK。 */
#define PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK (1u << 2u)

/** @brief Linux 明确标记该帧来自内部可信入口，可跳过外部入口鉴权要求。 */
#define PUT_SHM_DESCRIPTOR_FLAG_INTERNAL_TRUSTED (1u << 3u)

/** @brief Linux 明确允许该帧进入高优先级或 CAN/RS485 控制路径。 */
#define PUT_SHM_DESCRIPTOR_FLAG_CONTROL_ALLOWED (1u << 4u)

/** @brief descriptor trust 低位标志 mask，其他高位保留给后续诊断或业务标志。 */
#define PUT_SHM_DESCRIPTOR_TRUST_FLAG_MASK \
    (PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK | PUT_SHM_DESCRIPTOR_FLAG_INTEGRITY_OK | \
     PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK | PUT_SHM_DESCRIPTOR_FLAG_INTERNAL_TRUSTED | \
     PUT_SHM_DESCRIPTOR_FLAG_CONTROL_ALLOWED)

/**
 * @brief Frame Pool 固定 block。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    uint8_t bytes[PUT_SHM_FRAME_POOL_BLOCK_SIZE]; /**< 完整 anyMSG 字节缓存。 */
} put_shm_frame_block_t;

/**
 * @brief descriptor ring 中搬运的完整 anyMSG 元数据。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    uint32_t frame_id;        /**< Frame Pool block ID，范围为 0 ~ 63。 */
    uint32_t frame_offset;    /**< 完整 anyMSG 相对 Frame Pool 起点的偏移。 */
    uint16_t frame_length;    /**< 完整 anyMSG 字节数，包含 40B header。 */
    uint8_t source_interface; /**< 来源物理接口，取值见 put_shm_interface_t。 */
    uint8_t target_interface; /**< 目标物理接口，取值见 put_shm_interface_t。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH];      /**< anyMSG source_cid raw bytes。 */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< anyMSG destination_cid raw bytes。 */
    uint8_t type;             /**< anyMSG payload type。 */
    uint8_t priority;         /**< 内部调度优先级，不写入 anyMSG 保留字段。 */
    uint8_t ttl;              /**< 内部转发 TTL。 */
    uint8_t reserved0;        /**< 保留字段，发送方必须填 0。 */
    uint32_t epoch;           /**< Linux 启动纪元。 */
    uint32_t flags;           /**< descriptor 内部标志。 */
    uint8_t reserved1[30];    /**< 保留字段，发送方必须填 0。 */
    uint16_t descriptor_crc16; /**< descriptor CRC-16，覆盖本字段之前的字节。 */
} put_shm_descriptor_t;

/**
 * @brief 小核通知 Linux 回收 Frame Pool block 的 descriptor。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    uint32_t frame_id;        /**< 需要 Linux 回收的 Frame Pool block ID。 */
    uint32_t reason;          /**< 回收原因，取值见 put_shm_reclaim_reason_t。 */
    uint8_t source_interface; /**< 原始来源物理接口。 */
    uint8_t target_interface; /**< 原始目标物理接口。 */
    uint16_t reserved0;       /**< 保留字段，发送方必须填 0。 */
    uint32_t epoch;           /**< Linux 启动纪元。 */
    uint32_t flags;           /**< 回收附加标志。 */
    uint8_t reserved1[42];    /**< 保留字段，发送方必须填 0。 */
    uint16_t descriptor_crc16; /**< descriptor CRC-16，覆盖本字段之前的字节。 */
} put_shm_reclaim_descriptor_t;

/**
 * @brief descriptor ring 固定 header。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    uint32_t magic;           /**< 固定为 PUT_SHM_RING_MAGIC。 */
    uint16_t version;         /**< 固定为 PUT_SHM_IPC_VERSION。 */
    uint16_t header_size;     /**< 固定为 sizeof(put_shm_ring_header_t)。 */
    uint16_t depth;           /**< ring descriptor 数量。 */
    uint16_t descriptor_size; /**< 单个 descriptor 大小。 */
    uint8_t direction;        /**< 生产者到消费者的通知方向。 */
    uint8_t interface_id;     /**< 对应物理接口 ID，reclaim ring 固定为 0。 */
    uint8_t ring_kind;        /**< ring 类型，取值见 put_shm_ring_kind_t。 */
    uint8_t reserved[49];     /**< 保留字段，初始化时必须填 0。 */
} put_shm_ring_header_t;

/**
 * @brief ring 生产者拥有的 cache line。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    volatile uint32_t write_seq; /**< 生产者写序号，只允许生产者更新。 */
    uint32_t depth;              /**< ring 深度冗余副本，用于快速校验。 */
    uint32_t descriptor_size;    /**< descriptor 大小冗余副本，用于快速校验。 */
    uint32_t enqueue_count;      /**< 成功入队 descriptor 数量。 */
    uint32_t drop_count;         /**< ring full 丢弃最新 descriptor 的数量。 */
    uint32_t notify_count;       /**< 成功触发 doorbell/mailbox 的数量。 */
    uint32_t notify_fail_count;  /**< descriptor 已发布但通知失败的数量。 */
    uint8_t reserved[36];        /**< 保留字段，初始化时必须填 0。 */
} put_shm_ring_producer_t;

/**
 * @brief ring 消费者拥有的 cache line。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    volatile uint32_t read_seq;  /**< 消费者读序号，只允许消费者更新。 */
    uint32_t dequeue_count;      /**< 成功出队 descriptor 数量。 */
    uint32_t crc_error_count;    /**< descriptor CRC 错误数量。 */
    uint32_t format_error_count; /**< descriptor 或 ring 格式错误数量。 */
    uint8_t reserved[48];        /**< 保留字段，初始化时必须填 0。 */
} put_shm_ring_consumer_t;

/**
 * @brief descriptor ring。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    put_shm_ring_header_t header;                         /**< ring 固定 header。 */
    put_shm_ring_producer_t producer;                     /**< 生产者控制行。 */
    put_shm_ring_consumer_t consumer;                     /**< 消费者控制行。 */
    put_shm_descriptor_t descriptors[PUT_SHM_DESCRIPTOR_RING_DEPTH]; /**< descriptor 数组。 */
} put_shm_descriptor_ring_t;

/**
 * @brief reclaim ring。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    put_shm_ring_header_t header;                                     /**< ring 固定 header。 */
    put_shm_ring_producer_t producer;                                 /**< 生产者控制行。 */
    put_shm_ring_consumer_t consumer;                                 /**< 消费者控制行。 */
    put_shm_reclaim_descriptor_t descriptors[PUT_SHM_RECLAIM_RING_DEPTH]; /**< 回收 descriptor 数组。 */
} put_shm_reclaim_ring_t;

/**
 * @brief pending bitmap 独立 cache line。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    volatile uint32_t bits; /**< pending bitmask，每个 bit 表示一个 ring 可能非空。 */
    uint32_t set_count;     /**< pending bit 被设置的次数。 */
    uint32_t clear_count;   /**< pending bit 被清除的次数。 */
    uint32_t reserved0;     /**< 保留字段，初始化时必须填 0。 */
    uint8_t reserved1[48];  /**< 保留字段，初始化时必须填 0。 */
} put_shm_pending_line_t;

/**
 * @brief 共享内存区域固定 header。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    uint32_t magic;                  /**< 固定为 PUT_SHM_REGION_MAGIC。 */
    uint16_t version;                /**< 固定为 PUT_SHM_IPC_VERSION。 */
    uint16_t header_size;            /**< 固定为 sizeof(put_shm_region_header_t)。 */
    uint32_t region_size;            /**< 固定为 PUT_SHM_REGION_SIZE。 */
    uint32_t frame_pool_offset;      /**< Frame Pool 相对 region 起点的偏移。 */
    uint32_t frame_pool_block_count; /**< Frame Pool block 数量。 */
    uint32_t frame_pool_block_size;  /**< Frame Pool 单 block 大小。 */
    uint32_t rx_rings_offset;        /**< RX ring 数组偏移。 */
    uint32_t tx_rings_offset;        /**< TX ring 数组偏移。 */
    uint32_t rx_pending_offset;      /**< RX pending bitmap 偏移。 */
    uint32_t tx_pending_offset;      /**< TX pending bitmap 偏移。 */
    uint32_t reclaim_pending_offset; /**< reclaim pending bitmap 偏移。 */
    uint32_t reclaim_ring_offset;    /**< reclaim ring 偏移。 */
    uint32_t linux_epoch;            /**< Linux 当前启动纪元。 */
    uint32_t rtos_epoch;             /**< RTOS 当前启动纪元。 */
    uint8_t reserved[8];             /**< 保留字段，初始化时必须填 0。 */
} put_shm_region_header_t;

/**
 * @brief 共享内存 IPC v2 完整区域。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    put_shm_region_header_t header;                                  /**< 共享内存区域 header。 */
    put_shm_frame_block_t frame_pool[PUT_SHM_FRAME_POOL_BLOCK_COUNT]; /**< 完整 anyMSG Frame Pool。 */
    put_shm_descriptor_ring_t rx_rings[PUT_SHM_INTERFACE_COUNT];      /**< Linux 写、RTOS 读的 RX rings。 */
    put_shm_descriptor_ring_t tx_rings[PUT_SHM_INTERFACE_COUNT];      /**< RTOS 写、Linux 读的 TX rings。 */
    put_shm_pending_line_t rx_pending_bitmap;                         /**< RX pending bitmap。 */
    put_shm_pending_line_t tx_pending_bitmap;                         /**< TX pending bitmap。 */
    put_shm_pending_line_t reclaim_pending;                           /**< reclaim pending bitmap。 */
    put_shm_reclaim_ring_t reclaim_ring;                              /**< RTOS 写、Linux 读的回收 ring。 */
    uint8_t reserved[PUT_SHM_REGION_RESERVED_LEN];                    /**< 尾部保留区。 */
} put_shm_region_t;

PUT_SHM_STATIC_ASSERT(ANYMSG_HEADER_SIZE == 40u,
                      "anyMSG header must be 40 bytes");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_descriptor_t) == PUT_SHM_DESCRIPTOR_SIZE,
                      "shared memory descriptor must be 64 bytes");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_reclaim_descriptor_t) == PUT_SHM_RECLAIM_DESCRIPTOR_SIZE,
                      "shared memory reclaim descriptor must be 64 bytes");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_ring_header_t) == PUT_SHM_RING_HEADER_SIZE,
                      "shared memory ring header must be one cache line");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_ring_producer_t) == PUT_SHM_RING_PRODUCER_SIZE,
                      "shared memory producer state must be one cache line");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_ring_consumer_t) == PUT_SHM_RING_CONSUMER_SIZE,
                      "shared memory consumer state must be one cache line");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_pending_line_t) == PUT_SHM_PENDING_LINE_SIZE,
                      "shared memory pending bitmap must be one cache line");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_descriptor_ring_t) == PUT_SHM_DESCRIPTOR_RING_SIZE,
                      "shared memory descriptor ring size must match ABI constant");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_reclaim_ring_t) == PUT_SHM_RECLAIM_RING_SIZE,
                      "shared memory reclaim ring size must match ABI constant");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_region_header_t) == PUT_SHM_REGION_HEADER_SIZE,
                      "shared memory region header must be one cache line");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_region_t) == PUT_SHM_REGION_SIZE,
                      "shared memory region must be 64 KiB");

#ifdef __cplusplus
}
#endif

#endif /* SHARED_MEMORY_IPC_H */
