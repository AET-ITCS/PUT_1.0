/**
 * @file rtos_firmware_config.h
 * @brief rtos_firmware 默认配置。
 * @author Yukikaze
 */
#ifndef RTOS_FIRMWARE_CONFIG_H
#define RTOS_FIRMWARE_CONFIG_H

#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 默认 Linux heartbeat 超时时间，单位毫秒。 */
#ifndef RTOS_FIRMWARE_LINUX_HEARTBEAT_TIMEOUT_MS
#define RTOS_FIRMWARE_LINUX_HEARTBEAT_TIMEOUT_MS 3000u
#endif

/** @brief 默认 CAN bitrate，后续真实 CAN 驱动接入时使用。 */
#ifndef RTOS_FIRMWARE_CAN_BITRATE
#define RTOS_FIRMWARE_CAN_BITRATE 500000u
#endif

/** @brief 默认是否允许构建 host smoke executable。 */
#ifndef RTOS_FIRMWARE_HOST_SMOKE_ENABLE
#define RTOS_FIRMWARE_HOST_SMOKE_ENABLE 1u
#endif

/** @brief 板端共享内存物理/总线地址，0 表示 host 或尚未配置。 */
#ifndef RTOS_FIRMWARE_SHARED_MEMORY_BASE
#define RTOS_FIRMWARE_SHARED_MEMORY_BASE 0u
#endif

/** @brief 板端共享内存大小，必须与 PUT_SHM_REGION_SIZE 保持一致。 */
#ifndef RTOS_FIRMWARE_SHARED_MEMORY_SIZE
#define RTOS_FIRMWARE_SHARED_MEMORY_SIZE PUT_SHM_REGION_SIZE
#endif

/** @brief P1 本地优先级队列固定容量。 */
#ifndef RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY
#define RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY 64u
#endif

/** @brief P1 支持的优先级数量。 */
#ifndef RTOS_FIRMWARE_PRIORITY_COUNT
#define RTOS_FIRMWARE_PRIORITY_COUNT 4u
#endif

/** @brief 优先级 0 每轮默认调度配额。 */
#ifndef RTOS_FIRMWARE_PRIORITY_0_QUOTA
#define RTOS_FIRMWARE_PRIORITY_0_QUOTA 16u
#endif

/** @brief 优先级 1 每轮默认调度配额。 */
#ifndef RTOS_FIRMWARE_PRIORITY_1_QUOTA
#define RTOS_FIRMWARE_PRIORITY_1_QUOTA 12u
#endif

/** @brief 优先级 2 每轮默认调度配额。 */
#ifndef RTOS_FIRMWARE_PRIORITY_2_QUOTA
#define RTOS_FIRMWARE_PRIORITY_2_QUOTA 8u
#endif

/** @brief 优先级 3 每轮默认调度配额。 */
#ifndef RTOS_FIRMWARE_PRIORITY_3_QUOTA
#define RTOS_FIRMWARE_PRIORITY_3_QUOTA 4u
#endif

/** @brief P1 端心跳表固定容量。 */
#ifndef RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY
#define RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY 64u
#endif

/** @brief P1 默认路由表版本。 */
#ifndef RTOS_FIRMWARE_ROUTE_TABLE_VERSION
#define RTOS_FIRMWARE_ROUTE_TABLE_VERSION 1u
#endif

/** @brief P1 默认生效路由 epoch。 */
#ifndef RTOS_FIRMWARE_ROUTE_EPOCH
#define RTOS_FIRMWARE_ROUTE_EPOCH 1u
#endif

/** @brief P2 单接口默认 RX drain budget。 */
#ifndef RTOS_FIRMWARE_RX_DRAIN_BUDGET_PER_INTERFACE
#define RTOS_FIRMWARE_RX_DRAIN_BUDGET_PER_INTERFACE 8u
#endif

/** @brief P2 单轮所有接口默认 RX drain 总 budget。 */
#ifndef RTOS_FIRMWARE_RX_DRAIN_BUDGET_TOTAL
#define RTOS_FIRMWARE_RX_DRAIN_BUDGET_TOTAL 64u
#endif

/** @brief P2 reclaim ring 满时本地冻结的 reclaim 输出容量。 */
#ifndef RTOS_FIRMWARE_PENDING_RECLAIM_CAPACITY
#define RTOS_FIRMWARE_PENDING_RECLAIM_CAPACITY PUT_SHM_FRAME_POOL_BLOCK_COUNT
#endif

/** @brief P3 Linux heartbeat warning 阈值，单位毫秒。 */
#ifndef RTOS_FIRMWARE_LINUX_HEARTBEAT_WARNING_MS
#define RTOS_FIRMWARE_LINUX_HEARTBEAT_WARNING_MS 300u
#endif

/** @brief P3 Linux heartbeat suspected abnormal 阈值，单位毫秒。 */
#ifndef RTOS_FIRMWARE_LINUX_HEARTBEAT_SUSPECT_MS
#define RTOS_FIRMWARE_LINUX_HEARTBEAT_SUSPECT_MS 500u
#endif

/** @brief P3 Linux heartbeat 全局降级阈值，单位毫秒。 */
#ifndef RTOS_FIRMWARE_LINUX_HEARTBEAT_DEGRADED_MS
#define RTOS_FIRMWARE_LINUX_HEARTBEAT_DEGRADED_MS 1000u
#endif

/** @brief P3 endpoint heartbeat warning 阈值，单位毫秒。 */
#ifndef RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_WARN_MS
#define RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_WARN_MS 3000u
#endif

/** @brief P3 endpoint heartbeat offline 阈值，单位毫秒。 */
#ifndef RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_OFFLINE_MS
#define RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_OFFLINE_MS 5000u
#endif

/** @brief P3 ring/pending 长时间未恢复告警阈值，单位毫秒。 */
#ifndef RTOS_FIRMWARE_MONITOR_STUCK_WARN_MS
#define RTOS_FIRMWARE_MONITOR_STUCK_WARN_MS 300u
#endif

/** @brief P3 统计 latency 滑动窗口容量。 */
#ifndef RTOS_FIRMWARE_LATENCY_WINDOW_CAPACITY
#define RTOS_FIRMWARE_LATENCY_WINDOW_CAPACITY 32u
#endif

#ifdef __cplusplus
}
#endif

#endif /* RTOS_FIRMWARE_CONFIG_H */
