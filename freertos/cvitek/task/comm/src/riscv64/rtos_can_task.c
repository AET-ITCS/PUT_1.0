/**
 * @file rtos_can_task.c
 * @brief FreeRTOS comm task 占位实现。
 *
 * 当前任务入口以非阻塞单步 helper 实现，便于 host 测试；后续接入真实
 * FreeRTOS scheduler 后，可在任务循环中复用这些 helper。
 */
#include "rtos_can_task.h"

#include <stdbool.h>

#include "rtos_can_driver.h"
#include "rtos_can_forward.h"
#include "rtos_ipc.h"
#include "rtos_protocol_adapter.h"
#include "rtos_recovery.h"
#include "rtos_status.h"

static bool g_gpio14_rx_pending;

static void send_task_event(uint32_t event_type, uint32_t detail)
{
    rtos_ipc_event_t event;

    event.event_type = event_type;
    event.detail = detail;
    (void)rtos_ipc_send_error_event(&event);
}

unified_error_t rtos_can_task_init_gpio14_irq(void)
{
    g_gpio14_rx_pending = false;
    return UNIFIED_OK;
}

void rtos_can_task_gpio14_irq_notify(void)
{
    g_gpio14_rx_pending = true;
}

bool rtos_can_task_gpio14_irq_is_pending(void)
{
    return g_gpio14_rx_pending;
}

uint32_t rtos_can_task_gateway_ipc_drain_once(void)
{
    rtos_ipc_payload_t payload;
    uint32_t drained = 0u;

    while (rtos_ipc_poll_linux_payload(&payload) == UNIFIED_OK) {
        rtos_can_message_t message;

        if (rtos_protocol_adapter_linux_payload_to_can(&payload, &message) == UNIFIED_OK) {
            (void)rtos_can_forward_submit_message(&message);
        } else {
            rtos_status_inc_ipc_payload_drop();
        }

        ++drained;
    }

    return drained;
}

uint32_t rtos_can_task_can_rx_drain_once(void)
{
    rtos_can_message_t message;
    rtos_can_driver_health_t health;
    uint32_t drained = 0u;

    if (!g_gpio14_rx_pending) {
        return 0u;
    }

    g_gpio14_rx_pending = false;
    if (rtos_can_driver_get_health(&health) == UNIFIED_OK) {
        if (health.rx_overflow) {
            rtos_status_inc_rx_overrun();
            rtos_status_inc_xl2515_rx_overflow();
            send_task_event(RTOS_IPC_EVENT_RX_OVERFLOW, 0u);
        }

        if (health.error_passive) {
            rtos_status_inc_can_error_passive();
        }
    }

    while (rtos_can_driver_read(&message) == UNIFIED_OK) {
        rtos_status_inc_rx_from_can();
        (void)rtos_ipc_send_can_rx(&message);
        ++drained;
    }

    return drained;
}

unified_error_t rtos_can_task_status_report_once(void)
{
    rtos_status_snapshot_t snapshot;

    rtos_status_get_snapshot(&snapshot);
    return rtos_ipc_send_status(&snapshot);
}

void Gateway_IPC_Task(void *parameters)
{
    (void)parameters;
    (void)rtos_can_task_gateway_ipc_drain_once();
}

void CAN_TX_Task(void *parameters)
{
    (void)parameters;
    (void)rtos_can_forward_drain_tx_queue_once();
}

void CAN_RX_Task(void *parameters)
{
    (void)parameters;
    (void)rtos_can_task_can_rx_drain_once();
}

void Status_Task(void *parameters)
{
    (void)parameters;
    (void)rtos_can_task_status_report_once();
}

void Watchdog_Task(void *parameters)
{
    (void)parameters;
    (void)rtos_recovery_watchdog_task_check_once();
}
