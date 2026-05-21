/* FreeRTOS comm CAN driver 抽象：只处理小核内部 CAN 消息，不绑定上层协议。 */
#ifndef RTOS_CAN_DRIVER_H
#define RTOS_CAN_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_can_message.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTOS_CAN_DRIVER_ERROR_NONE = 0,
    RTOS_CAN_DRIVER_ERROR_NOT_READY,
    RTOS_CAN_DRIVER_ERROR_LISTEN_ONLY,
    RTOS_CAN_DRIVER_ERROR_NO_RX,
    RTOS_CAN_DRIVER_ERROR_SPI,
    RTOS_CAN_DRIVER_ERROR_BUS_OFF,
    RTOS_CAN_DRIVER_ERROR_TIMEOUT,
} rtos_can_driver_error_t;

typedef struct {
    uint32_t init_count;
    uint32_t send_count;
    uint32_t read_count;
    uint32_t reset_count;
    uint32_t abort_tx_count;
    uint32_t clear_tx_count;
    bool initialized;
    bool listen_only;
    rtos_can_driver_error_t last_error;
} rtos_can_driver_mock_snapshot_t;

unified_error_t rtos_can_driver_init(void);
unified_error_t rtos_can_driver_set_bitrate(uint32_t bitrate);
unified_error_t rtos_can_driver_send(const rtos_can_message_t *message);
unified_error_t rtos_can_driver_read(rtos_can_message_t *out_message);
rtos_can_driver_error_t rtos_can_driver_get_error(void);
unified_error_t rtos_can_driver_abort_tx(void);
unified_error_t rtos_can_driver_clear_tx_buffers(void);
unified_error_t rtos_can_driver_set_listen_only(void);
unified_error_t rtos_can_driver_set_normal(void);
unified_error_t rtos_can_driver_reset(void);
void rtos_can_driver_get_mock_snapshot(rtos_can_driver_mock_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_DRIVER_H */
