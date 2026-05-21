#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtos_can_driver.h"
#include "rtos_can_forward.h"
#include "rtos_can_task.h"
#include "rtos_config.h"
#include "rtos_ipc.h"
#include "rtos_protocol_adapter.h"
#include "rtos_status.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                    __FILE__, __LINE__, #condition);                         \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static uint32_t g_can_rx_send_count;
static rtos_can_message_t g_last_can_rx;
static bool g_fail_can_rx_send;
static uint32_t g_status_send_count;
static rtos_status_snapshot_t g_last_status;

static void fill_message(rtos_can_message_t *message, uint32_t can_id)
{
    static const uint8_t payload[RTOS_CAN_CLASSIC_DATA_MAX_LEN] = {
        0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, 0xCDu, 0xEFu,
    };

    memset(message, 0, sizeof(*message));
    message->can_id = can_id;
    message->can_dlc = (uint8_t)sizeof(payload);
    message->can_flags = (uint8_t)RTOS_CAN_FLAG_NONE;
    memcpy(message->can_data, payload, sizeof(payload));
}

static unified_error_t test_payload_to_can(const rtos_ipc_payload_view_t *payload,
                                           rtos_can_message_t *out_message)
{
    if ((payload == 0) || (out_message == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (payload->length != sizeof(*out_message)) {
        return UNIFIED_ERR_LENGTH;
    }

    memcpy(out_message, payload->bytes, sizeof(*out_message));
    return UNIFIED_OK;
}

static unified_error_t test_can_rx_sender(const rtos_can_message_t *message)
{
    if (message == 0) {
        return UNIFIED_ERR_NULL;
    }

    ++g_can_rx_send_count;
    g_last_can_rx = *message;
    return g_fail_can_rx_send ? UNIFIED_ERR_INVALID_ARG : UNIFIED_OK;
}

static unified_error_t test_status_sender(const rtos_status_snapshot_t *status)
{
    if (status == 0) {
        return UNIFIED_ERR_NULL;
    }

    ++g_status_send_count;
    g_last_status = *status;
    return UNIFIED_OK;
}

static void reset_test_hooks(void)
{
    g_can_rx_send_count = 0u;
    memset(&g_last_can_rx, 0, sizeof(g_last_can_rx));
    g_fail_can_rx_send = false;
    g_status_send_count = 0u;
    memset(&g_last_status, 0, sizeof(g_last_status));
}

static int test_gateway_ipc_drains_payloads(void)
{
    rtos_can_message_t first;
    rtos_can_message_t second;
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    CHECK(gateway_forward_init() == UNIFIED_OK);
    rtos_protocol_adapter_set_linux_payload_to_can(test_payload_to_can);
    fill_message(&first, 0x101u);
    fill_message(&second, 0x102u);

    CHECK(rtos_ipc_mock_receive_payload((const uint8_t *)&first,
                                        (uint16_t)sizeof(first)) == UNIFIED_OK);
    CHECK(rtos_ipc_mock_receive_payload((const uint8_t *)&second,
                                        (uint16_t)sizeof(second)) == UNIFIED_OK);
    Gateway_IPC_Task(0);

    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 2u);
    CHECK(status.tx_to_can_ok == 2u);
    CHECK(status.ipc_payload_drop == 0u);
    CHECK(driver.send_count == 2u);
    return 0;
}

static int test_gateway_ipc_queue_full_drops_latest(void)
{
    uint32_t i;
    rtos_can_message_t message;
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    CHECK(gateway_forward_init() == UNIFIED_OK);
    rtos_protocol_adapter_set_linux_payload_to_can(test_payload_to_can);

    for (i = 0u; i < RTOS_IPC_MOCK_RX_QUEUE_LEN; ++i) {
        fill_message(&message, 0x200u + i);
        CHECK(rtos_ipc_mock_receive_payload((const uint8_t *)&message,
                                            (uint16_t)sizeof(message)) == UNIFIED_OK);
    }

    fill_message(&message, 0x777u);
    CHECK(rtos_ipc_mock_receive_payload((const uint8_t *)&message,
                                        (uint16_t)sizeof(message)) ==
          UNIFIED_ERR_INVALID_ARG);
    Gateway_IPC_Task(0);

    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == RTOS_IPC_MOCK_RX_QUEUE_LEN);
    CHECK(status.tx_to_can_ok == RTOS_IPC_MOCK_RX_QUEUE_LEN);
    CHECK(status.ipc_payload_drop == 1u);
    CHECK(driver.send_count == RTOS_IPC_MOCK_RX_QUEUE_LEN);
    return 0;
}

