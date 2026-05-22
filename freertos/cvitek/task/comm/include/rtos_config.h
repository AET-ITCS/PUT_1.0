/**
 * @file rtos_config.h
 * @brief FreeRTOS comm 模块默认配置。
 *
 * 所有宏都允许在编译命令中预定义后覆盖，便于硬件目标、host mock
 * 测试和后续压力测试使用不同参数。
 */
#ifndef RTOS_CONFIG_H
#define RTOS_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RTOS_CAN_TX_QUEUE_LEN
#define RTOS_CAN_TX_QUEUE_LEN 32u                 // CAN TX 软件队列长度。
#endif

#ifndef RTOS_CAN_RX_QUEUE_LEN
#define RTOS_CAN_RX_QUEUE_LEN 32u                 // CAN RX 软件队列长度预留。
#endif

#ifndef RTOS_CAN_MOCK_RX_QUEUE_LEN
#define RTOS_CAN_MOCK_RX_QUEUE_LEN 8u             // host mock CAN RX 注入队列长度。
#endif

#ifndef RTOS_IPC_MOCK_RX_QUEUE_LEN
#define RTOS_IPC_MOCK_RX_QUEUE_LEN 8u             // host mock Linux->RTOS payload 队列长度。
#endif

#ifndef RTOS_IPC_PAYLOAD_MAX_LEN
#define RTOS_IPC_PAYLOAD_MAX_LEN 128u             // IPC opaque payload 最大长度；不作为大小核正式 ABI。
#endif

#ifndef RTOS_STATUS_PERIOD_MS
#define RTOS_STATUS_PERIOD_MS 1000u               // 状态上报周期，单位毫秒。
#endif

#ifndef RTOS_LINUX_HEARTBEAT_TIMEOUT_MS
#define RTOS_LINUX_HEARTBEAT_TIMEOUT_MS 3000u     // Linux heartbeat 超时阈值，单位毫秒。
#endif

#ifndef RTOS_CAN_BITRATE
#define RTOS_CAN_BITRATE 500000u                  // 默认 CAN bitrate。
#endif

#ifndef RTOS_XL2515_OSC_HZ
#define RTOS_XL2515_OSC_HZ 16000000u              // XL2515 外部晶振频率。
#endif

#ifndef RTOS_SPI_INIT_HZ
#define RTOS_SPI_INIT_HZ 1000000u                 // SPI 初始化阶段频率。
#endif

#ifndef RTOS_SPI_RUN_HZ
#define RTOS_SPI_RUN_HZ 8000000u                  // SPI 稳定运行阶段频率。
#endif

#ifndef RTOS_CAN_TX_RETRY_MAX
#define RTOS_CAN_TX_RETRY_MAX 2u                  // CAN TX 短暂错误最大重试次数。
#endif

#ifndef RTOS_CAN_LOOPBACK_ENABLE
#define RTOS_CAN_LOOPBACK_ENABLE 0u               // 非零时 XL2515 初始化后进入 loopback 模式。
#endif

#ifndef RTOS_FAIL_SAFE_LISTEN_ONLY_ENABLE
#define RTOS_FAIL_SAFE_LISTEN_ONLY_ENABLE 1u      // 非零时 fail-safe offline 切入 Listen-Only。
#endif

#ifndef RTOS_LINUX_REHANDSHAKE_REQUIRED
#define RTOS_LINUX_REHANDSHAKE_REQUIRED 1u        // 非零时 Linux 恢复必须重新握手后才恢复 TX。
#endif

#ifndef RTOS_XL2515_MODE_TIMEOUT_POLLS
#define RTOS_XL2515_MODE_TIMEOUT_POLLS 1000u      // XL2515 模式切换等待轮询次数。
#endif

#ifndef RTOS_XL2515_TX_TIMEOUT_POLLS
#define RTOS_XL2515_TX_TIMEOUT_POLLS 10000u       // XL2515 TX 完成等待轮询次数。
#endif

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CONFIG_H */
