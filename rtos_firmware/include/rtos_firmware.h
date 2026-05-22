/**
 * @file rtos_firmware.h
 * @brief rtos_firmware 顶层入口接口。
 * @author Yukikaze
 */
#ifndef RTOS_FIRMWARE_H
#define RTOS_FIRMWARE_H

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 rtos_firmware 骨架模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_firmware_main(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_FIRMWARE_H */