static int test_can_rx_task_drains_after_gpio14_notify(void)
{
    rtos_can_message_t first;
    rtos_can_message_t second;
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    reset_test_hooks();
    CHECK(gateway_forward_init() == UNIFIED_OK);
    rtos_ipc_set_can_rx_sender(test_can_rx_sender);
    fill_message(&first, 0x301u);
    fill_message(&second, 0x302u);
    CHECK(rtos_can_driver_mock_inject_rx(&first) == UNIFIED_OK);
    CHECK(rtos_can_driver_mock_inject_rx(&second) == UNIFIED_OK);

    rtos_can_task_gpio14_irq_notify();
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(driver.read_count == 0u);
    CHECK(rtos_can_task_gpio14_irq_is_pending());

    CAN_RX_Task(0);
    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(!rtos_can_task_gpio14_irq_is_pending());
    CHECK(status.rx_from_can == 2u);
    CHECK(status.tx_to_linux == 2u);
    CHECK(status.drop_ring_full == 0u);
    CHECK(g_can_rx_send_count == 2u);
    CHECK(g_last_can_rx.can_id == second.can_id);
    CHECK(driver.read_count == 2u);
    return 0;
}

static int test_can_rx_task_no_rx_is_quiet(void)
{
    rtos_status_snapshot_t status;

    reset_test_hooks();
    CHECK(gateway_forward_init() == UNIFIED_OK);
    rtos_ipc_set_can_rx_sender(test_can_rx_sender);
    rtos_can_task_gpio14_irq_notify();
    CAN_RX_Task(0);

    rtos_status_get_snapshot(&status);
    CHECK(status.rx_from_can == 0u);
    CHECK(status.tx_to_linux == 0u);
    CHECK(status.drop_ring_full == 0u);
    CHECK(g_can_rx_send_count == 0u);
    return 0;
}

static int test_can_rx_send_failure_counts_ring_drop(void)
{
    rtos_can_message_t message;
    rtos_status_snapshot_t status;

    reset_test_hooks();
    CHECK(gateway_forward_init() == UNIFIED_OK);
    rtos_ipc_set_can_rx_sender(test_can_rx_sender);
    g_fail_can_rx_send = true;
    fill_message(&message, 0x401u);
    CHECK(rtos_can_driver_mock_inject_rx(&message) == UNIFIED_OK);

    rtos_can_task_gpio14_irq_notify();
    CAN_RX_Task(0);

    rtos_status_get_snapshot(&status);
    CHECK(status.rx_from_can == 1u);
    CHECK(status.tx_to_linux == 0u);
    CHECK(status.drop_ring_full == 1u);
    CHECK(g_can_rx_send_count == 1u);
    return 0;
}

static int test_status_task_reports_snapshot(void)
{
    rtos_status_snapshot_t status;

    reset_test_hooks();
    CHECK(gateway_forward_init() == UNIFIED_OK);
    rtos_ipc_set_status_sender(test_status_sender);
    Status_Task(0);

    rtos_status_get_snapshot(&status);
    CHECK(g_status_send_count == 1u);
    CHECK(g_last_status.can_ready);
    CHECK(g_last_status.linux_online);
    CHECK(status.tx_to_linux == 1u);
    return 0;
}

int main(void)
{
    CHECK(test_gateway_ipc_drains_payloads() == 0);
    CHECK(test_gateway_ipc_queue_full_drops_latest() == 0);
    CHECK(test_can_rx_task_drains_after_gpio14_notify() == 0);
    CHECK(test_can_rx_task_no_rx_is_quiet() == 0);
    CHECK(test_can_rx_send_failure_counts_ring_drop() == 0);
    CHECK(test_status_task_reports_snapshot() == 0);
    return 0;
}
