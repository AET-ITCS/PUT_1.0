/* FreeRTOS comm 任务入口占位：后续接入真实 FreeRTOS xTaskCreate。 */
#ifndef RTOS_CAN_TASK_H
#define RTOS_CAN_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

void Gateway_IPC_Task(void *parameters);
void CAN_TX_Task(void *parameters);
void CAN_RX_Task(void *parameters);
void Status_Task(void *parameters);
void Watchdog_Task(void *parameters);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_TASK_H */
