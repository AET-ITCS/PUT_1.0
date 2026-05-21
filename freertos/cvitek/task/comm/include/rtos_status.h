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
    uint32_t rx_from_can;
    uint32_t tx_to_linux;
    uint32_t drop_ring_full;
    uint32_t ipc_payload_drop;
    uint32_t rx_overrun;
    uint32_t xl2515_rx_overflow;
    uint32_t spi_error;
    uint32_t can_bus_off;
    uint32_t can_error_passive;
    uint32_t linux_heartbeat_timeout;
    uint32_t linux_offline_enter;
    uint32_t tx_queue_purged;
    uint32_t xl2515_tx_aborted;
    uint32_t listen_only_enter;
    uint32_t linux_rehandshake_ok;
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
void rtos_status_inc_rx_from_can(void);
void rtos_status_inc_tx_to_linux(void);
void rtos_status_inc_drop_ring_full(void);
void rtos_status_inc_ipc_payload_drop(void);
void rtos_status_inc_rx_overrun(void);
void rtos_status_inc_xl2515_rx_overflow(void);
void rtos_status_inc_spi_error(void);
void rtos_status_inc_can_bus_off(void);
void rtos_status_inc_can_error_passive(void);
void rtos_status_inc_linux_heartbeat_timeout(void);
void rtos_status_inc_linux_offline_enter(void);
void rtos_status_add_tx_queue_purged(uint32_t count);
void rtos_status_inc_xl2515_tx_aborted(void);
void rtos_status_inc_listen_only_enter(void);
void rtos_status_inc_linux_rehandshake_ok(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_STATUS_H */
