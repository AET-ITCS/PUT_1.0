/**
 * @file shared_memory_ipc.h
 * @brief 大小核共享内存 IPC v1 公共 ABI 定义。
 * @author Yukikaze
 *
 * 本文件只冻结共享内存区域、ring、slot 和公共 payload 的 ABI。
 * 具体读写实现、cache flush/invalidate、mailbox/cmdqu doorbell 由
 * Linux 侧和 rtos_firmware/ 侧的平台适配层分别实现。
 */
#ifndef SHARED_MEMORY_IPC_H
#define SHARED_MEMORY_IPC_H

#include <stdint.h>

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

/**
 * @brief 共享内存 IPC ABI 版本。
 *
 * v1 固定使用双向 SPSC ring，payload 作为 opaque bytes 搬运。
 */
#define PUT_SHM_IPC_VERSION 1u

/** @brief 共享内存区域 magic，用于判断 reserved-memory 是否为项目 IPC 区。 */
#define PUT_SHM_REGION_MAGIC 0x50555431u

/** @brief 共享内存 ring magic，用于判断单个 ring 是否初始化完成。 */
#define PUT_SHM_RING_MAGIC 0x52494E47u

/** @brief 共享内存 slot magic，用于判断单个 slot 内容是否可解析。 */
#define PUT_SHM_SLOT_MAGIC 0x53484D31u

/** @brief 共享内存 cache line 对齐长度，SG2002 双核通信按 64 字节规划。 */
#define PUT_SHM_CACHE_LINE_SIZE 64u

/** @brief 单个 IPC payload 最大长度。 */
#define PUT_SHM_PAYLOAD_MAX_LEN 128u

/** @brief 经典 CAN 数据区长度，仅用于历史 CAN RX 回传 payload。 */
#define PUT_SHM_CAN_CLASSIC_DATA_MAX_LEN 8u

/** @brief 单个共享内存 slot 固定长度，便于按 cache line 对齐和跳转。 */
#define PUT_SHM_SLOT_SIZE 256u

/** @brief Linux -> RTOS ring 深度，v1 使用 32 个 slot。 */
#define PUT_SHM_L2R_DEPTH 32u

/** @brief RTOS -> Linux ring 深度，v1 使用 32 个 slot。 */
#define PUT_SHM_R2L_DEPTH 32u

/** @brief 共享内存 reserved-memory 总大小，物理地址由 DTS 和 RTOS BSP 提供。 */
#define PUT_SHM_REGION_SIZE (64u * 1024u)

/** @brief ring header 固定占用 1 个 cache line。 */
#define PUT_SHM_RING_HEADER_SIZE PUT_SHM_CACHE_LINE_SIZE

/** @brief 生产者控制行固定占用 1 个 cache line，避免和消费者写字段共享 cache line。 */
#define PUT_SHM_RING_PRODUCER_SIZE PUT_SHM_CACHE_LINE_SIZE

/** @brief 消费者控制行固定占用 1 个 cache line，避免和生产者写字段共享 cache line。 */
#define PUT_SHM_RING_CONSUMER_SIZE PUT_SHM_CACHE_LINE_SIZE

/** @brief slot header 固定长度。 */
#define PUT_SHM_SLOT_HEADER_SIZE 32u

/** @brief slot 中未使用区域长度，用于将 slot 固定到 PUT_SHM_SLOT_SIZE。 */
#define PUT_SHM_SLOT_RESERVED_LEN \
    (PUT_SHM_SLOT_SIZE - PUT_SHM_SLOT_HEADER_SIZE - PUT_SHM_PAYLOAD_MAX_LEN)

/** @brief 单个 ring 固定长度，两个方向当前使用同一种 ring 布局。 */
#define PUT_SHM_RING_SIZE \
    (PUT_SHM_RING_HEADER_SIZE + PUT_SHM_RING_PRODUCER_SIZE + \
     PUT_SHM_RING_CONSUMER_SIZE + (PUT_SHM_SLOT_SIZE * PUT_SHM_L2R_DEPTH))

/** @brief 共享内存区域 header 固定占用 1 个 cache line。 */
#define PUT_SHM_REGION_HEADER_SIZE PUT_SHM_CACHE_LINE_SIZE

/** @brief 共享内存区域尾部保留长度，后续可用于扩展调试信息或新 ring。 */
#define PUT_SHM_REGION_RESERVED_LEN \
    (PUT_SHM_REGION_SIZE - PUT_SHM_REGION_HEADER_SIZE - (PUT_SHM_RING_SIZE * 2u))

/**
 * @brief 共享内存 ring 方向。
 */
