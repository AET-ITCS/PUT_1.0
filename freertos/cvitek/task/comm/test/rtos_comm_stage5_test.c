#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtos_can_driver.h"
#include "rtos_can_forward.h"
#include "rtos_can_task.h"
#include "rtos_config.h"
#include "rtos_ipc.h"
#include "rtos_recovery.h"
#include "rtos_status.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                    __FILE__, __LINE__, #condition);                         \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static uint32_t g_event_send_count;
static rtos_ipc_event_t g_last_event;
static bool g_fail_event_send;
static uint32_t g_can_rx_send_count;

static void fill_message(rtos_can_message_t *message, uint32_t can_id)
{
    static const uint8_t payload[RTOS_CAN_CLASSIC_DATA_MAX_LEN] = {
        0x10u, 0x21u, 0x32u, 0x43u, 0x54u, 0x65u, 0x76u, 0x87u,
    };

    memset(message, 0, sizeof(*message));
    message->can_id = can_id;
    message->can_dlc = (uint8_t)sizeof(payload);
    message->can_flags = (uint8_t)RTOS_CAN_FLAG_NONE;
    memcpy(message->can_data, payload, sizeof(payload));
}

static unified_error_t test_event_sender(const rtos_ipc_event_t *event)
{
    if (event == 0) {
        return UNIFIED_ERR_NULL;
    }

    ++g_event_send_count;
    g_last_event = *event;
    return g_fail_event_send ? UNIFIED_ERR_INVALID_ARG : UNIFIED_OK;
}

static unified_error_t test_can_rx_sender(const rtos_can_message_t *message)
{
    if (message == 0) {
        return UNIFIED_ERR_NULL;
    }

    ++g_can_rx_send_count;
    return UNIFIED_OK;
}

static void reset_hooks(void)
{
    g_event_send_count = 0u;
    memset(&g_last_event, 0, sizeof(g_last_event));
    g_fail_event_send = false;
    g_can_rx_send_count = 0u;
}

static int test_heartbeat_timeout_enters_offline(void)
{
    rtos_can_message_t message;
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    reset_hooks();
    CHECK(gateway_forward_init() == UNIFIED_OK);
    rtos_ipc_set_event_sender(test_event_sender);
    rtos_recovery_note_linux_heartbeat(100u);

    CHECK(rtos_recovery_watchdog_check_once(100u + RTOS_LINUX_HEARTBEAT_TIMEOUT_MS + 1u) ==
          UNIFIED_OK);

    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(!status.linux_online);
    CHECK(status.linux_heartbeat_timeout == 1u);
    CHECK(status.linux_offline_enter == 1u);
    CHECK(status.xl2515_tx_aborted == 1u);
    CHECK(status.listen_only_enter == 1u);
    CHECK(driver.abort_tx_count == 1u);
    CHECK(driver.clear_tx_count == 1u);
    CHECK(driver.listen_only);
    CHECK(g_event_send_count == 1u);
    CHECK(g_last_event.event_type == RTOS_IPC_EVENT_LINUX_HEARTBEAT_TIMEOUT);

    fill_message(&message, 0x501u);
    CHECK(rtos_can_forward_submit_message(&message) == UNIFIED_ERR_INVALID_ARG);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(driver.send_count == 0u);
    return 0;
}

static int test_fail_safe_purges_queued_tx(void)
{
    rtos_can_message_t first;
    rtos_can_message_t second;
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_message(&first, 0x511u);
    fill_message(&second, 0x512u);
    CHECK(rtos_can_forward_enqueue_message(&first) == UNIFIED_OK);
    CHECK(rtos_can_forward_enqueue_message(&second) == UNIFIED_OK);
    CHECK(rtos_can_forward_get_tx_queue_depth() == 2u);

    CHECK(rtos_recovery_watchdog_check_once(RTOS_LINUX_HEARTBEAT_TIMEOUT_MS + 1u) ==
          UNIFIED_OK);

    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(rtos_can_forward_get_tx_queue_depth() == 0u);
    CHECK(status.tx_queue_purged == 2u);
    CHECK(driver.send_count == 0u);
    return 0;
}

static int test_rehandshake_restores_only_new_tx(void)
{
    rtos_can_message_t message;
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    CHECK(gateway_forward_init() == UNIFIED_OK);
    CHECK(rtos_recovery_watchdog_check_once(RTOS_LINUX_HEARTBEAT_TIMEOUT_MS + 1u) ==
          UNIFIED_OK);
    rtos_recovery_note_linux_heartbeat(RTOS_LINUX_HEARTBEAT_TIMEOUT_MS + 10u);

    fill_message(&message, 0x521u);
    CHECK(rtos_can_forward_submit_message(&message) == UNIFIED_ERR_INVALID_ARG);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(driver.send_count == 0u);
    CHECK(driver.listen_only);

    CHECK(rtos_recovery_complete_linux_rehandshake(RTOS_LINUX_HEARTBEAT_TIMEOUT_MS + 20u) ==
          UNIFIED_OK);
    CHECK(rtos_can_forward_submit_message(&message) == UNIFIED_OK);

    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.linux_online);
    CHECK(status.linux_rehandshake_ok == 1u);
    CHECK(driver.send_count == 1u);
    CHECK(!driver.listen_only);
    return 0;
}

