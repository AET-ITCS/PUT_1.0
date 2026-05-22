/**
 * @file rtos_can_driver.c
 * @brief FreeRTOS comm CAN driver 实现。
 *
 * 默认构建使用 host mock driver。定义 RTOS_CAN_DRIVER_XL2515_ENABLE 后启用
 * XL2515/MCP2515-like SPI CAN 控制器路径；测试可通过
 * RTOS_CAN_DRIVER_XL2515_FAKE_PLATFORM 使用 fake SPI 平台。
 */
#include "rtos_can_driver.h"

#include <string.h>

#include "rtos_config.h"

#ifndef RTOS_CAN_DRIVER_XL2515_ENABLE
#define RTOS_CAN_DRIVER_XL2515_ENABLE 0
#endif

#ifndef RTOS_CAN_DRIVER_XL2515_FAKE_PLATFORM
#define RTOS_CAN_DRIVER_XL2515_FAKE_PLATFORM 0
#endif

static rtos_can_driver_mock_snapshot_t g_driver;
static uint32_t g_bitrate;

#if RTOS_CAN_DRIVER_XL2515_ENABLE

#if RTOS_CAN_DRIVER_XL2515_FAKE_PLATFORM
extern unified_error_t rtos_xl2515_fake_spi_init(uint32_t hz);
extern unified_error_t rtos_xl2515_fake_spi_xfer(const uint8_t *tx_buf,
                                                uint8_t *rx_buf,
                                                uint16_t length);
extern void rtos_xl2515_fake_delay_us(uint32_t usec);
extern void rtos_xl2515_fake_config_spi2_pinmux(void);
#else
#include "delay.h"
#include "drv_spi.h"
#include "hal_pinmux.h"
#endif

#define XL2515_CMD_RESET 0xC0u
#define XL2515_CMD_READ 0x03u
#define XL2515_CMD_WRITE 0x02u
#define XL2515_CMD_BIT_MODIFY 0x05u
#define XL2515_CMD_READ_STATUS 0xA0u
#define XL2515_CMD_RTS_TXB0 0x81u

#define XL2515_REG_CANSTAT 0x0Eu
#define XL2515_REG_CANCTRL 0x0Fu
#define XL2515_REG_CNF3 0x28u
#define XL2515_REG_CNF2 0x29u
#define XL2515_REG_CNF1 0x2Au
#define XL2515_REG_CANINTE 0x2Bu
#define XL2515_REG_CANINTF 0x2Cu
#define XL2515_REG_EFLG 0x2Du
#define XL2515_REG_TXB0CTRL 0x30u
#define XL2515_REG_TXB0SIDH 0x31u
#define XL2515_REG_TXB1CTRL 0x40u
#define XL2515_REG_TXB2CTRL 0x50u
#define XL2515_REG_RXB0CTRL 0x60u
#define XL2515_REG_RXB0SIDH 0x61u
#define XL2515_REG_RXB1CTRL 0x70u
#define XL2515_REG_RXB1SIDH 0x71u

#define XL2515_CANCTRL_REQOP_MASK 0xE0u
#define XL2515_MODE_NORMAL 0x00u
#define XL2515_MODE_SLEEP 0x20u
#define XL2515_MODE_LOOPBACK 0x40u
#define XL2515_MODE_LISTEN_ONLY 0x60u
#define XL2515_MODE_CONFIG 0x80u

#define XL2515_CANINTF_RX0IF 0x01u
#define XL2515_CANINTF_RX1IF 0x02u
#define XL2515_CANINTF_TX0IF 0x04u
#define XL2515_CANINTF_ERRIF 0x20u
#define XL2515_CANINTF_WAKIF 0x40u
#define XL2515_CANINTF_MERRF 0x80u

#define XL2515_EFLG_RXEP 0x08u
#define XL2515_EFLG_TXEP 0x10u
#define XL2515_EFLG_TXBO 0x20u
#define XL2515_EFLG_RX0OVR 0x40u
#define XL2515_EFLG_RX1OVR 0x80u
#define XL2515_TXBCTRL_TXREQ 0x08u
#define XL2515_SIDL_EXIDE 0x08u
#define XL2515_RXBCTRL_ACCEPT_ALL 0x60u
#define XL2515_CLASSIC_DLC_MASK 0x0Fu