typedef enum {
    PUT_SHM_DIRECTION_NONE = 0u,          /**< 未初始化方向。 */
    PUT_SHM_DIRECTION_LINUX_TO_RTOS = 1u, /**< Linux 大核写、RTOS 小核读。 */
    PUT_SHM_DIRECTION_RTOS_TO_LINUX = 2u, /**< RTOS 小核写、Linux 大核读。 */
} put_shm_direction_t;

/**
 * @brief 共享内存 IPC slot 消息类型。
 */
typedef enum {
    PUT_SHM_MESSAGE_TYPE_NONE = 0u,          /**< 空消息或未初始化 slot。 */
    PUT_SHM_MESSAGE_TYPE_OPAQUE_PAYLOAD = 1u, /**< 业务 payload，IPC 层不解释内容。 */
    PUT_SHM_MESSAGE_TYPE_HEARTBEAT = 2u,     /**< payload 为 put_shm_heartbeat_payload_t。 */
    PUT_SHM_MESSAGE_TYPE_STATUS = 3u,        /**< payload 为 put_shm_status_payload_t。 */
    PUT_SHM_MESSAGE_TYPE_EVENT = 4u,         /**< payload 为 put_shm_event_payload_t。 */
    PUT_SHM_MESSAGE_TYPE_HELLO = 5u,         /**< Linux 启动或重启后的握手请求。 */
    PUT_SHM_MESSAGE_TYPE_READY = 6u,         /**< RTOS 完成握手后的 ready 回应。 */
    PUT_SHM_MESSAGE_TYPE_CAN_RX = 7u,        /**< RTOS 回传 CAN RX，payload 为 put_shm_can_rx_payload_t。 */
} put_shm_message_type_t;

/**
 * @brief 共享内存 IPC 事件类型。
 */
typedef enum {
    PUT_SHM_EVENT_NONE = 0u,                    /**< 无事件。 */
    PUT_SHM_EVENT_LINUX_HEARTBEAT_TIMEOUT = 1u, /**< Linux heartbeat 超时。 */
    PUT_SHM_EVENT_CAN_BUS_OFF = 2u,             /**< CAN bus-off。 */
    PUT_SHM_EVENT_SPI_ERROR = 3u,               /**< SPI 或 CAN 控制器访问错误。 */
    PUT_SHM_EVENT_RX_OVERFLOW = 4u,             /**< CAN RX overflow 或处理过载。 */
    PUT_SHM_EVENT_IPC_PAYLOAD_DROP = 5u,        /**< IPC payload 因校验或队列原因被丢弃。 */
} put_shm_event_type_t;

/**
 * @brief heartbeat/hello/ready 公共 payload。
 */
typedef struct PUT_SHM_PACKED {
    uint32_t epoch;        /**< Linux 启动纪元，Linux 重启后必须递增。 */
    uint32_t sequence;     /**< heartbeat 或握手序号，由发送方递增。 */
    uint32_t timestamp_ms; /**< 发送方单调时间戳，单位毫秒。 */
    uint32_t flags;        /**< 控制标志，v1 未定义 bit 必须填 0。 */
} put_shm_heartbeat_payload_t;

/**
 * @brief RTOS -> Linux CAN RX 公共 payload。
 *
 * v1 只冻结经典 CAN 回传。can_flags 为共享内存私有 bitmask。
 */
typedef struct PUT_SHM_PACKED {
    uint32_t sequence;       /**< CAN RX 回传序号，由 RTOS 侧递增。 */
    uint32_t timestamp_ms;   /**< RTOS 接收 CAN 报文的时间戳，单位毫秒。 */
    uint32_t can_id;         /**< CAN 标准 ID 或扩展 ID。 */
    uint8_t can_dlc;         /**< CAN 数据字节数，v1 范围为 0 ~ 8。 */
    uint8_t can_flags;       /**< CAN flag bitmask，v1 只允许标准帧或扩展帧。 */
    uint8_t reserved0[2];    /**< 保留字节，发送方必须填 0。 */
    uint8_t can_data[PUT_SHM_CAN_CLASSIC_DATA_MAX_LEN]; /**< 经典 CAN 数据区。 */
    uint32_t reserved1[4];   /**< 保留字段，发送方必须填 0。 */
} put_shm_can_rx_payload_t;

/**
 * @brief RTOS -> Linux 状态快照公共 payload。
 *
 * 该结构只定义跨核 ABI 必需字段，不直接复用 freertos/ 下的私有状态结构。
 */
