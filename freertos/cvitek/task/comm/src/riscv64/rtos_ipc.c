/**
 * @file rtos_ipc.c
 * @brief FreeRTOS comm IPC mock 和回传 hook 实现。
 *
 * 本模块提供 Linux->RTOS mock payload queue 以及 RTOS->Linux CAN RX、
 * status、event 回传 hook。真实共享内存/cmdqu 后续从这些边界接入。
 */
#include "rtos_ipc.h"

#include <string.h>

#include "rtos_can_forward.h"
#include "rtos_config.h"

/** @brief Linux->RTOS mock payload 环形队列。 */
typedef struct {
    /** @brief payload 槽位。 */
    rtos_ipc_payload_t slots[RTOS_IPC_MOCK_RX_QUEUE_LEN];
    /** @brief 下一个读取位置。 */
    uint32_t head;
    /** @brief 下一个写入位置。 */
    uint32_t tail;
    /** @brief 当前队列深度。 */
    uint32_t count;
} rtos_ipc_payload_queue_t;

static rtos_ipc_payload_queue_t g_linux_rx_queue;
static rtos_ipc_can_rx_sender_fn_t g_can_rx_sender;
static rtos_ipc_payload_sender_fn_t g_payload_sender;
static rtos_ipc_status_sender_fn_t g_status_sender;
static rtos_ipc_event_sender_fn_t g_event_sender;

static void payload_queue_reset(void)
{
    memset(&g_linux_rx_queue, 0, sizeof(g_linux_rx_queue));
}

static bool payload_queue_push(const rtos_ipc_payload_t *payload)
{
    if ((payload == 0) || (g_linux_rx_queue.count >= RTOS_IPC_MOCK_RX_QUEUE_LEN)) {
        return false;
    }

    g_linux_rx_queue.slots[g_linux_rx_queue.tail] = *payload;
    g_linux_rx_queue.tail = (g_linux_rx_queue.tail + 1u) % RTOS_IPC_MOCK_RX_QUEUE_LEN;
    ++g_linux_rx_queue.count;
    return true;
}

static bool payload_queue_pop(rtos_ipc_payload_t *out_payload)
{
    if ((out_payload == 0) || (g_linux_rx_queue.count == 0u)) {
        return false;
    }

    *out_payload = g_linux_rx_queue.slots[g_linux_rx_queue.head];
    g_linux_rx_queue.head = (g_linux_rx_queue.head + 1u) % RTOS_IPC_MOCK_RX_QUEUE_LEN;
    --g_linux_rx_queue.count;
    return true;
}

static unified_error_t complete_rtos_to_linux_send(unified_error_t result)
{
    if (result == UNIFIED_OK) {
        rtos_status_inc_tx_to_linux();
    } else {
        rtos_status_inc_drop_ring_full();
    }

    return result;
}

/**
 * @brief 初始化 IPC mock 队列和回传 hook。
 * @return UNIFIED_OK 表示成功。
 */
unified_error_t rtos_ipc_init(void)
{
    payload_queue_reset();
    g_can_rx_sender = 0;
    g_payload_sender = 0;
    g_status_sender = 0;
    g_event_sender = 0;
    return UNIFIED_OK;
}

/**
 * @brief 阶段 2 兼容入口：直接注入一帧 CAN message。
 * @param message 待提交 CAN message。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_ipc_mock_receive_can_message(const rtos_can_message_t *message)
{
    return rtos_can_forward_submit_message(message);
}

/**
 * @brief 向 mock Linux->RTOS payload queue 注入 opaque payload。
 * @param bytes payload 字节指针；length 为 0 时可为 NULL。
 * @param length payload 长度。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_ipc_mock_receive_payload(const uint8_t *bytes, uint16_t length)
{
    rtos_ipc_payload_t payload;

    if ((bytes == 0) && (length > 0u)) {
        rtos_status_inc_ipc_payload_drop();
        return UNIFIED_ERR_NULL;
    }

    if (length > RTOS_IPC_PAYLOAD_MAX_LEN) {
        rtos_status_inc_ipc_payload_drop();
        return UNIFIED_ERR_LENGTH;
    }

    memset(&payload, 0, sizeof(payload));
    payload.length = length;
    if (length > 0u) {
        memcpy(payload.bytes, bytes, length);
    }

    if (!payload_queue_push(&payload)) {
        rtos_status_inc_ipc_payload_drop();
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

/**
 * @brief 非阻塞读取一个 Linux->RTOS mock payload。
 * @param[out] out_payload 输出 payload。
 * @return UNIFIED_OK 表示读到 payload；空队列返回错误码。
 */
