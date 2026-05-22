/**
 * @file rtos_can_task.h
 * @brief FreeRTOS comm 任务入口和 host 可测试 helper。
 *
 * 当前文件提供真实任务函数名和非阻塞单步 helper。host 测试直接调用
 * helper；硬件目标后续由 FreeRTOS scheduler 周期或事件驱动调用任务函数。
 */
#ifndef RTOS_CAN_TASK_H
#define RTOS_CAN_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

unified_error_t rtos_can_task_init_gpio14_irq(void);

void rtos_can_task_gpio14_irq_notify(void);

bool rtos_can_task_gpio14_irq_is_pending(void);

uint32_t rtos_can_task_gateway_ipc_drain_once(void);

uint32_t rtos_can_task_can_rx_drain_once(void);

unified_error_t rtos_can_task_status_report_once(void);

void Gateway_IPC_Task(void *parameters);

void CAN_TX_Task(void *parameters);

void CAN_RX_Task(void *parameters);

void Status_Task(void *parameters);

void Watchdog_Task(void *parameters);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_TASK_H */
