/**
 * @file rtos_bsp.h
 * @brief rtos_firmware BSP 占位接口。
 * @author Yukikaze
 */
#ifndef RTOS_BSP_H
#define RTOS_BSP_H

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 BSP 骨架。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_init(void);

/**
 * @brief 初始化 board 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_board_init(void);

/**
 * @brief 初始化 clock 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_clock_init(void);

/**
 * @brief 初始化 pinmux 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_pinmux_init(void);

/**
 * @brief 初始化 interrupt 占位模块。
 *
 * @return UNIFIED_OK 表示初始化成功，否则返回公共错误码。
 */
unified_error_t rtos_bsp_interrupt_init(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_BSP_H */
