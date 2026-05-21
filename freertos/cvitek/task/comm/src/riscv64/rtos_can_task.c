/* FreeRTOS comm 任务占位：后续接入真实 scheduler 和队列阻塞等待。 */
#include "rtos_can_task.h"

#include <stdbool.h>

#include "rtos_can_driver.h"
#include "rtos_can_forward.h"
#include "rtos_ipc.h"
#include "rtos_protocol_adapter.h"
#include "rtos_status.h"

static bool g_gpio14_rx_pending;

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
    uint32_t drained = 0u;

    if (!g_gpio14_rx_pending) {
        return 0u;
    }

    g_gpio14_rx_pending = false;
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
}
