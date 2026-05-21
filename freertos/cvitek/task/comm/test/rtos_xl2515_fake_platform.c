/**
 * @file rtos_xl2515_fake_platform.c
 * @brief Fake XL2515 SPI platform used by host tests.
 */
#include "rtos_xl2515_fake_platform.h"

#include <string.h>

#define XL2515_CMD_RESET 0xC0u
#define XL2515_CMD_READ 0x03u
#define XL2515_CMD_WRITE 0x02u
#define XL2515_CMD_BIT_MODIFY 0x05u
#define XL2515_CMD_READ_STATUS 0xA0u
#define XL2515_CMD_RTS_TXB0 0x81u

#define XL2515_REG_CANSTAT 0x0Eu
#define XL2515_REG_CANCTRL 0x0Fu
#define XL2515_REG_CANINTF 0x2Cu
#define XL2515_REG_EFLG 0x2Du
#define XL2515_REG_TXB0CTRL 0x30u
#define XL2515_REG_TXB0SIDH 0x31u
#define XL2515_REG_RXB0SIDH 0x61u

#define XL2515_MODE_CONFIG 0x80u
#define XL2515_MODE_LOOPBACK 0x40u
#define XL2515_CANCTRL_REQOP_MASK 0xE0u
#define XL2515_CANINTF_RX0IF 0x01u
#define XL2515_CANINTF_TX0IF 0x04u
#define XL2515_EFLG_TXBO 0x20u
#define XL2515_TXBCTRL_TXREQ 0x08u
#define XL2515_SIDL_EXIDE 0x08u

typedef struct {
    uint8_t regs[256];
    uint32_t spi_init_count;
    uint32_t last_spi_hz;
    uint32_t pinmux_count;
    uint32_t reset_command_count;
    uint32_t delay_us;
    bool tx_auto_complete;
} xl2515_fake_t;

static xl2515_fake_t g_fake;

static void fake_set_mode(uint8_t mode)
{
    g_fake.regs[XL2515_REG_CANCTRL] =
        (uint8_t)((g_fake.regs[XL2515_REG_CANCTRL] & (uint8_t)~XL2515_CANCTRL_REQOP_MASK) |
                  (mode & XL2515_CANCTRL_REQOP_MASK));
    g_fake.regs[XL2515_REG_CANSTAT] =
        (uint8_t)((g_fake.regs[XL2515_REG_CANSTAT] & (uint8_t)~XL2515_CANCTRL_REQOP_MASK) |
                  (mode & XL2515_CANCTRL_REQOP_MASK));
}

static void fake_encode_id(uint32_t can_id, uint8_t can_flags, uint8_t *out_regs)
{
    if ((can_flags & (uint8_t)RTOS_CAN_FLAG_EXTENDED_ID) != 0u) {
        out_regs[0] = (uint8_t)(can_id >> 21);
        out_regs[1] = (uint8_t)(((can_id >> 13) & 0xE0u) |
                                XL2515_SIDL_EXIDE |
                                ((can_id >> 16) & 0x03u));
        out_regs[2] = (uint8_t)(can_id >> 8);
        out_regs[3] = (uint8_t)can_id;
    } else {
        out_regs[0] = (uint8_t)(can_id >> 3);
        out_regs[1] = (uint8_t)((can_id & 0x07u) << 5);
        out_regs[2] = 0u;
        out_regs[3] = 0u;
    }
}

static uint8_t fake_read_status(void)
{
    uint8_t status = 0u;

    if ((g_fake.regs[XL2515_REG_CANINTF] & XL2515_CANINTF_RX0IF) != 0u) {
        status |= 0x01u;
    }

    if ((g_fake.regs[XL2515_REG_TXB0CTRL] & XL2515_TXBCTRL_TXREQ) != 0u) {
        status |= 0x04u;
    }

    return status;
}

static void fake_copy_txb0_to_rxb0(void)
{
    memcpy(&g_fake.regs[XL2515_REG_RXB0SIDH],
           &g_fake.regs[XL2515_REG_TXB0SIDH],
           13u);
    g_fake.regs[XL2515_REG_CANINTF] |= XL2515_CANINTF_RX0IF;
}

void rtos_xl2515_fake_reset(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.tx_auto_complete = true;
    fake_set_mode(XL2515_MODE_CONFIG);
}

void rtos_xl2515_fake_set_tx_auto_complete(bool enabled)
{
    g_fake.tx_auto_complete = enabled;
}

void rtos_xl2515_fake_set_bus_off(bool enabled)
{
    if (enabled) {
        g_fake.regs[XL2515_REG_EFLG] |= XL2515_EFLG_TXBO;
    } else {
        g_fake.regs[XL2515_REG_EFLG] &= (uint8_t)~XL2515_EFLG_TXBO;
    }
}

