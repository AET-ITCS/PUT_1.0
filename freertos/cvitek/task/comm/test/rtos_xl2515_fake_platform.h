/**
 * @file rtos_xl2515_fake_platform.h
 * @brief XL2515 host fake platform hooks used by driver tests.
 */
#ifndef RTOS_XL2515_FAKE_PLATFORM_H
#define RTOS_XL2515_FAKE_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_can_message.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reset fake XL2515 register state. */
void rtos_xl2515_fake_reset(void);

/** @brief Enable or disable automatic TX completion. */
void rtos_xl2515_fake_set_tx_auto_complete(bool enabled);

/** @brief Set or clear fake bus-off flag. */
void rtos_xl2515_fake_set_bus_off(bool enabled);

/** @brief Load one CAN frame into fake RXB0. */
void rtos_xl2515_fake_load_rx0(const rtos_can_message_t *message);

/** @brief Read one fake XL2515 register. */
uint8_t rtos_xl2515_fake_read_reg(uint8_t reg);

/** @brief Get fake SPI init call count. */
uint32_t rtos_xl2515_fake_get_spi_init_count(void);

/** @brief Get last SPI frequency passed to fake platform. */
uint32_t rtos_xl2515_fake_get_last_spi_hz(void);

/** @brief Get SPI2 pinmux configuration call count. */
uint32_t rtos_xl2515_fake_get_pinmux_count(void);

/** @brief Get XL2515 reset command count. */
uint32_t rtos_xl2515_fake_get_reset_command_count(void);

/** @brief Fake SPI init hook used by rtos_can_driver.c. */
unified_error_t rtos_xl2515_fake_spi_init(uint32_t hz);

/** @brief Fake SPI transfer hook used by rtos_can_driver.c. */
unified_error_t rtos_xl2515_fake_spi_xfer(const uint8_t *tx_buf,
                                          uint8_t *rx_buf,
                                          uint16_t length);

/** @brief Fake microsecond delay hook. */
void rtos_xl2515_fake_delay_us(uint32_t usec);

/** @brief Fake SPI2 pinmux hook. */
void rtos_xl2515_fake_config_spi2_pinmux(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_XL2515_FAKE_PLATFORM_H */
