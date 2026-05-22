/**
 * @file rtos_recovery.c
 * @brief FreeRTOS comm recovery 状态机实现。
 *
 * 负责 Linux heartbeat timeout、fail-safe offline、Listen-Only 切换和
 * Linux 重新握手恢复入口。时间由调用方显式传入，避免绑定具体 BSP tick。
 */
#include "rtos_recovery.h"

#include "rtos_can_driver.h"
#include "rtos_can_forward.h"
#include "rtos_config.h"
#include "rtos_ipc.h"
#include "rtos_status.h"

/** @brief recovery 运行状态。 */
typedef struct {
    /** @brief Linux 当前是否在线。 */
    bool linux_online;
    /** @brief CAN TX 路径是否允许发送。 */
    bool tx_enabled;
    /** @brief 是否处于 fail-safe offline。 */
    bool offline;
    /** @brief 最近一次 Linux heartbeat 时间戳，单位毫秒。 */
    uint32_t last_linux_heartbeat_ms;
    /** @brief host/mock Watchdog_Task 使用的当前时间。 */
    uint32_t mock_now_ms;
} rtos_recovery_state_t;

static rtos_recovery_state_t g_recovery;

static bool elapsed_ms_exceeds(uint32_t now_ms, uint32_t then_ms, uint32_t timeout_ms)
{
    return (uint32_t)(now_ms - then_ms) > timeout_ms;
}

static void send_recovery_event(uint32_t event_type, uint32_t detail)
{
    rtos_ipc_event_t event;

    event.event_type = event_type;
    event.detail = detail;
    (void)rtos_ipc_send_error_event(&event);
}

static unified_error_t enter_fail_safe_offline(uint32_t event_type, uint32_t detail)
{
    unified_error_t result = UNIFIED_OK;

    if (g_recovery.offline) {
        return UNIFIED_OK;
    }

    g_recovery.offline = true;
    g_recovery.tx_enabled = false;
    g_recovery.linux_online = false;
    rtos_status_set_linux_online(false);
    rtos_status_inc_linux_offline_enter();

    (void)rtos_can_forward_purge_tx_queue();

    if (rtos_can_driver_abort_tx() == UNIFIED_OK) {
        rtos_status_inc_xl2515_tx_aborted();
    } else {
        result = UNIFIED_ERR_INVALID_ARG;
    }

    if (rtos_can_driver_clear_tx_buffers() != UNIFIED_OK) {
        result = UNIFIED_ERR_INVALID_ARG;
    }

    if (RTOS_FAIL_SAFE_LISTEN_ONLY_ENABLE != 0u) {
        if (rtos_can_driver_set_listen_only() == UNIFIED_OK) {
            rtos_status_inc_listen_only_enter();
        } else {
            result = UNIFIED_ERR_INVALID_ARG;
        }
    }

    send_recovery_event(event_type, detail);
    return result;
}

/** @brief 初始化恢复状态机。 */
void rtos_recovery_init(void)
{
    g_recovery.linux_online = true;
    g_recovery.tx_enabled = true;
    g_recovery.offline = false;
    g_recovery.last_linux_heartbeat_ms = 0u;
    g_recovery.mock_now_ms = 0u;
    rtos_status_set_linux_online(true);
}

/**
 * @brief 记录一次 Linux heartbeat。
 * @param now_ms 当前时间，单位毫秒。
 */
void rtos_recovery_note_linux_heartbeat(uint32_t now_ms)
{
    g_recovery.last_linux_heartbeat_ms = now_ms;
    g_recovery.mock_now_ms = now_ms;

    if (!g_recovery.offline) {
        g_recovery.linux_online = true;
        rtos_status_set_linux_online(true);
    }
}

/**
 * @brief 单步执行 watchdog/recovery 检查。
 * @param now_ms 当前时间，单位毫秒。
 * @return UNIFIED_OK 表示检查完成，否则返回恢复动作错误码。
 */
unified_error_t rtos_recovery_watchdog_check_once(uint32_t now_ms)
{
    g_recovery.mock_now_ms = now_ms;

    if (!g_recovery.offline &&
        elapsed_ms_exceeds(now_ms,
                           g_recovery.last_linux_heartbeat_ms,
                           RTOS_LINUX_HEARTBEAT_TIMEOUT_MS)) {
        rtos_status_inc_linux_heartbeat_timeout();
        return enter_fail_safe_offline(RTOS_IPC_EVENT_LINUX_HEARTBEAT_TIMEOUT, now_ms);
    }

    return UNIFIED_OK;
}

/**
 * @brief 标记 Linux HELLO/READY 重新握手完成。
 * @param now_ms 当前时间，单位毫秒。
 * @return UNIFIED_OK 表示恢复到 Normal/TX enabled，否则返回错误码。
 */
unified_error_t rtos_recovery_complete_linux_rehandshake(uint32_t now_ms)
{
    g_recovery.mock_now_ms = now_ms;

    if (rtos_can_driver_set_normal() != UNIFIED_OK) {
        g_recovery.offline = true;
        g_recovery.tx_enabled = false;
        g_recovery.linux_online = false;
        rtos_status_set_linux_online(false);
        return UNIFIED_ERR_INVALID_ARG;
    }

    g_recovery.offline = false;
    g_recovery.tx_enabled = true;
    g_recovery.linux_online = true;
    g_recovery.last_linux_heartbeat_ms = now_ms;
    rtos_status_set_linux_online(true);
    rtos_status_inc_linux_rehandshake_ok();
    return UNIFIED_OK;
}

/**
 * @brief 查询 Linux 是否在线。
 * @return true 表示 Linux online。
 */
bool rtos_recovery_linux_online(void)
{
    return g_recovery.linux_online;
}

/**
 * @brief 查询 TX 路径是否允许发送。
 * @return true 表示可消费 CAN TX 队列。
 */
bool rtos_recovery_tx_enabled(void)
{
    return g_recovery.tx_enabled && !g_recovery.offline;
}

/**
 * @brief 查询是否处于 fail-safe offline。
 * @return true 表示处于 offline 状态。
 */
bool rtos_recovery_is_offline(void)
{
    return g_recovery.offline;
}

/**
 * @brief 设置 mock 当前时间，供 Watchdog_Task 占位入口使用。
 * @param now_ms 当前时间，单位毫秒。
 */
void rtos_recovery_mock_set_now(uint32_t now_ms)
{
    g_recovery.mock_now_ms = now_ms;
}

/**
 * @brief 使用 mock 当前时间执行一次 watchdog 检查。
 * @return UNIFIED_OK 表示检查完成，否则返回恢复动作错误码。
 */
unified_error_t rtos_recovery_watchdog_task_check_once(void)
{
    return rtos_recovery_watchdog_check_once(g_recovery.mock_now_ms);
}
