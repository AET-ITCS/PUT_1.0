/**
 * @file rtos_recovery.h
 * @brief FreeRTOS comm 恢复状态机。
 *
 * 管理 Linux heartbeat、fail-safe offline、TX 使能状态和恢复握手占位。
 * 本模块不绑定真实 tick 或硬件 watchdog；调用方通过 now_ms 显式传入时间。
 */
#ifndef RTOS_RECOVERY_H
#define RTOS_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化恢复状态机。 */
void rtos_recovery_init(void);

/**
 * @brief 记录一次 Linux heartbeat。
 * @param now_ms 当前时间，单位毫秒。
 */
void rtos_recovery_note_linux_heartbeat(uint32_t now_ms);

/**
 * @brief 单步执行 watchdog/recovery 检查。
 * @param now_ms 当前时间，单位毫秒。
 * @return UNIFIED_OK 表示检查完成，否则返回恢复动作错误码。
 */
unified_error_t rtos_recovery_watchdog_check_once(uint32_t now_ms);

/**
 * @brief 标记 Linux HELLO/READY 重新握手完成。
 * @param now_ms 当前时间，单位毫秒。
 * @return UNIFIED_OK 表示恢复到 Normal/TX enabled，否则返回错误码。
 */
unified_error_t rtos_recovery_complete_linux_rehandshake(uint32_t now_ms);

/**
 * @brief 查询 Linux 是否在线。
 * @return true 表示 Linux online。
 */
bool rtos_recovery_linux_online(void);

/**
 * @brief 查询 TX 路径是否允许发送。
 * @return true 表示可消费 CAN TX 队列。
 */
bool rtos_recovery_tx_enabled(void);

/**
 * @brief 查询是否处于 fail-safe offline。
 * @return true 表示处于 offline 状态。
 */
bool rtos_recovery_is_offline(void);

/**
 * @brief 设置 mock 当前时间，供 Watchdog_Task 占位入口使用。
 * @param now_ms 当前时间，单位毫秒。
 */
void rtos_recovery_mock_set_now(uint32_t now_ms);

/**
 * @brief 使用 mock 当前时间执行一次 watchdog 检查。
 * @return UNIFIED_OK 表示检查完成，否则返回恢复动作错误码。
 */
unified_error_t rtos_recovery_watchdog_task_check_once(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_RECOVERY_H */
