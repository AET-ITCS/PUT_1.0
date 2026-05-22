/**
 * @file rtos_can.h
 * @brief rtos_firmware CAN 占位接口。
 * @author Yukikaze
 */
#ifndef RTOS_CAN_H
#define RTOS_CAN_H

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 CAN 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_can_init(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_H */
