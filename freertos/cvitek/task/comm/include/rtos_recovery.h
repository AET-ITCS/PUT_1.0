/* FreeRTOS comm 恢复状态机：heartbeat、fail-safe offline 和重新握手占位。 */
#ifndef RTOS_RECOVERY_H
#define RTOS_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

void rtos_recovery_init(void);
void rtos_recovery_note_linux_heartbeat(uint32_t now_ms);
unified_error_t rtos_recovery_watchdog_check_once(uint32_t now_ms);
unified_error_t rtos_recovery_complete_linux_rehandshake(uint32_t now_ms);
bool rtos_recovery_linux_online(void);
bool rtos_recovery_tx_enabled(void);
bool rtos_recovery_is_offline(void);
void rtos_recovery_mock_set_now(uint32_t now_ms);
unified_error_t rtos_recovery_watchdog_task_check_once(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_RECOVERY_H */