void rtos_xl2515_fake_load_rx0(const rtos_can_message_t *message)
{
    uint8_t regs[13];

    if (message == NULL) {
        return;
    }

    memset(regs, 0, sizeof(regs));
    fake_encode_id(message->can_id, message->can_flags, regs);
    regs[4] = message->can_dlc;
    memcpy(&regs[5], message->can_data, message->can_dlc);
    memcpy(&g_fake.regs[XL2515_REG_RXB0SIDH], regs, sizeof(regs));
    g_fake.regs[XL2515_REG_CANINTF] |= XL2515_CANINTF_RX0IF;
}

uint8_t rtos_xl2515_fake_read_reg(uint8_t reg)
{
    return g_fake.regs[reg];
}

uint32_t rtos_xl2515_fake_get_spi_init_count(void)
{
    return g_fake.spi_init_count;
}

uint32_t rtos_xl2515_fake_get_last_spi_hz(void)
{
    return g_fake.last_spi_hz;
}

uint32_t rtos_xl2515_fake_get_pinmux_count(void)
{
    return g_fake.pinmux_count;
}

uint32_t rtos_xl2515_fake_get_reset_command_count(void)
{
    return g_fake.reset_command_count;
}

unified_error_t rtos_xl2515_fake_spi_init(uint32_t hz)
{
    ++g_fake.spi_init_count;
    g_fake.last_spi_hz = hz;
    return UNIFIED_OK;
}

unified_error_t rtos_xl2515_fake_spi_xfer(const uint8_t *tx_buf,
                                          uint8_t *rx_buf,
                                          uint16_t length)
{
    uint16_t i;

    if ((tx_buf == NULL) || (length == 0u)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (rx_buf != NULL) {
        memset(rx_buf, 0, length);
    }

    switch (tx_buf[0]) {
    case XL2515_CMD_RESET:
        ++g_fake.reset_command_count;
        fake_set_mode(XL2515_MODE_CONFIG);
        break;
    case XL2515_CMD_READ:
        if (length < 3u) {
            return UNIFIED_ERR_LENGTH;
        }
        if (rx_buf != NULL) {
            for (i = 2u; i < length; ++i) {
                rx_buf[i] = g_fake.regs[(uint8_t)(tx_buf[1] + (uint8_t)(i - 2u))];
            }
        }
        break;
    case XL2515_CMD_WRITE:
        if (length < 3u) {
            return UNIFIED_ERR_LENGTH;
        }
        for (i = 2u; i < length; ++i) {
            uint8_t reg = (uint8_t)(tx_buf[1] + (uint8_t)(i - 2u));

            g_fake.regs[reg] = tx_buf[i];
            if (reg == XL2515_REG_CANCTRL) {
                fake_set_mode((uint8_t)(tx_buf[i] & XL2515_CANCTRL_REQOP_MASK));
            }
        }
        break;
    case XL2515_CMD_BIT_MODIFY:
        if (length != 4u) {
            return UNIFIED_ERR_LENGTH;
        }
        g_fake.regs[tx_buf[1]] = (uint8_t)((g_fake.regs[tx_buf[1]] & (uint8_t)~tx_buf[2]) |
                                           (tx_buf[3] & tx_buf[2]));
        if (tx_buf[1] == XL2515_REG_CANCTRL) {
            fake_set_mode((uint8_t)(g_fake.regs[XL2515_REG_CANCTRL] &
                                    XL2515_CANCTRL_REQOP_MASK));
        }
        break;
    case XL2515_CMD_READ_STATUS:
        if ((rx_buf != NULL) && (length >= 2u)) {
            rx_buf[1] = fake_read_status();
        }
        break;
    case XL2515_CMD_RTS_TXB0:
        g_fake.regs[XL2515_REG_TXB0CTRL] |= XL2515_TXBCTRL_TXREQ;
        if (g_fake.tx_auto_complete) {
            if ((g_fake.regs[XL2515_REG_CANSTAT] & XL2515_CANCTRL_REQOP_MASK) ==
                XL2515_MODE_LOOPBACK) {
                fake_copy_txb0_to_rxb0();
            }
            g_fake.regs[XL2515_REG_TXB0CTRL] &= (uint8_t)~XL2515_TXBCTRL_TXREQ;
            g_fake.regs[XL2515_REG_CANINTF] |= XL2515_CANINTF_TX0IF;
        }
        break;
    default:
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

void rtos_xl2515_fake_delay_us(uint32_t usec)
{
    g_fake.delay_us += usec;
}

void rtos_xl2515_fake_config_spi2_pinmux(void)
{
    ++g_fake.pinmux_count;
}
