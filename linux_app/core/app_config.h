/* linux_app 配置解析：读取多协议入口和状态快照参数。 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONFIG_DEFAULT_PATH "linux_app/config/device_config.ini"
#define APP_CONFIG_PATH_MAX 256u
#define APP_CONFIG_DEV_PATH_MAX 128u

typedef enum {
    APP_RS485_PROTOCOL_DEBUG = 0,
    APP_RS485_PROTOCOL_MODBUS_RTU = 1,
} app_rs485_protocol_t;

typedef struct {
    bool ethernet_udp_enabled;
    uint16_t ethernet_udp_port;

    bool rs485_enabled;
    char rs485_dev[APP_CONFIG_DEV_PATH_MAX];
    uint32_t rs485_baud;
    app_rs485_protocol_t rs485_protocol;
    uint8_t rs485_slave_id;
    bool rs485_response_enabled;

    bool status_enabled;
    char status_dir[APP_CONFIG_PATH_MAX];

    uint32_t max_packets;
} app_config_t;

void app_config_set_defaults(app_config_t *config);
unified_error_t app_config_load_file(app_config_t *config, const char *path);
unified_error_t app_config_validate(const app_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
