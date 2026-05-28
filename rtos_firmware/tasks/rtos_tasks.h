/**
 * @file rtos_tasks.h
 * @brief P1 task entry placeholder API.
 * @author Yukikaze
 */
#ifndef RTOS_TASKS_H
#define RTOS_TASKS_H

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

unified_error_t rtos_ipc_event_task_p1_placeholder(void);
unified_error_t rtos_router_scheduler_task_p1_placeholder(void);
unified_error_t rtos_tx_writer_task_p1_placeholder(void);
unified_error_t rtos_heartbeat_task_p1_placeholder(void);
unified_error_t rtos_error_monitor_task_p1_placeholder(void);
unified_error_t rtos_recovery_task_p1_placeholder(void);
unified_error_t rtos_statistics_task_p1_placeholder(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_TASKS_H */
