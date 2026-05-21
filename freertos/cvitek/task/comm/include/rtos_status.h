/* FreeRTOS comm 状态统计：记录 CAN 层 mock 链路结果。 */
#ifndef RTOS_STATUS_H
#define RTOS_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t rx_from_linux;
    uint32_t tx_to_can_ok;
    uint32_t tx_to_can_fail;
    uint32_t drop_null;
    uint32_t drop_flag;
    uint32_t drop_can_id;
    uint32_t drop_dlc;
    uint32_t drop_queue_full;
    bool can_ready;
    bool linux_online;
} rtos_status_snapshot_t;

void rtos_status_init(void);
void rtos_status_reset(void);
void rtos_status_get_snapshot(rtos_status_snapshot_t *out_snapshot);
void rtos_status_set_can_ready(bool ready);
void rtos_status_set_linux_online(bool online);
void rtos_status_inc_rx_from_linux(void);
void rtos_status_inc_tx_to_can_ok(void);
void rtos_status_inc_tx_to_can_fail(void);
void rtos_status_inc_drop_queue_full(void);
void rtos_status_inc_drop_null(void);
void rtos_status_inc_drop_flag(void);
void rtos_status_inc_drop_can_id(void);
void rtos_status_inc_drop_dlc(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_STATUS_H */
