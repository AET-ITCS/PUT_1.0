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

/**
 * @brief 初始化 GPIO14/XL2515 INT# 中断占位状态。
 * @return UNIFIED_OK 表示成功。
 */
unified_error_t rtos_can_task_init_gpio14_irq(void);

/**
 * @brief GPIO14 ISR 通知入口。
 *
 * ISR 中只设置 pending 标志，不进行 SPI 读写。
 */
void rtos_can_task_gpio14_irq_notify(void);

/**
 * @brief 查询 GPIO14 RX pending 标志。
 * @return true 表示 CAN_RX_Task 需要 drain RX buffer。
 */
bool rtos_can_task_gpio14_irq_is_pending(void);

/**
 * @brief 单步 drain Linux->RTOS mock payload queue。
 * @return 本次处理的 payload 数量。
 */
uint32_t rtos_can_task_gateway_ipc_drain_once(void);

/**
 * @brief 单步 drain CAN RX。
 * @return 本次读取并回传的 CAN RX 报文数量。
 */
uint32_t rtos_can_task_can_rx_drain_once(void);

/**
 * @brief 单次发送状态快照到 RTOS->Linux 回传占位接口。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_task_status_report_once(void);

/** @brief Gateway IPC 任务入口。 */
void Gateway_IPC_Task(void *parameters);

/** @brief CAN TX 任务入口。 */
void CAN_TX_Task(void *parameters);

/** @brief CAN RX 任务入口。 */
void CAN_RX_Task(void *parameters);

/** @brief 状态上报任务入口。 */
void Status_Task(void *parameters);

/** @brief Watchdog/recovery 检查任务入口。 */
void Watchdog_Task(void *parameters);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_TASK_H */
