/**
 * @file rtos_ipc.h
 * @brief FreeRTOS comm IPC 占位和回传 hook。
 *
 * 本文件只提供小核内部 IPC 接入点，不定义共享内存 ring、cmdqu/mailbox
 * 命令或 payload ABI。真实共享内存模块接入后可替换这些 hook。
 */
#ifndef RTOS_IPC_H
#define RTOS_IPC_H

#include <stdint.h>

#include "error_code.h"
#include "rtos_can_message.h"
#include "rtos_protocol_adapter.h"
#include "rtos_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief RTOS->Linux 错误/状态事件占位结构。 */
typedef struct {
    /** @brief 事件类型，取值见 @ref rtos_ipc_event_type_t。 */
    uint32_t event_type;

    /** @brief 事件附加信息，由事件类型解释。 */
    uint32_t detail;
} rtos_ipc_event_t;

/** @brief RTOS->Linux 事件类型。 */
typedef enum {
    /** @brief Linux heartbeat 超时。 */
    RTOS_IPC_EVENT_LINUX_HEARTBEAT_TIMEOUT = 1u,
    /** @brief CAN bus-off。 */
    RTOS_IPC_EVENT_CAN_BUS_OFF = 2u,
    /** @brief SPI/driver 错误。 */
    RTOS_IPC_EVENT_SPI_ERROR = 3u,
    /** @brief XL2515 RX overflow。 */
    RTOS_IPC_EVENT_RX_OVERFLOW = 4u,
} rtos_ipc_event_type_t;

/** @brief CAN RX 回传 hook 类型。 */
typedef unified_error_t (*rtos_ipc_can_rx_sender_fn_t)(const rtos_can_message_t *message);

/** @brief RTOS->Linux payload 回传 hook 类型。 */
typedef unified_error_t (*rtos_ipc_payload_sender_fn_t)(const rtos_ipc_payload_t *payload);

/** @brief 状态快照回传 hook 类型。 */
typedef unified_error_t (*rtos_ipc_status_sender_fn_t)(const rtos_status_snapshot_t *status);

/** @brief 错误事件回传 hook 类型。 */
typedef unified_error_t (*rtos_ipc_event_sender_fn_t)(const rtos_ipc_event_t *event);

/**
 * @brief 初始化 IPC mock 队列和回传 hook。
 * @return UNIFIED_OK 表示成功。
 */
unified_error_t rtos_ipc_init(void);

/**
 * @brief 阶段 2 兼容入口：直接注入一帧 CAN message。
 * @param message 待提交 CAN message。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_ipc_mock_receive_can_message(const rtos_can_message_t *message);

/**
 * @brief 向 mock Linux->RTOS payload queue 注入 opaque payload。
 * @param bytes payload 字节指针；length 为 0 时可为 NULL。
 * @param length payload 长度。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_ipc_mock_receive_payload(const uint8_t *bytes, uint16_t length);

/**
 * @brief 非阻塞读取一个 Linux->RTOS mock payload。
 * @param[out] out_payload 输出 payload。
 * @return UNIFIED_OK 表示读到 payload；空队列返回错误码。
 */
unified_error_t rtos_ipc_poll_linux_payload(rtos_ipc_payload_t *out_payload);

/**
 * @brief 注册 CAN RX 回传 hook。
 * @param sender 回传函数；NULL 表示使用默认 no-op。
 */
void rtos_ipc_set_can_rx_sender(rtos_ipc_can_rx_sender_fn_t sender);

/**
 * @brief 注册 RTOS->Linux payload 回传 hook。
 * @param sender 回传函数；NULL 表示使用默认 no-op。
 */
void rtos_ipc_set_payload_sender(rtos_ipc_payload_sender_fn_t sender);

/**
 * @brief 注册状态快照回传 hook。
 * @param sender 回传函数；NULL 表示使用默认 no-op。
 */
void rtos_ipc_set_status_sender(rtos_ipc_status_sender_fn_t sender);

/**
 * @brief 注册错误事件回传 hook。
 * @param sender 回传函数；NULL 表示使用默认 no-op。
 */
void rtos_ipc_set_event_sender(rtos_ipc_event_sender_fn_t sender);

/**
 * @brief 通过回传 hook 发送 CAN RX 消息。
 * @param message CAN RX 消息。
 * @return UNIFIED_OK 表示发送成功或未注册 hook；hook 失败返回错误码。
 */
unified_error_t rtos_ipc_send_can_rx(const rtos_can_message_t *message);

/**
 * @brief 通过回传 hook 发送状态快照。
 * @param status 状态快照。
 * @return UNIFIED_OK 表示发送成功或未注册 hook；hook 失败返回错误码。
 */
unified_error_t rtos_ipc_send_status(const rtos_status_snapshot_t *status);

/**
 * @brief 通过回传 hook 发送错误事件。
 * @param event 错误事件。
 * @return UNIFIED_OK 表示发送成功或未注册 hook；hook 失败返回错误码。
 */
unified_error_t rtos_ipc_send_error_event(const rtos_ipc_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_IPC_H */
