/* FreeRTOS comm 任务占位：后续接入真实 scheduler 和队列阻塞等待。 */
#include "rtos_can_task.h"

#include "rtos_can_forward.h"

void Gateway_IPC_Task(void *parameters)
{
    (void)parameters;
}

void CAN_TX_Task(void *parameters)
{
    (void)parameters;
}

void CAN_RX_Task(void *parameters)
{
    (void)parameters;
}

void Status_Task(void *parameters)
{
    (void)parameters;
}

void Watchdog_Task(void *parameters)
{
    (void)parameters;
}