static int test_spi_timeout_retries_and_resets(void)
{
    rtos_can_message_t message;
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    reset_hooks();
    CHECK(gateway_forward_init() == UNIFIED_OK);
    rtos_ipc_set_event_sender(test_event_sender);
    fill_message(&message, 0x531u);
    rtos_can_driver_mock_set_send_error(RTOS_CAN_DRIVER_ERROR_TIMEOUT,
                                        RTOS_CAN_TX_RETRY_MAX + 1u);

    CHECK(rtos_can_forward_submit_message(&message) == UNIFIED_OK);

    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.tx_to_can_ok == 0u);
    CHECK(status.tx_to_can_fail == RTOS_CAN_TX_RETRY_MAX + 1u);
    CHECK(status.spi_error == 1u);
    CHECK(driver.send_count == RTOS_CAN_TX_RETRY_MAX + 1u);
    CHECK(driver.reset_count == 1u);
    CHECK(g_event_send_count == 1u);
    CHECK(g_last_event.event_type == RTOS_IPC_EVENT_SPI_ERROR);
    return 0;
}

static int test_bus_off_resets_and_reports_event(void)
{
    rtos_can_message_t message;
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    reset_hooks();
    CHECK(gateway_forward_init() == UNIFIED_OK);
    rtos_ipc_set_event_sender(test_event_sender);
    fill_message(&message, 0x541u);
    rtos_can_driver_mock_set_send_error(RTOS_CAN_DRIVER_ERROR_BUS_OFF, 1u);

    CHECK(rtos_can_forward_submit_message(&message) == UNIFIED_OK);

    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.tx_to_can_fail == 1u);
    CHECK(status.can_bus_off == 1u);
    CHECK(driver.send_count == 1u);
    CHECK(driver.reset_count == 1u);
    CHECK(!driver.listen_only);
    CHECK(g_event_send_count == 1u);
    CHECK(g_last_event.event_type == RTOS_IPC_EVENT_CAN_BUS_OFF);
    return 0;
}

static int test_can_rx_still_runs_offline(void)
{
    rtos_can_message_t message;
    rtos_status_snapshot_t status;

    reset_hooks();
    CHECK(gateway_forward_init() == UNIFIED_OK);
    rtos_ipc_set_can_rx_sender(test_can_rx_sender);
    CHECK(rtos_recovery_watchdog_check_once(RTOS_LINUX_HEARTBEAT_TIMEOUT_MS + 1u) ==
          UNIFIED_OK);
    fill_message(&message, 0x551u);
    CHECK(rtos_can_driver_mock_inject_rx(&message) == UNIFIED_OK);

    rtos_can_task_gpio14_irq_notify();
    CAN_RX_Task(0);

    rtos_status_get_snapshot(&status);
    CHECK(!status.linux_online);
    CHECK(status.rx_from_can == 1u);
    CHECK(g_can_rx_send_count == 1u);
    return 0;
}

static int test_rx_overflow_event_failure_counts_ring_drop(void)
{
    rtos_status_snapshot_t status;

    reset_hooks();
    CHECK(gateway_forward_init() == UNIFIED_OK);
    rtos_ipc_set_event_sender(test_event_sender);
    g_fail_event_send = true;
    rtos_can_driver_mock_set_health(false, false, true);

    rtos_can_task_gpio14_irq_notify();
    CAN_RX_Task(0);

    rtos_status_get_snapshot(&status);
    CHECK(status.rx_overrun == 1u);
    CHECK(status.xl2515_rx_overflow == 1u);
    CHECK(status.drop_ring_full == 1u);
    CHECK(g_event_send_count == 1u);
    CHECK(g_last_event.event_type == RTOS_IPC_EVENT_RX_OVERFLOW);
    return 0;
}

int main(void)
{
    CHECK(test_heartbeat_timeout_enters_offline() == 0);
    CHECK(test_fail_safe_purges_queued_tx() == 0);
    CHECK(test_rehandshake_restores_only_new_tx() == 0);
    CHECK(test_spi_timeout_retries_and_resets() == 0);
    CHECK(test_bus_off_resets_and_reports_event() == 0);
    CHECK(test_can_rx_still_runs_offline() == 0);
    CHECK(test_rx_overflow_event_failure_counts_ring_drop() == 0);
    return 0;
}
