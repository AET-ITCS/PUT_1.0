/* FreeRTOS comm 转发主逻辑：mock 阶段同步消费协议适配层输出的 CAN TX 队列。 */
#include "rtos_can_forward.h"

#include <stdbool.h>
#include <string.h>

#include "rtos_can_driver.h"
#include "rtos_can_task.h"
#include "rtos_config.h"
#include "rtos_ipc.h"
#include "rtos_protocol_adapter.h"
#include "rtos_recovery.h"
#include "rtos_status.h"

typedef struct {
    rtos_can_message_t slots[RTOS_CAN_TX_QUEUE_LEN];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} rtos_can_tx_queue_t;

static rtos_can_tx_queue_t g_tx_queue;

static void tx_queue_reset(void)
{
    memset(&g_tx_queue, 0, sizeof(g_tx_queue));
}

static bool tx_queue_push(const rtos_can_message_t *message)
{
    if ((message == NULL) || (g_tx_queue.count >= RTOS_CAN_TX_QUEUE_LEN)) {
        return false;
    }

    g_tx_queue.slots[g_tx_queue.tail] = *message;
    g_tx_queue.tail = (g_tx_queue.tail + 1u) % RTOS_CAN_TX_QUEUE_LEN;
    ++g_tx_queue.count;
    return true;
}

static bool tx_queue_pop(rtos_can_message_t *out_message)
{
    if ((out_message == NULL) || (g_tx_queue.count == 0u)) {
        return false;
    }

    *out_message = g_tx_queue.slots[g_tx_queue.head];
    g_tx_queue.head = (g_tx_queue.head + 1u) % RTOS_CAN_TX_QUEUE_LEN;
    --g_tx_queue.count;
    return true;
}

static bool driver_error_is_retryable(rtos_can_driver_error_t error)
{
    return (error == RTOS_CAN_DRIVER_ERROR_SPI) ||
           (error == RTOS_CAN_DRIVER_ERROR_TIMEOUT);
}

static void report_driver_event(uint32_t event_type, uint32_t detail)
{
    rtos_ipc_event_t event;

    event.event_type = event_type;
    event.detail = detail;
    (void)rtos_ipc_send_error_event(&event);
}

static void recover_after_driver_error(rtos_can_driver_error_t error)
{
    if ((error == RTOS_CAN_DRIVER_ERROR_SPI) ||
        (error == RTOS_CAN_DRIVER_ERROR_TIMEOUT)) {
        rtos_status_inc_spi_error();
        if (rtos_can_driver_reset() == UNIFIED_OK) {
            rtos_status_set_can_ready(true);
        } else {
            rtos_status_set_can_ready(false);
        }
        report_driver_event(RTOS_IPC_EVENT_SPI_ERROR, (uint32_t)error);
    } else if (error == RTOS_CAN_DRIVER_ERROR_BUS_OFF) {
        rtos_status_inc_can_bus_off();
        if ((rtos_can_driver_reset() == UNIFIED_OK) &&
            (rtos_can_driver_set_normal() == UNIFIED_OK)) {
            rtos_status_set_can_ready(true);
        } else {
            rtos_status_set_can_ready(false);
        }
        report_driver_event(RTOS_IPC_EVENT_CAN_BUS_OFF, (uint32_t)error);
    }
}

static unified_error_t send_message_with_recovery(const rtos_can_message_t *message)
{
    uint32_t retry_count = 0u;

    while (true) {
        rtos_can_driver_error_t error;

        if (rtos_can_driver_send(message) == UNIFIED_OK) {
            rtos_status_inc_tx_to_can_ok();
            return UNIFIED_OK;
        }

        error = rtos_can_driver_get_error();
        rtos_status_inc_tx_to_can_fail();

        if (driver_error_is_retryable(error) && (retry_count < RTOS_CAN_TX_RETRY_MAX)) {
            ++retry_count;
            continue;
        }

        recover_after_driver_error(error);
        return UNIFIED_ERR_INVALID_ARG;
    }
}

uint32_t rtos_can_forward_drain_tx_queue_once(void)
{
    rtos_can_message_t message;
    uint32_t drained = 0u;

    if (!rtos_recovery_tx_enabled()) {
        return 0u;
    }

    while (tx_queue_pop(&message)) {
        (void)send_message_with_recovery(&message);
        ++drained;
    }

    return drained;
}

static bool can_message_flags_are_supported(uint8_t can_flags)
{
    return (can_flags & (uint8_t)~RTOS_CAN_FLAG_EXTENDED_ID) == 0u;
}

static bool can_message_id_is_valid(uint32_t can_id, uint8_t can_flags)
{
    if ((can_flags & (uint8_t)RTOS_CAN_FLAG_EXTENDED_ID) != 0u) {
        return can_id <= RTOS_CAN_EXTENDED_ID_MAX;
    }

    return can_id <= RTOS_CAN_STANDARD_ID_MAX;
}

static unified_error_t validate_can_message(const rtos_can_message_t *message)
{
    if (message == NULL) {
        rtos_status_inc_drop_null();
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (!can_message_flags_are_supported(message->can_flags)) {
        rtos_status_inc_drop_flag();
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (!can_message_id_is_valid(message->can_id, message->can_flags)) {
        rtos_status_inc_drop_can_id();
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (message->can_dlc > RTOS_CAN_CLASSIC_DATA_MAX_LEN) {
        rtos_status_inc_drop_dlc();
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

uint32_t rtos_can_forward_purge_tx_queue(void)
{
    uint32_t purged = g_tx_queue.count;

    tx_queue_reset();
    rtos_status_add_tx_queue_purged(purged);
    return purged;
}

uint32_t rtos_can_forward_get_tx_queue_depth(void)
{
    return g_tx_queue.count;
}

unified_error_t gateway_forward_init(void)
{
    unified_error_t result;

    rtos_status_init();
    rtos_protocol_adapter_init();
    tx_queue_reset();

    result = rtos_ipc_init();
    if (result != UNIFIED_OK) {
        return result;
    }

    result = rtos_can_driver_init();
    if (result != UNIFIED_OK) {
        rtos_status_set_can_ready(false);
        return result;
    }

    result = rtos_can_driver_set_bitrate(RTOS_CAN_BITRATE);
    if (result != UNIFIED_OK) {
        rtos_status_set_can_ready(false);
        return result;
    }

    result = rtos_can_task_init_gpio14_irq();
    if (result != UNIFIED_OK) {
        rtos_status_set_can_ready(false);
        return result;
    }

    rtos_recovery_init();
    rtos_status_set_can_ready(true);
    return UNIFIED_OK;
}

unified_error_t rtos_can_forward_enqueue_message(const rtos_can_message_t *message)
{
    unified_error_t validate_result;

    if (!rtos_recovery_tx_enabled()) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    validate_result = validate_can_message(message);
    if (validate_result != UNIFIED_OK) {
        return validate_result;
    }

    if (!tx_queue_push(message)) {
        rtos_status_inc_drop_queue_full();
        return UNIFIED_ERR_INVALID_ARG;
    }

    rtos_status_inc_rx_from_linux();
    return UNIFIED_OK;
}

unified_error_t rtos_can_forward_submit_message(const rtos_can_message_t *message)
{
    unified_error_t result;

    result = rtos_can_forward_enqueue_message(message);
    if (result != UNIFIED_OK) {
        return result;
    }

    (void)rtos_can_forward_drain_tx_queue_once();
    return UNIFIED_OK;
}