unified_error_t rtos_ipc_poll_linux_payload(rtos_ipc_payload_t *out_payload)
{
    if (out_payload == 0) {
        return UNIFIED_ERR_NULL;
    }

    if (!payload_queue_pop(out_payload)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

/**
 * @brief 注册 CAN RX 回传 hook。
 * @param sender 回传函数；NULL 表示使用默认 no-op。
 */
void rtos_ipc_set_can_rx_sender(rtos_ipc_can_rx_sender_fn_t sender)
{
    g_can_rx_sender = sender;
}

/**
 * @brief 注册 RTOS->Linux payload 回传 hook。
 * @param sender 回传函数；NULL 表示使用默认 no-op。
 */
void rtos_ipc_set_payload_sender(rtos_ipc_payload_sender_fn_t sender)
{
    g_payload_sender = sender;
}

/**
 * @brief 注册状态快照回传 hook。
 * @param sender 回传函数；NULL 表示使用默认 no-op。
 */
void rtos_ipc_set_status_sender(rtos_ipc_status_sender_fn_t sender)
{
    g_status_sender = sender;
}

/**
 * @brief 注册错误事件回传 hook。
 * @param sender 回传函数；NULL 表示使用默认 no-op。
 */
void rtos_ipc_set_event_sender(rtos_ipc_event_sender_fn_t sender)
{
    g_event_sender = sender;
}

/**
 * @brief 通过回传 hook 发送 CAN RX 消息。
 * @param message CAN RX 消息。
 * @return UNIFIED_OK 表示发送成功或未注册 hook；hook 失败返回错误码。
 */
unified_error_t rtos_ipc_send_can_rx(const rtos_can_message_t *message)
{
    unified_error_t result = UNIFIED_OK;
    rtos_ipc_payload_t payload;

    if (message == 0) {
        return UNIFIED_ERR_NULL;
    }

    if (g_can_rx_sender != 0) {
        result = g_can_rx_sender(message);
    }

    if ((result == UNIFIED_OK) && (g_payload_sender != 0)) {
        result = rtos_protocol_adapter_can_rx_to_linux_payload(message, &payload);
        if (result == UNIFIED_OK) {
            result = g_payload_sender(&payload);
        }
    }

    return complete_rtos_to_linux_send(result);
}

/**
 * @brief 通过回传 hook 发送状态快照。
 * @param status 状态快照。
 * @return UNIFIED_OK 表示发送成功或未注册 hook；hook 失败返回错误码。
 */
unified_error_t rtos_ipc_send_status(const rtos_status_snapshot_t *status)
{
    unified_error_t result = UNIFIED_OK;

    if (status == 0) {
        return UNIFIED_ERR_NULL;
    }

    if (g_status_sender != 0) {
        result = g_status_sender(status);
    }

    return complete_rtos_to_linux_send(result);
}

/**
 * @brief 通过回传 hook 发送错误事件。
 * @param event 错误事件。
 * @return UNIFIED_OK 表示发送成功或未注册 hook；hook 失败返回错误码。
 */
unified_error_t rtos_ipc_send_error_event(const rtos_ipc_event_t *event)
{
    unified_error_t result = UNIFIED_OK;

    if (event == 0) {
        return UNIFIED_ERR_NULL;
    }

    if (g_event_sender != 0) {
        result = g_event_sender(event);
    }

    return complete_rtos_to_linux_send(result);
}
