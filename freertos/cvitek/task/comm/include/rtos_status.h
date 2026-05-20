/* FreeRTOS comm 状态统计：记录 mock 链路和帧校验结果。 */
#ifndef RTOS_STATUS_H
#define RTOS_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "rtos_gateway_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t rx_from_linux;
    uint32_t tx_to_can_ok;
    uint32_t tx_to_can_fail;
    uint32_t drop_null;
    uint32_t drop_magic;
    uint32_t drop_version;
    uint32_t drop_type;
    uint32_t drop_source_protocol;
    uint32_t drop_vehicle_type;
    uint32_t drop_flag;
    uint32_t drop_can_id;
    uint32_t drop_dlc;
    uint32_t drop_crc;
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
void rtos_status_record_validation_error(rtos_frame_validate_error_t error);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_STATUS_H */
