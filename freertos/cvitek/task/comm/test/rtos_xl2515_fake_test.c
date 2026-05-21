/**
 * @file rtos_xl2515_fake_test.c
 * @brief Host tests for XL2515 driver register, loopback, and error paths.
 */
#include <stdio.h>
#include <string.h>

#include "rtos_can_driver.h"
#include "rtos_config.h"
#include "rtos_xl2515_fake_platform.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                    __FILE__, __LINE__, #condition);                         \
            return 1;                                                        \
        }                                                                    \
    } while (0)

#define XL2515_REG_CANSTAT 0x0Eu
#define XL2515_REG_CNF3 0x28u
#define XL2515_REG_CNF2 0x29u
#define XL2515_REG_CNF1 0x2Au
#define XL2515_REG_TXB0CTRL 0x30u
#define XL2515_REG_TXB0SIDH 0x31u
#define XL2515_REG_TXB0SIDL 0x32u
#define XL2515_REG_TXB0EID8 0x33u
#define XL2515_REG_TXB0EID0 0x34u
#define XL2515_REG_TXB0DLC 0x35u
#define XL2515_REG_TXB0D0 0x36u
#define XL2515_MODE_LOOPBACK 0x40u
#define XL2515_TXBCTRL_TXREQ 0x08u

static void fill_standard_message(rtos_can_message_t *message)
{
    static const uint8_t payload[RTOS_CAN_CLASSIC_DATA_MAX_LEN] = {
        0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u,
    };

    memset(message, 0, sizeof(*message));
    message->can_id = 0x123u;
    message->can_dlc = 8u;
    message->can_flags = (uint8_t)RTOS_CAN_FLAG_NONE;
    memcpy(message->can_data, payload, sizeof(payload));
}

static void fill_extended_message(rtos_can_message_t *message)
{
    static const uint8_t payload[3] = { 0xA1u, 0xB2u, 0xC3u };

    memset(message, 0, sizeof(*message));
    message->can_id = 0x18DAF110u;
    message->can_dlc = (uint8_t)sizeof(payload);
    message->can_flags = (uint8_t)RTOS_CAN_FLAG_EXTENDED_ID;
    memcpy(message->can_data, payload, sizeof(payload));
}

int main(void)
{
    rtos_can_message_t tx;
    rtos_can_message_t rx;

    rtos_xl2515_fake_reset();
    CHECK(rtos_can_driver_init() == UNIFIED_OK);
    CHECK(rtos_xl2515_fake_get_pinmux_count() == 1u);
    CHECK(rtos_xl2515_fake_get_spi_init_count() == 2u);
    CHECK(rtos_xl2515_fake_get_last_spi_hz() == RTOS_SPI_RUN_HZ);
    CHECK(rtos_xl2515_fake_get_reset_command_count() == 1u);
    CHECK(rtos_xl2515_fake_read_reg(XL2515_REG_CNF1) == 0x00u);
    CHECK(rtos_xl2515_fake_read_reg(XL2515_REG_CNF2) == 0xB1u);
    CHECK(rtos_xl2515_fake_read_reg(XL2515_REG_CNF3) == 0x05u);
    CHECK((rtos_xl2515_fake_read_reg(XL2515_REG_CANSTAT) & 0xE0u) ==
          XL2515_MODE_LOOPBACK);

    CHECK(rtos_can_driver_set_bitrate(250000u) == UNIFIED_ERR_INVALID_ARG);
    CHECK(rtos_can_driver_set_bitrate(RTOS_CAN_BITRATE) == UNIFIED_OK);

    fill_standard_message(&tx);
    CHECK(rtos_can_driver_send(&tx) == UNIFIED_OK);
    CHECK(rtos_xl2515_fake_read_reg(XL2515_REG_TXB0SIDH) == 0x24u);
    CHECK(rtos_xl2515_fake_read_reg(XL2515_REG_TXB0SIDL) == 0x60u);
    CHECK(rtos_xl2515_fake_read_reg(XL2515_REG_TXB0DLC) == tx.can_dlc);
    CHECK(rtos_xl2515_fake_read_reg(XL2515_REG_TXB0D0) == tx.can_data[0]);
    CHECK(rtos_can_driver_read(&rx) == UNIFIED_OK);
    CHECK(rx.can_id == tx.can_id);
    CHECK(rx.can_dlc == tx.can_dlc);
    CHECK(rx.can_flags == tx.can_flags);
    CHECK(memcmp(rx.can_data, tx.can_data, tx.can_dlc) == 0);
    CHECK(rtos_can_driver_read(&rx) == UNIFIED_ERR_INVALID_ARG);
    CHECK(rtos_can_driver_get_error() == RTOS_CAN_DRIVER_ERROR_NO_RX);

    fill_extended_message(&tx);
    CHECK(rtos_can_driver_send(&tx) == UNIFIED_OK);
    CHECK(rtos_xl2515_fake_read_reg(XL2515_REG_TXB0SIDH) == 0xC6u);
    CHECK(rtos_xl2515_fake_read_reg(XL2515_REG_TXB0SIDL) == 0xCAu);
    CHECK(rtos_xl2515_fake_read_reg(XL2515_REG_TXB0EID8) == 0xF1u);
    CHECK(rtos_xl2515_fake_read_reg(XL2515_REG_TXB0EID0) == 0x10u);
    CHECK(rtos_can_driver_read(&rx) == UNIFIED_OK);
    CHECK(rx.can_id == tx.can_id);
    CHECK(rx.can_flags == tx.can_flags);
    CHECK(rx.can_dlc == tx.can_dlc);
    CHECK(memcmp(rx.can_data, tx.can_data, tx.can_dlc) == 0);

    CHECK(rtos_can_driver_set_listen_only() == UNIFIED_OK);
    CHECK(rtos_can_driver_send(&tx) == UNIFIED_ERR_INVALID_ARG);
    CHECK(rtos_can_driver_get_error() == RTOS_CAN_DRIVER_ERROR_LISTEN_ONLY);
    CHECK(rtos_can_driver_set_normal() == UNIFIED_OK);

    rtos_xl2515_fake_set_bus_off(true);
    CHECK(rtos_can_driver_send(&tx) == UNIFIED_ERR_INVALID_ARG);
    CHECK(rtos_can_driver_get_error() == RTOS_CAN_DRIVER_ERROR_BUS_OFF);
    rtos_xl2515_fake_set_bus_off(false);

    rtos_xl2515_fake_set_tx_auto_complete(false);
    CHECK(rtos_can_driver_send(&tx) == UNIFIED_ERR_INVALID_ARG);
    CHECK(rtos_can_driver_get_error() == RTOS_CAN_DRIVER_ERROR_TIMEOUT);
    CHECK((rtos_xl2515_fake_read_reg(XL2515_REG_TXB0CTRL) & XL2515_TXBCTRL_TXREQ) != 0u);
    CHECK(rtos_can_driver_abort_tx() == UNIFIED_OK);
    CHECK((rtos_xl2515_fake_read_reg(XL2515_REG_TXB0CTRL) & XL2515_TXBCTRL_TXREQ) == 0u);

    return 0;
}
