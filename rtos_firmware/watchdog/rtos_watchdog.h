/**
 * @file rtos_watchdog.h
 * @brief rtos_firmware watchdog 占位接口。
 * @author Yukikaze
 */
#ifndef RTOS_WATCHDOG_H
#define RTOS_WATCHDOG_H

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 watchdog 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_watchdog_init(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_WATCHDOG_H */
