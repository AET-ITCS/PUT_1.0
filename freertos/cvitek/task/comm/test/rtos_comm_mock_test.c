#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtos_config.h"
#include "rtos_can_driver.h"
#include "rtos_can_forward.h"
#include "rtos_ipc.h"
#include "rtos_status.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                    __FILE__, __LINE__, #condition);                         \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static void fill_valid_message(rtos_can_message_t *message)
{
    static const uint8_t payload[RTOS_CAN_CLASSIC_DATA_MAX_LEN] = {
        0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u, 0x70u, 0x80u,
    };

    memset(message, 0, sizeof(*message));
    message->can_id = 0x123u;
    message->can_dlc = (uint8_t)sizeof(payload);
    message->can_flags = (uint8_t)RTOS_CAN_FLAG_NONE;
    memcpy(message->can_data, payload, sizeof(payload));
}

extern int comm_main(void);

int main(void)
{
    rtos_can_message_t message;
    rtos_status_snapshot_t status;
    rtos_can_driver_mock_snapshot_t driver;

    CHECK(comm_main() == 0);
    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.can_ready);
    CHECK(status.linux_online);
    CHECK(driver.initialized);
    CHECK(driver.init_count == 1u);
    CHECK(driver.bitrate == RTOS_CAN_BITRATE);
    CHECK(!driver.listen_only);
    CHECK(driver.last_error == RTOS_CAN_DRIVER_ERROR_NONE);

    fill_valid_message(&message);
    CHECK(gateway_forward_init() == UNIFIED_OK);
    CHECK(rtos_can_forward_submit_message(&message) == UNIFIED_OK);
    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 1u);
    CHECK(status.tx_to_can_ok == 1u);
    CHECK(status.tx_to_can_fail == 0u);
    CHECK(driver.send_count == 1u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_message(&message);
    CHECK(rtos_ipc_mock_receive_can_message(&message) == UNIFIED_OK);
    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 1u);
    CHECK(status.tx_to_can_ok == 1u);
    CHECK(status.tx_to_can_fail == 0u);
    CHECK(driver.send_count == 1u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    CHECK(rtos_can_forward_submit_message(NULL) == UNIFIED_ERR_INVALID_ARG);
    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 0u);
    CHECK(status.drop_null == 1u);
    CHECK(driver.send_count == 0u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_message(&message);
    message.can_flags = (uint8_t)(RTOS_CAN_FLAG_EXTENDED_ID << 1);
    CHECK(rtos_can_forward_submit_message(&message) == UNIFIED_ERR_INVALID_ARG);
    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 0u);
    CHECK(status.drop_flag == 1u);
    CHECK(driver.send_count == 0u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_message(&message);
    message.can_id = 0x800u;
    CHECK(rtos_can_forward_submit_message(&message) == UNIFIED_ERR_INVALID_ARG);
    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 0u);
    CHECK(status.drop_can_id == 1u);
    CHECK(driver.send_count == 0u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_message(&message);
    message.can_flags = (uint8_t)RTOS_CAN_FLAG_EXTENDED_ID;
    message.can_id = 0x1FFFFFFFu;
    CHECK(rtos_can_forward_submit_message(&message) == UNIFIED_OK);
    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 1u);
    CHECK(status.tx_to_can_ok == 1u);
    CHECK(driver.send_count == 1u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_message(&message);
    message.can_flags = (uint8_t)RTOS_CAN_FLAG_EXTENDED_ID;
    message.can_id = 0x20000000u;
    CHECK(rtos_can_forward_submit_message(&message) == UNIFIED_ERR_INVALID_ARG);
    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 0u);
    CHECK(status.drop_can_id == 1u);
    CHECK(driver.send_count == 0u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_message(&message);
    message.can_dlc = (uint8_t)(RTOS_CAN_CLASSIC_DATA_MAX_LEN + 1u);
    CHECK(rtos_can_forward_submit_message(&message) == UNIFIED_ERR_INVALID_ARG);
    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 0u);
    CHECK(status.drop_dlc == 1u);
    CHECK(driver.send_count == 0u);

    CHECK(gateway_forward_init() == UNIFIED_OK);
    fill_valid_message(&message);
    CHECK(rtos_can_driver_set_listen_only() == UNIFIED_OK);
    CHECK(rtos_can_forward_submit_message(&message) == UNIFIED_OK);
    rtos_status_get_snapshot(&status);
    rtos_can_driver_get_mock_snapshot(&driver);
    CHECK(status.rx_from_linux == 1u);
    CHECK(status.tx_to_can_ok == 0u);
    CHECK(status.tx_to_can_fail == 1u);
    CHECK(driver.send_count == 0u);
    CHECK(driver.listen_only);
    CHECK(driver.last_error == RTOS_CAN_DRIVER_ERROR_LISTEN_ONLY);

    return 0;
}
