/**
 * @file rtos_can_driver.h
 * @brief XL2515/CAN driver 抽象接口。
 *
 * 业务层只通过本文件操作 CAN 控制器，不直接访问 XL2515 寄存器或 SPI。
 * 默认 host 构建使用 mock driver；硬件目标通过编译开关启用 XL2515 实现。
 */
#ifndef RTOS_CAN_DRIVER_H
#define RTOS_CAN_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_can_message.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTOS_CAN_DRIVER_ERROR_NONE = 0,       // 0 表示无错误，非 0 错误码表示有错误。
    RTOS_CAN_DRIVER_ERROR_NOT_READY,    // 设备未准备好，可能是未初始化或硬件问题。
    RTOS_CAN_DRIVER_ERROR_LISTEN_ONLY,  // 当前处于 Listen-Only 模式，无法发送报文。
    RTOS_CAN_DRIVER_ERROR_NO_RX,        // 没有可读取的 RX 报文，可能是 RX buffer 为空。
    RTOS_CAN_DRIVER_ERROR_SPI,          // SPI 通信错误，可能是总线故障或设备响应异常。
    RTOS_CAN_DRIVER_ERROR_BUS_OFF,      // CAN 控制器进入 bus-off 状态，无法正常通信。
    RTOS_CAN_DRIVER_ERROR_TIMEOUT,      // 等待模式切换或 TX 完成时发生超时，可能是硬件响应迟缓或异常。
} rtos_can_driver_error_t;              // CAN driver 最近一次错误类型。

typedef struct {
    uint32_t init_count;            // 统计 init 调用次数，验证初始化流程。
    uint32_t send_count;            // 统计 send 调用次数，验证发送流程和错误注入。
    uint32_t read_count;            // 统计 read 调用次数，验证读取流程和 RX 注入。
    uint32_t reset_count;           // 统计 reset 调用次数，验证重置流程。
    uint32_t abort_tx_count;        // 统计 abort_tx 调用次数，验证 TX 中止流程。
    uint32_t clear_tx_count;        // 统计 clear_tx_buffers 调用次数，验证 TX buffer 清空流程。
    uint32_t bitrate;               // 记录当前 bitrate，验证 bitrate 配置流程。
    bool has_last_tx_message;       // 标志是否有成功发送的最后一帧，用于验证发送结果和错误注入。
    rtos_can_message_t last_tx_message;      // 记录最近一次成功发送的 CAN message，验证发送内容和错误注入。
    bool initialized;               // 标志 driver 是否已初始化，验证初始化流程和错误注入。
    bool listen_only;               // 标志 driver 是否处于 Listen-Only 模式，验证模式切换流程和错误注入。
    rtos_can_driver_error_t last_error;      // 记录最近一次错误，验证错误注入和错误处理流程。
} rtos_can_driver_mock_snapshot_t;           // mock driver 快照，用于 host 测试和调试。

typedef struct {
    bool bus_off;           // 进入 bus-off 状态表示控制器已检测到过多错误，已断开总线通信。
    bool error_passive;     // error passive 状态表示控制器错误计数器超过被动阈值，通信能力受限。
    bool rx_overflow;       // RX buffer overflow 表示接收缓冲区溢出，可能导致丢失报文或通信异常。
} rtos_can_driver_health_t; // CAN 控制器健康状态快照。

unified_error_t rtos_can_driver_init(void);

unified_error_t rtos_can_driver_set_bitrate(uint32_t bitrate);

unified_error_t rtos_can_driver_send(const rtos_can_message_t *message);

unified_error_t rtos_can_driver_read(rtos_can_message_t *out_message);

rtos_can_driver_error_t rtos_can_driver_get_error(void);

unified_error_t rtos_can_driver_get_health(rtos_can_driver_health_t *out_health);

unified_error_t rtos_can_driver_abort_tx(void);

unified_error_t rtos_can_driver_clear_tx_buffers(void);

unified_error_t rtos_can_driver_set_listen_only(void);

unified_error_t rtos_can_driver_set_normal(void);

unified_error_t rtos_can_driver_reset(void);

void rtos_can_driver_get_mock_snapshot(rtos_can_driver_mock_snapshot_t *out_snapshot);

void rtos_can_driver_mock_reset_rx(void);

unified_error_t rtos_can_driver_mock_inject_rx(const rtos_can_message_t *message);

void rtos_can_driver_mock_set_send_error(rtos_can_driver_error_t error,
                                         uint32_t fail_count);

void rtos_can_driver_mock_set_health(bool bus_off,
                                     bool error_passive,
                                     bool rx_overflow);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_DRIVER_H */
