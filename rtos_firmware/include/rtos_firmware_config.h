/**
 * @file rtos_firmware_config.h
 * @brief rtos_firmware 默认配置。
 * @author Yukikaze
 */
#ifndef RTOS_FIRMWARE_CONFIG_H
#define RTOS_FIRMWARE_CONFIG_H

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

/** @brief P1 本地 priority 队列固定容量。 */
#ifndef RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY
#define RTOS_FIRMWARE_PRIORITY_QUEUE_CAPACITY 64u
#endif

/** @brief P1 支持的 priority 数量。 */
#ifndef RTOS_FIRMWARE_PRIORITY_COUNT
#define RTOS_FIRMWARE_PRIORITY_COUNT 4u
#endif

/** @brief priority 0 每轮默认调度配额。 */
#ifndef RTOS_FIRMWARE_PRIORITY_0_QUOTA
#define RTOS_FIRMWARE_PRIORITY_0_QUOTA 16u
#endif

/** @brief priority 1 每轮默认调度配额。 */
#ifndef RTOS_FIRMWARE_PRIORITY_1_QUOTA
#define RTOS_FIRMWARE_PRIORITY_1_QUOTA 12u
#endif

/** @brief priority 2 每轮默认调度配额。 */
#ifndef RTOS_FIRMWARE_PRIORITY_2_QUOTA
#define RTOS_FIRMWARE_PRIORITY_2_QUOTA 8u
#endif

/** @brief priority 3 每轮默认调度配额。 */
#ifndef RTOS_FIRMWARE_PRIORITY_3_QUOTA
#define RTOS_FIRMWARE_PRIORITY_3_QUOTA 4u
#endif

/** @brief P1 endpoint heartbeat 表固定容量。 */
#ifndef RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY
#define RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY 64u
#endif

/** @brief P1 默认 route table version。 */
#ifndef RTOS_FIRMWARE_ROUTE_TABLE_VERSION
#define RTOS_FIRMWARE_ROUTE_TABLE_VERSION 1u
#endif

/** @brief P1 默认 active route epoch。 */
#ifndef RTOS_FIRMWARE_ROUTE_EPOCH
#define RTOS_FIRMWARE_ROUTE_EPOCH 1u
#endif

#ifdef __cplusplus
}
#endif

#endif /* RTOS_FIRMWARE_CONFIG_H */
