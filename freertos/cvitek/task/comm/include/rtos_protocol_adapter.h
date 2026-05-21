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

/** @brief 小核内部 opaque IPC payload 存储容器。 */
typedef struct {
    /** @brief payload 实际长度，单位字节。 */
    uint16_t length;

    /** @brief payload 字节内容；格式 TBD。 */
    uint8_t bytes[RTOS_IPC_PAYLOAD_MAX_LEN];
} rtos_ipc_payload_t;

/** @brief 传给协议适配 hook 的只读 payload view。 */
typedef struct {
    /** @brief payload 只读字节指针。 */
    const uint8_t *bytes;

    /** @brief payload 长度，单位字节。 */
    uint16_t length;
} rtos_ipc_payload_view_t;

/**
 * @brief Linux payload 转 CAN message 的可替换适配函数。
 * @param payload Linux->RTOS opaque payload view。
 * @param[out] out_message 输出内部 CAN 消息。
 * @return UNIFIED_OK 表示成功解析，否则返回公共错误码。
 */
typedef unified_error_t (*rtos_protocol_adapter_linux_payload_to_can_fn_t)(
    const rtos_ipc_payload_view_t *payload,
    rtos_can_message_t *out_message);

/** @brief 初始化协议适配层占位状态。 */
void rtos_protocol_adapter_init(void);

/**
 * @brief 注册 Linux payload 转 CAN message 的适配 hook。
 * @param handler 适配函数；NULL 表示恢复默认 unified_frame_t 解析。
 */
void rtos_protocol_adapter_set_linux_payload_to_can(
    rtos_protocol_adapter_linux_payload_to_can_fn_t handler);

/**
 * @brief 将 opaque Linux payload 转换为内部 CAN 消息。
 * @param payload 输入 payload。
 * @param[out] out_message 输出 CAN 消息。
 * @return UNIFIED_OK 表示成功；未注册 hook 时使用默认 unified_frame_t 解析。
 */
unified_error_t rtos_protocol_adapter_linux_payload_to_can(
    const rtos_ipc_payload_t *payload,
    rtos_can_message_t *out_message);

/**
 * @brief 将 CAN RX 消息打包为 RTOS->Linux payload。
 *
 * 阶段 6 仓库内联调默认复用 unified_frame_t。该函数不实现真实共享内存
 * 写入，只生成可交给 rtos_ipc payload sender hook 的 payload。
 *
 * @param message CAN RX 消息。
 * @param[out] out_payload 输出 RTOS->Linux payload。
 * @return UNIFIED_OK 表示成功。
 */
unified_error_t rtos_protocol_adapter_can_rx_to_linux_payload(
    const rtos_can_message_t *message,
    rtos_ipc_payload_t *out_payload);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_PROTOCOL_ADAPTER_H */
