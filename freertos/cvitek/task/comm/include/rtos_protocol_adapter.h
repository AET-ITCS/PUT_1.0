/**
 * @file rtos_protocol_adapter.h
 * @brief FreeRTOS 协议适配层接口。
 *
 * 当前默认支持 96 字节 unified_frame_t 作为仓库内联调 payload，同时保留
 * 可替换解析 hook，便于真实共享内存/cmdqu 接入前后复用同一边界。
 */
#ifndef RTOS_PROTOCOL_ADAPTER_H
#define RTOS_PROTOCOL_ADAPTER_H

#include <stdint.h>

#include "error_code.h"
#include "rtos_can_message.h"
#include "rtos_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t length;        // payload 实际长度，单位字节。

    uint8_t bytes[RTOS_IPC_PAYLOAD_MAX_LEN];      // payload 字节内容；格式 TBD。
} rtos_ipc_payload_t;                             // 小核内部 opaque IPC payload 存储容器。

typedef struct {
    const uint8_t *bytes;     // payload 只读字节指针。

    uint16_t length;          // payload 长度，单位字节。
} rtos_ipc_payload_view_t;    // 传给协议适配 hook 的只读 payload view。

typedef unified_error_t (*rtos_protocol_adapter_linux_payload_to_can_fn_t)(
    const rtos_ipc_payload_view_t *payload,
    rtos_can_message_t *out_message);      // Linux payload 转 CAN message 的可替换适配函数；payload 为输入 view，out_message 输出内部 CAN 消息。

void rtos_protocol_adapter_init(void);

void rtos_protocol_adapter_set_linux_payload_to_can(
    rtos_protocol_adapter_linux_payload_to_can_fn_t handler);

unified_error_t rtos_protocol_adapter_linux_payload_to_can(
    const rtos_ipc_payload_t *payload,
    rtos_can_message_t *out_message);

unified_error_t rtos_protocol_adapter_can_rx_to_linux_payload(
    const rtos_can_message_t *message,
    rtos_ipc_payload_t *out_payload);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_PROTOCOL_ADAPTER_H */
