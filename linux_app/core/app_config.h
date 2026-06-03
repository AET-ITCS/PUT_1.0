/**
 * @file app_config.h
 * @brief linux_app 配置解析接口。
 * @author Yukikaze
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_code.h"
#include "four_g_adapter.h"
#include "linux_shm_platform.h"
#include "wifi_adapter.h"

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
#define APP_CONFIG_ETHERNET_DEFAULT_TX_PEER_ADDR ""
#define APP_CONFIG_ETHERNET_DEFAULT_TX_PEER_PORT APP_CONFIG_ETHERNET_DEFAULT_PORT
#define APP_CONFIG_CAN_IFNAME_MAX 32u
#define APP_CONFIG_CAN_DEFAULT_IFNAME "can0"
#define APP_CONFIG_CAN_DEFAULT_BITRATE 500000u
#define APP_CONFIG_CAN_DEFAULT_TX_CAN_ID 0x321u
#define APP_CONFIG_CAN_DEFAULT_RX_FILTER_ID 0x320u
#define APP_CONFIG_CAN_DEFAULT_RX_FILTER_MASK 0x7FFu
#define APP_CONFIG_CAN_DEFAULT_REASSEMBLY_TIMEOUT_MS 500u
#define APP_CONFIG_WIFI_BIND_ADDR_MAX 64u
#define APP_CONFIG_WIFI_DEFAULT_BIND_ADDR "0.0.0.0"
#define APP_CONFIG_WIFI_DEFAULT_PORT 5001u
#define APP_CONFIG_WIFI_DEFAULT_UDP_ENABLED true
#define APP_CONFIG_WIFI_DEFAULT_TCP_ENABLED true
#define APP_CONFIG_WIFI_DEFAULT_TX_PEER_ADDR ""
#define APP_CONFIG_WIFI_DEFAULT_TX_PEER_PORT APP_CONFIG_WIFI_DEFAULT_PORT
#define APP_CONFIG_FOUR_G_IFNAME_MAX FOUR_G_ADAPTER_IFNAME_MAX
#define APP_CONFIG_FOUR_G_DEFAULT_IFNAME FOUR_G_ADAPTER_DEFAULT_IFNAME
#define APP_CONFIG_FOUR_G_BIND_ADDR_MAX 64u
#define APP_CONFIG_FOUR_G_DEFAULT_BIND_ADDR FOUR_G_ADAPTER_DEFAULT_BIND_ADDR
#define APP_CONFIG_FOUR_G_DEFAULT_PORT FOUR_G_ADAPTER_DEFAULT_PORT
#define APP_CONFIG_FOUR_G_DEFAULT_UDP_ENABLED true
#define APP_CONFIG_FOUR_G_DEFAULT_TCP_ENABLED true
#define APP_CONFIG_FOUR_G_DEFAULT_BIND_TO_DEVICE true
#define APP_CONFIG_FOUR_G_DEFAULT_TX_PEER_ADDR ""
#define APP_CONFIG_FOUR_G_DEFAULT_TX_PEER_PORT APP_CONFIG_FOUR_G_DEFAULT_PORT
#define APP_CONFIG_RS485_UART_DEVICE_MAX 64u
#define APP_CONFIG_RS485_DEFAULT_UART_DEVICE "/dev/ttyS4"
#define APP_CONFIG_RS485_DEFAULT_BAUDRATE 115200u
#define APP_CONFIG_IPC_CONTROL_DEVICE_MAX LINUX_SHM_CONTROL_DEVICE_PATH_MAX

typedef enum {
    APP_CONFIG_IPC_BACKEND_HOST = 0u,   /**< host/mock 后端，使用进程内对齐内存。 */
    APP_CONFIG_IPC_BACKEND_DEVMEM = 1u, /**< 板端 /dev/mem 后端，映射 reserved-memory。 */
} app_config_ipc_backend_t;

typedef struct {
    bool status_enabled;
    char status_dir[APP_CONFIG_PATH_MAX];

    app_config_ipc_backend_t ipc_backend; /**< 共享内存 IPC 映射后端。 */
    uintptr_t ipc_physical_base;          /**< reserved-memory 物理基地址。 */
    uint32_t ipc_region_size;             /**< 共享内存 region 字节数。 */
    bool ipc_format_on_start;             /**< 启动时是否由 Linux 格式化 region。 */
    char ipc_control_device[APP_CONFIG_IPC_CONTROL_DEVICE_MAX]; /**< cache/doorbell control 设备路径。 */

    bool can_enabled;
    char can_ifname[APP_CONFIG_CAN_IFNAME_MAX];
    uint32_t can_bitrate;
    uint32_t can_tx_can_id;
    uint32_t can_rx_filter_id;
    uint32_t can_rx_filter_mask;
    bool can_extended_id;
    uint32_t can_reassembly_timeout_ms;

    bool ethernet_enabled;
    bool ethernet_udp_enabled;
    bool ethernet_tcp_enabled;
    char ethernet_bind_addr[APP_CONFIG_ETHERNET_BIND_ADDR_MAX];
    uint16_t ethernet_port;
    char ethernet_tx_peer_addr[APP_CONFIG_ETHERNET_BIND_ADDR_MAX];
    uint16_t ethernet_tx_peer_port;

    bool wifi_enabled;
    bool wifi_udp_enabled;
    bool wifi_tcp_enabled;
    char wifi_bind_addr[APP_CONFIG_WIFI_BIND_ADDR_MAX];
    uint16_t wifi_port;
    char wifi_tx_peer_addr[APP_CONFIG_WIFI_BIND_ADDR_MAX];
    uint16_t wifi_tx_peer_port;
    size_t wifi_tx_peer_count;
    wifi_tx_peer_t wifi_tx_peers[WIFI_TX_PEER_MAX];

    bool bluetooth_enabled;
    uint8_t bluetooth_channel;

    bool four_g_enabled;
    char four_g_ifname[APP_CONFIG_FOUR_G_IFNAME_MAX];
    bool four_g_bind_to_device;
    bool four_g_udp_enabled;
    bool four_g_tcp_enabled;
    char four_g_bind_addr[APP_CONFIG_FOUR_G_BIND_ADDR_MAX];
    uint16_t four_g_port;
    char four_g_tx_peer_addr[APP_CONFIG_FOUR_G_BIND_ADDR_MAX];
    uint16_t four_g_tx_peer_port;
    size_t four_g_tx_peer_count;
    four_g_tx_peer_t four_g_tx_peers[FOUR_G_TX_PEER_MAX];

    bool rs485_enabled;
    char rs485_uart_device[APP_CONFIG_RS485_UART_DEVICE_MAX];
    uint32_t rs485_baudrate;
    uint32_t rs485_flags;

    uint32_t max_packets;
} app_config_t;

void app_config_set_defaults(app_config_t *config);
unified_error_t app_config_load_file(app_config_t *config, const char *path);
unified_error_t app_config_validate(const app_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
