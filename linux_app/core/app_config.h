/* linux_app 配置解析：读取协议入口和状态快照参数。 */
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
#define APP_CONFIG_ETHERNET_BIND_ADDR_MAX 64u
#define APP_CONFIG_ETHERNET_DEFAULT_BIND_ADDR "0.0.0.0"
#define APP_CONFIG_ETHERNET_DEFAULT_PORT 5000u
#define APP_CONFIG_ETHERNET_DEFAULT_UDP_ENABLED true
#define APP_CONFIG_ETHERNET_DEFAULT_TCP_ENABLED true
#define APP_CONFIG_CAN_IFNAME_MAX 32u
#define APP_CONFIG_CAN_DEFAULT_IFNAME "can0"
#define APP_CONFIG_CAN_DEFAULT_BITRATE 500000u
#define APP_CONFIG_CAN_DEFAULT_RX_FILTER_ID 0x320u
#define APP_CONFIG_CAN_DEFAULT_RX_FILTER_MASK 0x7FFu
#define APP_CONFIG_CAN_DEFAULT_REASSEMBLY_TIMEOUT_MS 500u

typedef struct {
    bool status_enabled;
    char status_dir[APP_CONFIG_PATH_MAX];

    bool can_enabled;
    char can_ifname[APP_CONFIG_CAN_IFNAME_MAX];
    uint32_t can_bitrate;
    uint32_t can_rx_filter_id;
    uint32_t can_rx_filter_mask;
    bool can_extended_id;
    uint32_t can_reassembly_timeout_ms;

    bool ethernet_enabled;
    bool ethernet_udp_enabled;
    bool ethernet_tcp_enabled;
    char ethernet_bind_addr[APP_CONFIG_ETHERNET_BIND_ADDR_MAX];
    uint16_t ethernet_port;

    bool bluetooth_enabled;
    uint8_t bluetooth_channel;

    uint32_t max_packets;
} app_config_t;

void app_config_set_defaults(app_config_t *config);
unified_error_t app_config_load_file(app_config_t *config, const char *path);
unified_error_t app_config_validate(const app_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
