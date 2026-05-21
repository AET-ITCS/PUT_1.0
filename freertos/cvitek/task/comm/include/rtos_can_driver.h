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

/** @brief CAN driver 最近一次错误类型。 */
typedef enum {
    /** @brief 无错误。 */
    RTOS_CAN_DRIVER_ERROR_NONE = 0,
    /** @brief driver 尚未初始化或设备不可用。 */
    RTOS_CAN_DRIVER_ERROR_NOT_READY,
    /** @brief 控制器处于 Listen-Only，禁止发送。 */
    RTOS_CAN_DRIVER_ERROR_LISTEN_ONLY,
    /** @brief 当前没有可读取的 CAN RX 报文。 */
    RTOS_CAN_DRIVER_ERROR_NO_RX,
    /** @brief SPI 读写或寄存器访问错误。 */
    RTOS_CAN_DRIVER_ERROR_SPI,
    /** @brief CAN bus-off。 */
    RTOS_CAN_DRIVER_ERROR_BUS_OFF,
    /** @brief 等待模式切换或 TX 完成超时。 */
    RTOS_CAN_DRIVER_ERROR_TIMEOUT,
} rtos_can_driver_error_t;

/** @brief mock driver 快照，用于 host 测试和调试。 */
typedef struct {
    /** @brief 初始化调用次数。 */
    uint32_t init_count;
    /** @brief 发送调用次数。 */
    uint32_t send_count;
    /** @brief 读取调用次数。 */
    uint32_t read_count;
    /** @brief reset 调用次数。 */
    uint32_t reset_count;
    /** @brief abort TX 调用次数。 */
    uint32_t abort_tx_count;
    /** @brief clear TX buffer 调用次数。 */
    uint32_t clear_tx_count;
    /** @brief 当前 bitrate。 */
    uint32_t bitrate;
    /** @brief 是否已有成功发送的最后一帧。 */
    bool has_last_tx_message;
    /** @brief 最近一次成功发送的 CAN message。 */
    rtos_can_message_t last_tx_message;
    /** @brief driver 是否已初始化。 */
    bool initialized;
    /** @brief driver 是否处于 Listen-Only。 */
    bool listen_only;
    /** @brief 最近一次错误。 */
    rtos_can_driver_error_t last_error;
} rtos_can_driver_mock_snapshot_t;

/** @brief CAN 控制器健康状态快照。 */
typedef struct {
    /** @brief CAN bus-off 标志。 */
    bool bus_off;
    /** @brief CAN error passive 标志。 */
    bool error_passive;
    /** @brief RX buffer overflow/overrun 标志。 */
    bool rx_overflow;
} rtos_can_driver_health_t;

/**
 * @brief 初始化 CAN driver。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_init(void);

/**
 * @brief 配置 CAN bitrate。
 * @param bitrate 目标 bitrate，v1 默认仅支持 RTOS_CAN_BITRATE。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_set_bitrate(uint32_t bitrate);

/**
 * @brief 发送一帧 CAN 报文。
 * @param message 待发送的内部 CAN 报文。
 * @return UNIFIED_OK 表示发送完成，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_send(const rtos_can_message_t *message);

/**
 * @brief 读取一帧 CAN RX 报文。
 * @param[out] out_message 输出报文。
 * @return UNIFIED_OK 表示读到报文；无 RX 或失败返回错误码。
 */
unified_error_t rtos_can_driver_read(rtos_can_message_t *out_message);

/**
 * @brief 获取最近一次 driver 错误。
 * @return 最近一次错误枚举。
 */
rtos_can_driver_error_t rtos_can_driver_get_error(void);

/**
 * @brief 获取 CAN 控制器健康状态。
 * @param[out] out_health 输出健康状态。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_get_health(rtos_can_driver_health_t *out_health);

/**
 * @brief 取消已请求发送的 TX buffer。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_abort_tx(void);

/**
 * @brief 清空 TX buffer 控制状态。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_clear_tx_buffers(void);

/**
 * @brief 将 CAN 控制器切换到 Listen-Only 模式。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_set_listen_only(void);

/**
 * @brief 将 CAN 控制器切换到 Normal 模式。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_set_normal(void);

/**
 * @brief 复位并重新初始化 CAN 控制器。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_reset(void);

/**
 * @brief 获取 mock driver 快照。
 * @param[out] out_snapshot 输出快照；NULL 时忽略。
 */
void rtos_can_driver_get_mock_snapshot(rtos_can_driver_mock_snapshot_t *out_snapshot);

/** @brief 清空 mock RX 注入队列。 */
void rtos_can_driver_mock_reset_rx(void);

/**
 * @brief 向 mock driver 注入一帧 RX 报文。
 * @param message 待注入报文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_can_driver_mock_inject_rx(const rtos_can_message_t *message);

/**
 * @brief 配置 mock send 错误注入。
 * @param error 注入的错误类型；RTOS_CAN_DRIVER_ERROR_NONE 表示关闭注入。
 * @param fail_count 连续失败次数。
 */
void rtos_can_driver_mock_set_send_error(rtos_can_driver_error_t error,
                                         uint32_t fail_count);

/**
 * @brief 配置 mock driver 健康状态。
 * @param bus_off bus-off 标志。
 * @param error_passive error passive 标志。
 * @param rx_overflow RX overflow 标志。
 */
void rtos_can_driver_mock_set_health(bool bus_off,
                                     bool error_passive,
                                     bool rx_overflow);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_DRIVER_H */
