/**
 * @file error_code.h
 * @brief 公共错误码定义，统一大核 Linux、小核 RTOS 和公共模块的返回值。
 * @author Yukikaze
 */
#ifndef ERROR_CODE_H
#define ERROR_CODE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 公共错误码。
 *
 * 约定：
 * - 0 表示成功；
 * - 负数表示错误；
 * - 大核 Linux、小核 RTOS、公共工具代码共用同一套错误码。
 */
typedef enum {
    UNIFIED_OK = 0,                       /**< 成功。 */

    UNIFIED_ERR_NULL = -1,                /**< 空指针。 */
    UNIFIED_ERR_INVALID_ARG = -2,         /**< 参数非法。 */
    UNIFIED_ERR_LENGTH = -3,              /**< 长度非法。 */

    UNIFIED_ERR_PROTOCOL_HEADER = -10,    /**< 协议头非法。 */
    UNIFIED_ERR_PAYLOAD_LENGTH = -11,     /**< payload 长度非法。 */
    UNIFIED_ERR_UNKNOWN_TYPE = -12,       /**< 未知类型。 */
    UNIFIED_ERR_CRC = -13,                /**< CRC 校验失败。 */

    UNIFIED_ERR_CAN_DLC = -20,            /**< CAN DLC 越界。 */

    UNIFIED_ERR_IPC_QUEUE_EMPTY = -30,    /**< 共享内存 IPC 队列为空。 */
    UNIFIED_ERR_IPC_QUEUE_FULL = -31,     /**< 共享内存 IPC 队列已满。 */
    UNIFIED_ERR_IPC_NOT_READY = -32,      /**< 共享内存 IPC 尚未初始化或未就绪。 */
    UNIFIED_ERR_IPC_NOTIFY_FAILED = -33,  /**< 共享内存 IPC doorbell/mailbox 通知失败。 */
    UNIFIED_ERR_IPC_OFFLINE = -34,        /**< 对端离线或 fail-safe 状态下拒绝业务帧。 */
} unified_error_t;

#ifdef __cplusplus
}
#endif

#endif /* ERROR_CODE_H */
