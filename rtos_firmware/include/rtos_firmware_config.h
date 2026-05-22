/**
 * @file rtos_firmware_config.h
 * @brief rtos_firmware 默认配置。
 * @author Yukikaze
 */
#ifndef RTOS_FIRMWARE_CONFIG_H
#define RTOS_FIRMWARE_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 默认 Linux heartbeat 超时时间，单位毫秒。 */
#ifndef RTOS_FIRMWARE_LINUX_HEARTBEAT_TIMEOUT_MS
#define RTOS_FIRMWARE_LINUX_HEARTBEAT_TIMEOUT_MS 3000u
#endif

/** @brief 默认 CAN bitrate，后续真实 CAN 驱动接入时使用。 */
#ifndef RTOS_FIRMWARE_CAN_BITRATE
#define RTOS_FIRMWARE_CAN_BITRATE 500000u
#endif

/** @brief 默认是否允许构建 host smoke executable。 */
#ifndef RTOS_FIRMWARE_HOST_SMOKE_ENABLE
#define RTOS_FIRMWARE_HOST_SMOKE_ENABLE 1u
#endif

#ifdef __cplusplus
}
#endif

#endif /* RTOS_FIRMWARE_CONFIG_H */