#define XL2515_CNF1_16MHZ_500K 0x00u
#define XL2515_CNF2_16MHZ_500K 0xB1u
#define XL2515_CNF3_16MHZ_500K 0x05u

static unified_error_t xl2515_spi_init(uint32_t hz)
{
#if RTOS_CAN_DRIVER_XL2515_FAKE_PLATFORM
    return rtos_xl2515_fake_spi_init(hz);
#else
    struct spi_cfg cfg;

    cfg.tmode = 0u;
    cfg.data_width = 8u;
    cfg.freq = hz;
    hal_spi_init(SPI2, &cfg);
    return UNIFIED_OK;
#endif
}

static unified_error_t xl2515_spi_xfer(const uint8_t *tx_buf,
                                       uint8_t *rx_buf,
                                       uint16_t length)
{
    if ((tx_buf == NULL) || (length == 0u)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

#if RTOS_CAN_DRIVER_XL2515_FAKE_PLATFORM
    return rtos_xl2515_fake_spi_xfer(tx_buf, rx_buf, length);
#else
    {
        struct spi_message message;

        message.send_buf = tx_buf;
        message.recv_buf = rx_buf;
        message.length = length;
        message.next = NULL;
        spixfer(SPI2, &message);
    }
    return UNIFIED_OK;
#endif
}

static void xl2515_delay_us(uint32_t usec)
{
#if RTOS_CAN_DRIVER_XL2515_FAKE_PLATFORM
    rtos_xl2515_fake_delay_us(usec);
#else
    udelay(usec);
#endif
}

static void xl2515_config_spi2_pinmux(void)
{
#if RTOS_CAN_DRIVER_XL2515_FAKE_PLATFORM
    rtos_xl2515_fake_config_spi2_pinmux();
#else
    hal_pinmux_config(PINMUX_SPI2);
#endif
}

static unified_error_t xl2515_reset_chip(void)
{
    uint8_t tx[1] = { XL2515_CMD_RESET };

    if (xl2515_spi_xfer(tx, NULL, (uint16_t)sizeof(tx)) != UNIFIED_OK) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_SPI;
        return UNIFIED_ERR_INVALID_ARG;
    }

    xl2515_delay_us(10000u);
    return UNIFIED_OK;
}

static unified_error_t xl2515_read_reg(uint8_t reg, uint8_t *out_value)
{
    uint8_t tx[3] = { XL2515_CMD_READ, reg, 0x00u };
    uint8_t rx[3] = { 0u, 0u, 0u };

    if (out_value == NULL) {
        return UNIFIED_ERR_NULL;
    }

    if (xl2515_spi_xfer(tx, rx, (uint16_t)sizeof(tx)) != UNIFIED_OK) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_SPI;
        return UNIFIED_ERR_INVALID_ARG;
    }

    *out_value = rx[2];
    return UNIFIED_OK;
}

static unified_error_t xl2515_read_regs(uint8_t reg, uint8_t *out_values, uint8_t length)
{
    uint8_t tx[16];
    uint8_t rx[16];
    uint8_t i;

    if ((out_values == NULL) || (length == 0u) ||
        ((uint32_t)length + 2u > sizeof(tx))) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(tx, 0, sizeof(tx));
    memset(rx, 0, sizeof(rx));
    tx[0] = XL2515_CMD_READ;
    tx[1] = reg;

    if (xl2515_spi_xfer(tx, rx, (uint16_t)((uint16_t)length + 2u)) != UNIFIED_OK) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_SPI;
        return UNIFIED_ERR_INVALID_ARG;
    }

    for (i = 0u; i < length; ++i) {
        out_values[i] = rx[(uint8_t)(i + 2u)];
    }

    return UNIFIED_OK;
}

static unified_error_t xl2515_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[3] = { XL2515_CMD_WRITE, reg, value };

    if (xl2515_spi_xfer(tx, NULL, (uint16_t)sizeof(tx)) != UNIFIED_OK) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_SPI;
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

