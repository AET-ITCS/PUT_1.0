/* FreeRTOS comm CAN driver mock：后续替换为 XL2515 SPI 实现。 */
#include "rtos_can_driver.h"

#include <string.h>

static rtos_can_driver_mock_snapshot_t g_driver;
static uint32_t g_bitrate;

unified_error_t rtos_can_driver_init(void)
{
    memset(&g_driver, 0, sizeof(g_driver));
    g_driver.initialized = true;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    g_bitrate = 0u;
    ++g_driver.init_count;
    return UNIFIED_OK;
}

unified_error_t rtos_can_driver_set_bitrate(uint32_t bitrate)
{
    if (!g_driver.initialized || (bitrate == 0u)) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    g_bitrate = bitrate;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

unified_error_t rtos_can_driver_send(const rtos_can_message_t *message)
{
    if (message == NULL) {
        return UNIFIED_ERR_NULL;
    }

    if (!g_driver.initialized) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (g_driver.listen_only) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_LISTEN_ONLY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    ++g_driver.send_count;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

unified_error_t rtos_can_driver_read(rtos_can_message_t *out_message)
{
    if (out_message == NULL) {
        return UNIFIED_ERR_NULL;
    }

    memset(out_message, 0, sizeof(*out_message));
    ++g_driver.read_count;
    return UNIFIED_OK;
}

rtos_can_driver_error_t rtos_can_driver_get_error(void)
{
    return g_driver.last_error;
}

unified_error_t rtos_can_driver_abort_tx(void)
{
    ++g_driver.abort_tx_count;
    return UNIFIED_OK;
}

unified_error_t rtos_can_driver_clear_tx_buffers(void)
{
    ++g_driver.clear_tx_count;
    return UNIFIED_OK;
}

unified_error_t rtos_can_driver_set_listen_only(void)
{
    if (!g_driver.initialized) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    g_driver.listen_only = true;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

unified_error_t rtos_can_driver_set_normal(void)
{
    if (!g_driver.initialized) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    g_driver.listen_only = false;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

unified_error_t rtos_can_driver_reset(void)
{
    ++g_driver.reset_count;
    g_driver.initialized = true;
    g_driver.listen_only = false;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

void rtos_can_driver_get_mock_snapshot(rtos_can_driver_mock_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return;
    }

    *out_snapshot = g_driver;
    (void)g_bitrate;
}
