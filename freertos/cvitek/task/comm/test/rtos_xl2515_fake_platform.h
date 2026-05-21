#ifndef RTOS_XL2515_FAKE_PLATFORM_H
#define RTOS_XL2515_FAKE_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_can_message.h"

#ifdef __cplusplus
extern "C" {
#endif

void rtos_xl2515_fake_reset(void);
void rtos_xl2515_fake_set_tx_auto_complete(bool enabled);
void rtos_xl2515_fake_set_bus_off(bool enabled);
void rtos_xl2515_fake_load_rx0(const rtos_can_message_t *message);
uint8_t rtos_xl2515_fake_read_reg(uint8_t reg);
uint32_t rtos_xl2515_fake_get_spi_init_count(void);
uint32_t rtos_xl2515_fake_get_last_spi_hz(void);
uint32_t rtos_xl2515_fake_get_pinmux_count(void);
uint32_t rtos_xl2515_fake_get_reset_command_count(void);

unified_error_t rtos_xl2515_fake_spi_init(uint32_t hz);
unified_error_t rtos_xl2515_fake_spi_xfer(const uint8_t *tx_buf,
                                          uint8_t *rx_buf,
                                          uint16_t length);
void rtos_xl2515_fake_delay_us(uint32_t usec);
void rtos_xl2515_fake_config_spi2_pinmux(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_XL2515_FAKE_PLATFORM_H */