static unified_error_t xl2515_write_regs(uint8_t reg, const uint8_t *values, uint8_t length)
{
    uint8_t tx[16];
    uint8_t i;

    if ((values == NULL) || (length == 0u) ||
        ((uint32_t)length + 2u > sizeof(tx))) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(tx, 0, sizeof(tx));
    tx[0] = XL2515_CMD_WRITE;
    tx[1] = reg;
    for (i = 0u; i < length; ++i) {
        tx[(uint8_t)(i + 2u)] = values[i];
    }

    if (xl2515_spi_xfer(tx, NULL, (uint16_t)((uint16_t)length + 2u)) != UNIFIED_OK) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_SPI;
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

static unified_error_t xl2515_bit_modify(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t tx[4] = { XL2515_CMD_BIT_MODIFY, reg, mask, value };

    if (xl2515_spi_xfer(tx, NULL, (uint16_t)sizeof(tx)) != UNIFIED_OK) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_SPI;
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

static unified_error_t xl2515_read_status(uint8_t *out_status)
{
    uint8_t tx[2] = { XL2515_CMD_READ_STATUS, 0x00u };
    uint8_t rx[2] = { 0u, 0u };

    if (out_status == NULL) {
        return UNIFIED_ERR_NULL;
    }

    if (xl2515_spi_xfer(tx, rx, (uint16_t)sizeof(tx)) != UNIFIED_OK) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_SPI;
        return UNIFIED_ERR_INVALID_ARG;
    }

    *out_status = rx[1];
    return UNIFIED_OK;
}

static unified_error_t xl2515_request_to_send_txb0(void)
{
    uint8_t tx[1] = { XL2515_CMD_RTS_TXB0 };

    if (xl2515_spi_xfer(tx, NULL, (uint16_t)sizeof(tx)) != UNIFIED_OK) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_SPI;
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

static unified_error_t xl2515_wait_mode(uint8_t mode)
{
    uint32_t i;

    for (i = 0u; i < RTOS_XL2515_MODE_TIMEOUT_POLLS; ++i) {
        uint8_t canstat;

        if (xl2515_read_reg(XL2515_REG_CANSTAT, &canstat) != UNIFIED_OK) {
            return UNIFIED_ERR_INVALID_ARG;
        }

        if ((canstat & XL2515_CANCTRL_REQOP_MASK) == mode) {
            return UNIFIED_OK;
        }

        xl2515_delay_us(10u);
    }

    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_TIMEOUT;
    return UNIFIED_ERR_INVALID_ARG;
}

static unified_error_t xl2515_set_mode(uint8_t mode)
{
    if (xl2515_bit_modify(XL2515_REG_CANCTRL, XL2515_CANCTRL_REQOP_MASK, mode) != UNIFIED_OK) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    return xl2515_wait_mode(mode);
}

static unified_error_t xl2515_apply_bitrate(uint32_t bitrate)
{
    if ((bitrate != RTOS_CAN_BITRATE) ||
        (RTOS_CAN_BITRATE != 500000u) ||
        (RTOS_XL2515_OSC_HZ != 16000000u)) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (xl2515_write_reg(XL2515_REG_CNF1, XL2515_CNF1_16MHZ_500K) != UNIFIED_OK ||
        xl2515_write_reg(XL2515_REG_CNF2, XL2515_CNF2_16MHZ_500K) != UNIFIED_OK ||
        xl2515_write_reg(XL2515_REG_CNF3, XL2515_CNF3_16MHZ_500K) != UNIFIED_OK) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    g_bitrate = bitrate;
    return UNIFIED_OK;
}

static unified_error_t xl2515_config_accept_all(void)
{
    if (xl2515_write_reg(XL2515_REG_RXB0CTRL, XL2515_RXBCTRL_ACCEPT_ALL) != UNIFIED_OK ||
        xl2515_write_reg(XL2515_REG_RXB1CTRL, XL2515_RXBCTRL_ACCEPT_ALL) != UNIFIED_OK) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

static unified_error_t xl2515_clear_interrupts(void)
{
    return xl2515_write_reg(XL2515_REG_CANINTF, 0x00u);
}

static unified_error_t xl2515_check_bus_off(void)
{
    uint8_t eflg;

    if (xl2515_read_reg(XL2515_REG_EFLG, &eflg) != UNIFIED_OK) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if ((eflg & XL2515_EFLG_TXBO) != 0u) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_BUS_OFF;
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

static void xl2515_encode_id(uint32_t can_id, uint8_t can_flags, uint8_t *out_regs)
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

static void xl2515_decode_id(const uint8_t *regs, rtos_can_message_t *out_message)
{
    if ((regs[1] & XL2515_SIDL_EXIDE) != 0u) {
        out_message->can_id = ((uint32_t)regs[0] << 21) |
                              ((uint32_t)(regs[1] & 0xE0u) << 13) |
                              ((uint32_t)(regs[1] & 0x03u) << 16) |
                              ((uint32_t)regs[2] << 8) |
                              (uint32_t)regs[3];
        out_message->can_flags = (uint8_t)RTOS_CAN_FLAG_EXTENDED_ID;
    } else {
        out_message->can_id = ((uint32_t)regs[0] << 3) |
                              (uint32_t)(regs[1] >> 5);
        out_message->can_flags = (uint8_t)RTOS_CAN_FLAG_NONE;
    }
}

static unified_error_t xl2515_load_txb0(const rtos_can_message_t *message)
{
    uint8_t regs[13];

    if ((message == NULL) || (message->can_dlc > RTOS_CAN_CLASSIC_DATA_MAX_LEN)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(regs, 0, sizeof(regs));
    xl2515_encode_id(message->can_id, message->can_flags, regs);
    regs[4] = (uint8_t)(message->can_dlc & XL2515_CLASSIC_DLC_MASK);
    memcpy(&regs[5], message->can_data, message->can_dlc);

    return xl2515_write_regs(XL2515_REG_TXB0SIDH, regs, (uint8_t)sizeof(regs));
}

static unified_error_t xl2515_read_rx_buffer(uint8_t sidh_reg, rtos_can_message_t *out_message)
{
    uint8_t regs[13];

    if (out_message == NULL) {
        return UNIFIED_ERR_NULL;
    }

    if (xl2515_read_regs(sidh_reg, regs, (uint8_t)sizeof(regs)) != UNIFIED_OK) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(out_message, 0, sizeof(*out_message));
    xl2515_decode_id(regs, out_message);
    out_message->can_dlc = (uint8_t)(regs[4] & XL2515_CLASSIC_DLC_MASK);
    if (out_message->can_dlc > RTOS_CAN_CLASSIC_DATA_MAX_LEN) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_SPI;
        return UNIFIED_ERR_INVALID_ARG;
    }

    memcpy(out_message->can_data, &regs[5], out_message->can_dlc);
    return UNIFIED_OK;
}

/**
 * @brief 初始化 CAN driver。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_init(void)
{
    uint8_t target_mode;

    memset(&g_driver, 0, sizeof(g_driver));
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    g_bitrate = 0u;
    ++g_driver.init_count;

    xl2515_config_spi2_pinmux();
    if (xl2515_spi_init(RTOS_SPI_INIT_HZ) != UNIFIED_OK ||
        xl2515_reset_chip() != UNIFIED_OK ||
        xl2515_wait_mode(XL2515_MODE_CONFIG) != UNIFIED_OK ||
        xl2515_apply_bitrate(RTOS_CAN_BITRATE) != UNIFIED_OK ||
        xl2515_config_accept_all() != UNIFIED_OK ||
        rtos_can_driver_clear_tx_buffers() != UNIFIED_OK ||
        xl2515_clear_interrupts() != UNIFIED_OK ||
        xl2515_write_reg(XL2515_REG_CANINTE, 0x00u) != UNIFIED_OK ||
        xl2515_spi_init(RTOS_SPI_RUN_HZ) != UNIFIED_OK) {
        g_driver.initialized = false;
        return UNIFIED_ERR_INVALID_ARG;
    }

    target_mode = (RTOS_CAN_LOOPBACK_ENABLE != 0u) ? XL2515_MODE_LOOPBACK : XL2515_MODE_NORMAL;
    if (xl2515_set_mode(target_mode) != UNIFIED_OK) {
        g_driver.initialized = false;
        return UNIFIED_ERR_INVALID_ARG;
    }

    g_driver.initialized = true;
    g_driver.listen_only = false;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

/**
 * @brief 配置 CAN bitrate。
 * @param bitrate 目标 bitrate，v1 默认仅支持 RTOS_CAN_BITRATE。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_set_bitrate(uint32_t bitrate)
{
    uint8_t restore_mode;

    if (!g_driver.initialized) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    if ((bitrate != RTOS_CAN_BITRATE) ||
        (RTOS_CAN_BITRATE != 500000u) ||
        (RTOS_XL2515_OSC_HZ != 16000000u)) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    restore_mode = g_driver.listen_only ? XL2515_MODE_LISTEN_ONLY :
                   ((RTOS_CAN_LOOPBACK_ENABLE != 0u) ? XL2515_MODE_LOOPBACK : XL2515_MODE_NORMAL);

    if (xl2515_set_mode(XL2515_MODE_CONFIG) != UNIFIED_OK ||
        xl2515_apply_bitrate(bitrate) != UNIFIED_OK ||
        xl2515_set_mode(restore_mode) != UNIFIED_OK) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

/**
 * @brief 发送一帧 CAN 报文。
 * @param message 待发送的内部 CAN 报文。
 * @return UNIFIED_OK 表示发送完成，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_send(const rtos_can_message_t *message)
{
    uint32_t i;

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

    if (message->can_dlc > RTOS_CAN_CLASSIC_DATA_MAX_LEN) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_SPI;
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (xl2515_check_bus_off() != UNIFIED_OK ||
        xl2515_bit_modify(XL2515_REG_TXB0CTRL, XL2515_TXBCTRL_TXREQ, 0x00u) != UNIFIED_OK ||
        xl2515_load_txb0(message) != UNIFIED_OK ||
        xl2515_request_to_send_txb0() != UNIFIED_OK) {
        ++g_driver.send_count;
        return UNIFIED_ERR_INVALID_ARG;
    }

    for (i = 0u; i < RTOS_XL2515_TX_TIMEOUT_POLLS; ++i) {
        uint8_t txb0ctrl;

        if (xl2515_check_bus_off() != UNIFIED_OK ||
            xl2515_read_reg(XL2515_REG_TXB0CTRL, &txb0ctrl) != UNIFIED_OK) {
            ++g_driver.send_count;
            return UNIFIED_ERR_INVALID_ARG;
        }

        if ((txb0ctrl & XL2515_TXBCTRL_TXREQ) == 0u) {
            xl2515_bit_modify(XL2515_REG_CANINTF, XL2515_CANINTF_TX0IF, 0x00u);
            ++g_driver.send_count;
            g_driver.last_tx_message = *message;
            g_driver.has_last_tx_message = true;
            g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
            return UNIFIED_OK;
        }

        xl2515_delay_us(10u);
    }

    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_TIMEOUT;
    ++g_driver.send_count;
    return UNIFIED_ERR_INVALID_ARG;
}

/**
 * @brief 读取一帧 CAN RX 报文。
 * @param[out] out_message 输出报文。
 * @return UNIFIED_OK 表示读到报文；无 RX 或失败返回错误码。
 */
unified_error_t rtos_can_driver_read(rtos_can_message_t *out_message)
{
    uint8_t status;
    uint8_t canintf;

    if (out_message == NULL) {
        return UNIFIED_ERR_NULL;
    }

    if (!g_driver.initialized) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (xl2515_read_status(&status) != UNIFIED_OK ||
        xl2515_read_reg(XL2515_REG_CANINTF, &canintf) != UNIFIED_OK) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if ((canintf & XL2515_CANINTF_RX0IF) != 0u) {
        if (xl2515_read_rx_buffer(XL2515_REG_RXB0SIDH, out_message) != UNIFIED_OK) {
            return UNIFIED_ERR_INVALID_ARG;
        }
        xl2515_bit_modify(XL2515_REG_CANINTF, XL2515_CANINTF_RX0IF, 0x00u);
    } else if ((canintf & XL2515_CANINTF_RX1IF) != 0u) {
        if (xl2515_read_rx_buffer(XL2515_REG_RXB1SIDH, out_message) != UNIFIED_OK) {
            return UNIFIED_ERR_INVALID_ARG;
        }
        xl2515_bit_modify(XL2515_REG_CANINTF, XL2515_CANINTF_RX1IF, 0x00u);
    } else {
        (void)status;
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NO_RX;
        return UNIFIED_ERR_INVALID_ARG;
    }

    ++g_driver.read_count;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

/**
 * @brief 获取最近一次 driver 错误。
 * @return 最近一次错误枚举。
 */
rtos_can_driver_error_t rtos_can_driver_get_error(void)
{
    return g_driver.last_error;
}

/**
 * @brief 获取 CAN 控制器健康状态。
 * @param[out] out_health 输出健康状态。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_get_health(rtos_can_driver_health_t *out_health)
{
    uint8_t eflg;

    if (out_health == NULL) {
        return UNIFIED_ERR_NULL;
    }

    memset(out_health, 0, sizeof(*out_health));

    if (!g_driver.initialized) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (xl2515_read_reg(XL2515_REG_EFLG, &eflg) != UNIFIED_OK) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_SPI;
        return UNIFIED_ERR_INVALID_ARG;
    }

    out_health->bus_off = (eflg & XL2515_EFLG_TXBO) != 0u;
    out_health->error_passive = (eflg & (XL2515_EFLG_RXEP | XL2515_EFLG_TXEP)) != 0u;
    out_health->rx_overflow = (eflg & (XL2515_EFLG_RX0OVR | XL2515_EFLG_RX1OVR)) != 0u;
    return UNIFIED_OK;
}

/**
 * @brief 取消已请求发送的 TX buffer。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_abort_tx(void)
{
    ++g_driver.abort_tx_count;

    if (!g_driver.initialized) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (xl2515_bit_modify(XL2515_REG_TXB0CTRL, XL2515_TXBCTRL_TXREQ, 0x00u) != UNIFIED_OK ||
        xl2515_bit_modify(XL2515_REG_TXB1CTRL, XL2515_TXBCTRL_TXREQ, 0x00u) != UNIFIED_OK ||
        xl2515_bit_modify(XL2515_REG_TXB2CTRL, XL2515_TXBCTRL_TXREQ, 0x00u) != UNIFIED_OK) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

/**
 * @brief 清空 TX buffer 控制状态。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_clear_tx_buffers(void)
{
    ++g_driver.clear_tx_count;

    if (xl2515_write_reg(XL2515_REG_TXB0CTRL, 0x00u) != UNIFIED_OK ||
        xl2515_write_reg(XL2515_REG_TXB1CTRL, 0x00u) != UNIFIED_OK ||
        xl2515_write_reg(XL2515_REG_TXB2CTRL, 0x00u) != UNIFIED_OK) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

/**
 * @brief 将 CAN 控制器切换到 Listen-Only 模式。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_set_listen_only(void)
{
    if (!g_driver.initialized) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (xl2515_set_mode(XL2515_MODE_LISTEN_ONLY) != UNIFIED_OK) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    g_driver.listen_only = true;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

/**
 * @brief 将 CAN 控制器切换到 Normal 模式。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_set_normal(void)
{
    uint8_t target_mode;

    if (!g_driver.initialized) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    target_mode = (RTOS_CAN_LOOPBACK_ENABLE != 0u) ? XL2515_MODE_LOOPBACK : XL2515_MODE_NORMAL;
    if (xl2515_set_mode(target_mode) != UNIFIED_OK) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    g_driver.listen_only = false;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

/**
 * @brief 复位并重新初始化 CAN 控制器。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_reset(void)
{
    ++g_driver.reset_count;

    if (xl2515_reset_chip() != UNIFIED_OK ||
        xl2515_wait_mode(XL2515_MODE_CONFIG) != UNIFIED_OK ||
        xl2515_apply_bitrate(RTOS_CAN_BITRATE) != UNIFIED_OK ||
        xl2515_config_accept_all() != UNIFIED_OK ||
        rtos_can_driver_clear_tx_buffers() != UNIFIED_OK ||
        xl2515_clear_interrupts() != UNIFIED_OK) {
        g_driver.initialized = false;
        return UNIFIED_ERR_INVALID_ARG;
    }

    g_driver.initialized = true;
    if (rtos_can_driver_set_normal() != UNIFIED_OK) {
        g_driver.initialized = false;
        return UNIFIED_ERR_INVALID_ARG;
    }

    g_driver.listen_only = false;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

/**
 * @brief 获取 mock driver 快照。
 * @param[out] out_snapshot 输出快照；NULL 时忽略。
 */
void rtos_can_driver_get_mock_snapshot(rtos_can_driver_mock_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return;
    }

    *out_snapshot = g_driver;
    out_snapshot->bitrate = g_bitrate;
}

/** @brief 清空 mock RX 注入队列。 */
void rtos_can_driver_mock_reset_rx(void)
{
}

/**
 * @brief 向 mock driver 注入一帧 RX 报文。
 * @param message 待注入报文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_mock_inject_rx(const rtos_can_message_t *message)
{
    if (message == NULL) {
        return UNIFIED_ERR_NULL;
    }

    return UNIFIED_ERR_INVALID_ARG;
}

/**
 * @brief 配置 mock send 错误注入。
 * @param error 注入的错误类型；RTOS_CAN_DRIVER_ERROR_NONE 表示关闭注入。
 * @param fail_count 连续失败次数。
 */
void rtos_can_driver_mock_set_send_error(rtos_can_driver_error_t error,
                                         uint32_t fail_count)
{
    (void)error;
    (void)fail_count;
}

/**
 * @brief 配置 mock driver 健康状态。
 * @param bus_off bus-off 标志。
 * @param error_passive error passive 标志。
 * @param rx_overflow RX overflow 标志。
 */
void rtos_can_driver_mock_set_health(bool bus_off,
                                     bool error_passive,
                                     bool rx_overflow)
{
    (void)bus_off;
    (void)error_passive;
    (void)rx_overflow;
}

#else /* RTOS_CAN_DRIVER_XL2515_ENABLE */

typedef struct {
    rtos_can_message_t slots[RTOS_CAN_MOCK_RX_QUEUE_LEN];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} rtos_can_mock_rx_queue_t;

static rtos_can_mock_rx_queue_t g_mock_rx_queue;
static rtos_can_driver_error_t g_mock_send_error;
static uint32_t g_mock_send_error_count;
static rtos_can_driver_health_t g_mock_health;

static void mock_rx_queue_reset(void)
{
    memset(&g_mock_rx_queue, 0, sizeof(g_mock_rx_queue));
}

static void mock_error_reset(void)
{
    g_mock_send_error = RTOS_CAN_DRIVER_ERROR_NONE;
    g_mock_send_error_count = 0u;
    memset(&g_mock_health, 0, sizeof(g_mock_health));
}

static bool mock_rx_queue_push(const rtos_can_message_t *message)
{
    if ((message == NULL) || (g_mock_rx_queue.count >= RTOS_CAN_MOCK_RX_QUEUE_LEN)) {
        return false;
    }

    g_mock_rx_queue.slots[g_mock_rx_queue.tail] = *message;
    g_mock_rx_queue.tail = (g_mock_rx_queue.tail + 1u) % RTOS_CAN_MOCK_RX_QUEUE_LEN;
    ++g_mock_rx_queue.count;
    return true;
}

static bool mock_rx_queue_pop(rtos_can_message_t *out_message)
{
    if ((out_message == NULL) || (g_mock_rx_queue.count == 0u)) {
        return false;
    }

    *out_message = g_mock_rx_queue.slots[g_mock_rx_queue.head];
    g_mock_rx_queue.head = (g_mock_rx_queue.head + 1u) % RTOS_CAN_MOCK_RX_QUEUE_LEN;
    --g_mock_rx_queue.count;
    return true;
}

/**
 * @brief 初始化 CAN driver。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_init(void)
{
    memset(&g_driver, 0, sizeof(g_driver));
    mock_rx_queue_reset();
    mock_error_reset();
    g_driver.initialized = true;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    g_bitrate = 0u;
    ++g_driver.init_count;
    return UNIFIED_OK;
}

/**
 * @brief 配置 CAN bitrate。
 * @param bitrate 目标 bitrate，v1 默认仅支持 RTOS_CAN_BITRATE。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
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

/**
 * @brief 发送一帧 CAN 报文。
 * @param message 待发送的内部 CAN 报文。
 * @return UNIFIED_OK 表示发送完成，否则返回公共错误码。
 */
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

    if (g_mock_send_error_count > 0u) {
        --g_mock_send_error_count;
        ++g_driver.send_count;
        g_driver.last_error = g_mock_send_error;
        return UNIFIED_ERR_INVALID_ARG;
    }

    ++g_driver.send_count;
    g_driver.last_tx_message = *message;
    g_driver.has_last_tx_message = true;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

/**
 * @brief 读取一帧 CAN RX 报文。
 * @param[out] out_message 输出报文。
 * @return UNIFIED_OK 表示读到报文；无 RX 或失败返回错误码。
 */
unified_error_t rtos_can_driver_read(rtos_can_message_t *out_message)
{
    if (out_message == NULL) {
        return UNIFIED_ERR_NULL;
    }

    if (!g_driver.initialized) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (!mock_rx_queue_pop(out_message)) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NO_RX;
        return UNIFIED_ERR_INVALID_ARG;
    }

    ++g_driver.read_count;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

/**
 * @brief 获取最近一次 driver 错误。
 * @return 最近一次错误枚举。
 */
rtos_can_driver_error_t rtos_can_driver_get_error(void)
{
    return g_driver.last_error;
}

/**
 * @brief 获取 CAN 控制器健康状态。
 * @param[out] out_health 输出健康状态。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_get_health(rtos_can_driver_health_t *out_health)
{
    if (out_health == NULL) {
        return UNIFIED_ERR_NULL;
    }

    if (!g_driver.initialized) {
        g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NOT_READY;
        return UNIFIED_ERR_INVALID_ARG;
    }

    *out_health = g_mock_health;
    return UNIFIED_OK;
}

/**
 * @brief 取消已请求发送的 TX buffer。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_abort_tx(void)
{
    ++g_driver.abort_tx_count;
    return UNIFIED_OK;
}

/**
 * @brief 清空 TX buffer 控制状态。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_clear_tx_buffers(void)
{
    ++g_driver.clear_tx_count;
    return UNIFIED_OK;
}

/**
 * @brief 将 CAN 控制器切换到 Listen-Only 模式。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
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

/**
 * @brief 将 CAN 控制器切换到 Normal 模式。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
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

/**
 * @brief 复位并重新初始化 CAN 控制器。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_reset(void)
{
    ++g_driver.reset_count;
    g_driver.initialized = true;
    g_driver.listen_only = false;
    g_driver.last_error = RTOS_CAN_DRIVER_ERROR_NONE;
    return UNIFIED_OK;
}

/**
 * @brief 获取 mock driver 快照。
 * @param[out] out_snapshot 输出快照；NULL 时忽略。
 */
void rtos_can_driver_get_mock_snapshot(rtos_can_driver_mock_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return;
    }

    *out_snapshot = g_driver;
    out_snapshot->bitrate = g_bitrate;
}

/** @brief 清空 mock RX 注入队列。 */
void rtos_can_driver_mock_reset_rx(void)
{
    mock_rx_queue_reset();
}

/**
 * @brief 向 mock driver 注入一帧 RX 报文。
 * @param message 待注入报文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_mock_inject_rx(const rtos_can_message_t *message)
{
    if (message == NULL) {
        return UNIFIED_ERR_NULL;
    }

    if (!mock_rx_queue_push(message)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

/**
 * @brief 配置 mock send 错误注入。
 * @param error 注入的错误类型；RTOS_CAN_DRIVER_ERROR_NONE 表示关闭注入。
 * @param fail_count 连续失败次数。
 */
void rtos_can_driver_mock_set_send_error(rtos_can_driver_error_t error,
                                         uint32_t fail_count)
{
    g_mock_send_error = error;
    g_mock_send_error_count = (error == RTOS_CAN_DRIVER_ERROR_NONE) ? 0u : fail_count;
}

/**
 * @brief 配置 mock driver 健康状态。
 * @param bus_off bus-off 标志。
 * @param error_passive error passive 标志。
 * @param rx_overflow RX overflow 标志。
 */
void rtos_can_driver_mock_set_health(bool bus_off,
                                     bool error_passive,
                                     bool rx_overflow)
{
    g_mock_health.bus_off = bus_off;
    g_mock_health.error_passive = error_passive;
    g_mock_health.rx_overflow = rx_overflow;
}

#endif /* RTOS_CAN_DRIVER_XL2515_ENABLE */
