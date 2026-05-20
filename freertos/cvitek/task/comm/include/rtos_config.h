/* FreeRTOS comm 默认配置：小核 CAN 转发 mock 链路使用的私有参数。 */
#ifndef RTOS_CONFIG_H
#define RTOS_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTOS_CAN_TX_QUEUE_LEN 32u
#define RTOS_CAN_RX_QUEUE_LEN 32u
#define RTOS_STATUS_PERIOD_MS 1000u
#define RTOS_LINUX_HEARTBEAT_TIMEOUT_MS 3000u
#define RTOS_CAN_BITRATE 500000u
#define RTOS_XL2515_OSC_HZ 16000000u
#define RTOS_SPI_INIT_HZ 1000000u
#define RTOS_SPI_RUN_HZ 8000000u
#define RTOS_CAN_TX_RETRY_MAX 2u
#define RTOS_CAN_LOOPBACK_ENABLE 0u
#define RTOS_FAIL_SAFE_LISTEN_ONLY_ENABLE 1u
#define RTOS_LINUX_REHANDSHAKE_REQUIRED 1u

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CONFIG_H */