typedef struct PUT_SHM_PACKED {
    uint32_t uptime_ms;                /**< RTOS 小核运行时间，单位毫秒。 */
    uint32_t rx_from_linux;            /**< RTOS 从 Linux 通道成功接收的业务消息数。 */
    uint32_t tx_to_can_ok;             /**< RTOS 成功发送到 CAN 的消息数。 */
    uint32_t tx_to_can_fail;           /**< RTOS 发送 CAN 失败次数。 */
    uint32_t rx_from_can;              /**< RTOS 从 CAN 总线接收的消息数。 */
    uint32_t tx_to_linux;              /**< RTOS 成功回传 Linux 的消息数。 */
    uint32_t drop_queue_full;          /**< CAN TX 队列满导致的丢弃次数。 */
    uint32_t drop_ring_full;           /**< 共享内存回传 ring 满导致的丢弃次数。 */
    uint32_t ipc_payload_drop;         /**< IPC payload 校验失败或适配失败次数。 */
    uint32_t rx_overrun;               /**< CAN RX 处理过载次数。 */
    uint32_t spi_error;                /**< SPI 或 CAN 控制器访问错误次数。 */
    uint32_t can_bus_off;              /**< CAN bus-off 次数。 */
    uint32_t linux_heartbeat_timeout;  /**< Linux heartbeat 超时次数。 */
    uint8_t can_ready;                 /**< CAN 驱动是否 ready，0 表示 false，非 0 表示 true。 */
    uint8_t linux_online;              /**< Linux heartbeat/握手状态，0 表示 false，非 0 表示 true。 */
    uint8_t tx_enabled;                /**< CAN TX 路径是否允许发送，0 表示 false，非 0 表示 true。 */
    uint8_t reserved0;                 /**< 保留字节，发送方必须填 0。 */
    uint32_t reserved1[4];             /**< 保留字段，发送方必须填 0。 */
} put_shm_status_payload_t;

/**
 * @brief RTOS -> Linux 错误或状态事件公共 payload。
 */
typedef struct PUT_SHM_PACKED {
    uint32_t event_type;   /**< 事件类型，取值见 put_shm_event_type_t。 */
    uint32_t detail;       /**< 事件附加信息，由 event_type 解释。 */
    uint32_t sequence;     /**< 事件序号，由发送方递增。 */
    uint32_t timestamp_ms; /**< 事件产生时间戳，单位毫秒。 */
} put_shm_event_payload_t;

/**
 * @brief 单个 IPC slot 的固定头部。
 */
typedef struct PUT_SHM_PACKED {
    uint32_t magic;          /**< 固定为 PUT_SHM_SLOT_MAGIC。 */
    uint16_t version;        /**< 固定为 PUT_SHM_IPC_VERSION。 */
    uint16_t header_size;    /**< 固定为 sizeof(put_shm_slot_header_t)。 */
    uint32_t sequence;       /**< slot 序号，通常等于写入时的 write_seq。 */
    uint32_t epoch;          /**< Linux 启动纪元，用于识别重启前旧消息。 */
    uint16_t message_type;   /**< 消息类型，取值见 put_shm_message_type_t。 */
    uint16_t payload_length; /**< payload 实际长度，不能超过 PUT_SHM_PAYLOAD_MAX_LEN。 */
    uint16_t payload_crc16;  /**< payload CRC-16/CCITT-FALSE。 */
    uint16_t flags;          /**< slot 标志位，v1 未定义 bit 必须填 0。 */
    uint32_t reserved[2];    /**< 保留字段，发送方必须填 0。 */
} put_shm_slot_header_t;

/**
 * @brief 固定大小 IPC slot。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    put_shm_slot_header_t header;               /**< slot 头部，描述 payload 元数据。 */
    uint8_t payload[PUT_SHM_PAYLOAD_MAX_LEN];   /**< payload 数据区。 */
    uint8_t reserved[PUT_SHM_SLOT_RESERVED_LEN];/**< 保留区，发送方必须填 0。 */
} put_shm_slot_t;

/**
 * @brief ring 固定 header。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    uint32_t magic;       /**< 固定为 PUT_SHM_RING_MAGIC。 */
    uint16_t version;     /**< 固定为 PUT_SHM_IPC_VERSION。 */
    uint16_t header_size; /**< 固定为 sizeof(put_shm_ring_header_t)。 */
    uint16_t depth;       /**< ring slot 数量，v1 固定为 32。 */
    uint16_t slot_size;   /**< 单个 slot 大小，v1 固定为 256。 */
    uint8_t direction;    /**< ring 方向，取值见 put_shm_direction_t。 */
    uint8_t reserved[51]; /**< 保留字段，初始化时必须填 0。 */
} put_shm_ring_header_t;

/**
 * @brief ring 生产者拥有的 cache line。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    volatile uint32_t write_seq; /**< 生产者写入序号，只允许生产者更新。 */
    uint32_t depth;              /**< ring 深度冗余副本，用于快速校验。 */
    uint32_t slot_size;          /**< slot 大小冗余副本，用于快速校验。 */
    uint32_t drop_count;         /**< 生产者因 ring full 丢弃最新消息的计数。 */
    uint32_t notify_count;       /**< 生产者成功触发 doorbell/mailbox 的计数。 */
    uint32_t notify_fail_count;  /**< 生产者发布消息后 doorbell/mailbox 通知失败的计数。 */
    uint8_t reserved[40];        /**< 保留字段，初始化时必须填 0。 */
} put_shm_ring_producer_t;

