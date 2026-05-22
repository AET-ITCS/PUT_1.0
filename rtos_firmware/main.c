/**
 * @file main.c
 * @brief rtos_firmware smoke 入口。
 * @author Yukikaze
 */
#include "rtos_firmware.h"

#include "error_code.h"

/**
 * @brief 固件 smoke 入口。
 *
 * @return 0 表示骨架初始化成功，非 0 表示初始化失败。
 */
int main(void)
{
    unified_error_t init_result; /**< 固件骨架初始化结果。 */

    init_result = rtos_firmware_main();
    if (init_result != UNIFIED_OK) {
        /* 初始化失败时返回非 0，便于 host smoke target 捕获错误。 */
        return 1;
    }

    /* 当前只是骨架入口，真实 RTOS scheduler 后续再接入。 */
    return 0;
}
