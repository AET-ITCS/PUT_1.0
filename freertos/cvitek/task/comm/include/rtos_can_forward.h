/**
 * @file rtos_can_forward.h
 * @brief FreeRTOS 小核 CAN 转发核心接口。
 *
 * 本模块接收协议适配层输出的 @ref rtos_can_message_t，完成 CAN 层校验、
 * 软件 TX 队列管理、发送重试和错误恢复。
 */
#ifndef RTOS_CAN_FORWARD_H
#define RTOS_CAN_FORWARD_H

#include "error_code.h"
#include "rtos_can_message.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 comm 转发链路。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t gateway_forward_init(void);

/**
 * @brief 提交并同步 drain 一帧 CAN TX 消息。
 * @param message 待转发的 CAN 消息。
 * @return UNIFIED_OK 表示成功入队；发送错误通过状态统计体现。
 */
unified_error_t rtos_can_forward_submit_message(const rtos_can_message_t *message);

/**
 * @brief 只将 CAN TX 消息放入软件队列。
 * @param message 待入队的 CAN 消息。
 * @return UNIFIED_OK 表示成功入队，否则返回公共错误码。
 */
unified_error_t rtos_can_forward_enqueue_message(const rtos_can_message_t *message);

/**
 * @brief 非阻塞 drain 当前 CAN TX 队列。
 * @return 本次从软件队列取出的消息数量。
 */
uint32_t rtos_can_forward_drain_tx_queue_once(void);

/**
 * @brief 清空 CAN TX 软件队列。
 * @return 被清空的消息数量。
 */
uint32_t rtos_can_forward_purge_tx_queue(void);

/**
 * @brief 获取 CAN TX 软件队列当前深度。
 * @return 队列中未消费的消息数量。
 */
uint32_t rtos_can_forward_get_tx_queue_depth(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_FORWARD_H */