/**
 * @brief ring 消费者拥有的 cache line。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    volatile uint32_t read_seq;  /**< 消费者读取序号，只允许消费者更新。 */
    uint32_t dequeue_count;      /**< 消费者成功出队消息的计数。 */
    uint32_t crc_error_count;    /**< 消费者发现 slot payload CRC 错误的计数。 */
    uint32_t format_error_count; /**< 消费者发现 magic/version/length 错误的计数。 */
    uint8_t reserved[48];        /**< 保留字段，初始化时必须填 0。 */
} put_shm_ring_consumer_t;

/**
 * @brief 固定深度 SPSC ring。
 *
 * empty 条件：write_seq == read_seq。
 * full 条件：write_seq - read_seq >= depth。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    put_shm_ring_header_t header;       /**< ring 固定 header。 */
    put_shm_ring_producer_t producer;   /**< 生产者拥有的控制行。 */
    put_shm_ring_consumer_t consumer;   /**< 消费者拥有的控制行。 */
    put_shm_slot_t slots[PUT_SHM_L2R_DEPTH]; /**< 固定 slot 数组，两个方向 v1 深度相同。 */
} put_shm_ring_t;

/**
 * @brief 共享内存区域固定 header。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    uint32_t magic;        /**< 固定为 PUT_SHM_REGION_MAGIC。 */
    uint16_t version;      /**< 固定为 PUT_SHM_IPC_VERSION。 */
    uint16_t header_size;  /**< 固定为 sizeof(put_shm_region_header_t)。 */
    uint32_t region_size;  /**< 固定为 PUT_SHM_REGION_SIZE。 */
    uint32_t l2r_offset;   /**< Linux -> RTOS ring 相对共享内存区域起点的偏移。 */
    uint32_t r2l_offset;   /**< RTOS -> Linux ring 相对共享内存区域起点的偏移。 */
    uint32_t linux_epoch;  /**< Linux 当前启动纪元，Linux 重启后递增。 */
    uint32_t rtos_epoch;   /**< RTOS 当前启动纪元，RTOS 重启后递增。 */
    uint8_t reserved[36];  /**< 保留字段，初始化时必须填 0。 */
} put_shm_region_header_t;

/**
 * @brief 共享内存 IPC 完整区域。
 */
typedef struct PUT_SHM_PACKED_ALIGNED {
    put_shm_region_header_t header;              /**< 共享内存区域 header。 */
    put_shm_ring_t linux_to_rtos;                /**< Linux 写、RTOS 读的业务/控制 ring。 */
    put_shm_ring_t rtos_to_linux;                /**< RTOS 写、Linux 读的 CAN RX/状态/事件 ring。 */
    uint8_t reserved[PUT_SHM_REGION_RESERVED_LEN]; /**< 尾部保留区，初始化时必须填 0。 */
} put_shm_region_t;

PUT_SHM_STATIC_ASSERT(sizeof(put_shm_heartbeat_payload_t) <= PUT_SHM_PAYLOAD_MAX_LEN,
                      "heartbeat payload must fit shared memory payload");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_can_rx_payload_t) <= PUT_SHM_PAYLOAD_MAX_LEN,
                      "CAN RX payload must fit shared memory payload");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_status_payload_t) <= PUT_SHM_PAYLOAD_MAX_LEN,
                      "status payload must fit shared memory payload");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_event_payload_t) <= PUT_SHM_PAYLOAD_MAX_LEN,
                      "event payload must fit shared memory payload");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_slot_header_t) == PUT_SHM_SLOT_HEADER_SIZE,
                      "shared memory slot header must be 32 bytes");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_slot_t) == PUT_SHM_SLOT_SIZE,
                      "shared memory slot must be 256 bytes");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_ring_header_t) == PUT_SHM_RING_HEADER_SIZE,
                      "shared memory ring header must be one cache line");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_ring_producer_t) == PUT_SHM_RING_PRODUCER_SIZE,
                      "shared memory producer state must be one cache line");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_ring_consumer_t) == PUT_SHM_RING_CONSUMER_SIZE,
                      "shared memory consumer state must be one cache line");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_ring_t) == PUT_SHM_RING_SIZE,
                      "shared memory ring size must match ABI constant");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_region_header_t) == PUT_SHM_REGION_HEADER_SIZE,
                      "shared memory region header must be one cache line");
PUT_SHM_STATIC_ASSERT(sizeof(put_shm_region_t) == PUT_SHM_REGION_SIZE,
                      "shared memory region must be 64 KiB");

#ifdef __cplusplus
}
#endif

#endif /* SHARED_MEMORY_IPC_H */
