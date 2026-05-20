/* 公共错误码定义：统一大核 Linux、小核 RTOS 和公共模块的返回值。 */
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
    UNIFIED_OK = 0,

    UNIFIED_ERR_NULL = -1,
    UNIFIED_ERR_INVALID_ARG = -2,
    UNIFIED_ERR_LENGTH = -3,

    UNIFIED_ERR_PROTOCOL_HEADER = -10,
    UNIFIED_ERR_PAYLOAD_LENGTH = -11,
    UNIFIED_ERR_UNKNOWN_TYPE = -12,
    UNIFIED_ERR_CRC = -13,

    UNIFIED_ERR_CAN_DLC = -20,
} unified_error_t;

#ifdef __cplusplus
}
#endif

#endif /* ERROR_CODE_H */
