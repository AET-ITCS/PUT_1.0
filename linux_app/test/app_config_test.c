/**
 * @file app_config_test.c
 * @brief linux_app 配置解析单元测试。
 * @author Yukikaze
 */
#include "app_config.h"

#include <arpa/inet.h>
#include <linux/serial.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                          #condition);                                              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static int test_default_ethernet_config(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    CHECK(config.can_enabled);
    CHECK(strcmp(config.can_ifname, "can0") == 0);
    CHECK(config.can_bitrate == 500000u);
    CHECK(config.can_tx_can_id == 0x321u);
    CHECK(config.can_rx_filter_id == 0x320u);
    CHECK(config.can_rx_filter_mask == 0x7FFu);
    CHECK(!config.can_extended_id);
    CHECK(config.can_reassembly_timeout_ms == 500u);
    CHECK(config.ipc_backend == APP_CONFIG_IPC_BACKEND_HOST);
    CHECK(config.ipc_physical_base == 0u);
    CHECK(config.ipc_region_size == PUT_SHM_REGION_SIZE);
    CHECK(config.ipc_format_on_start);
    CHECK(strcmp(config.ipc_control_device, "") == 0);
    CHECK(config.ethernet_enabled);
    CHECK(config.ethernet_udp_enabled);
    CHECK(config.ethernet_tcp_enabled);
    CHECK(strcmp(config.ethernet_bind_addr, "0.0.0.0") == 0);
    CHECK(config.ethernet_port == 5000u);
    CHECK(strcmp(config.ethernet_tx_peer_addr, "") == 0);
    CHECK(config.ethernet_tx_peer_port == 5000u);
    CHECK(config.wifi_enabled);
    CHECK(config.wifi_udp_enabled);
    CHECK(config.wifi_tcp_enabled);
    CHECK(strcmp(config.wifi_bind_addr, "0.0.0.0") == 0);
    CHECK(config.wifi_port == 5001u);
    CHECK(strcmp(config.wifi_tx_peer_addr, "") == 0);
    CHECK(config.wifi_tx_peer_port == 5001u);
    CHECK(config.wifi_tx_peer_count == 0u);
    CHECK(!config.four_g_enabled);
    CHECK(strcmp(config.four_g_ifname, "usb0") == 0);
    CHECK(config.four_g_bind_to_device);
    CHECK(config.four_g_udp_enabled);
    CHECK(config.four_g_tcp_enabled);
    CHECK(strcmp(config.four_g_bind_addr, "0.0.0.0") == 0);
    CHECK(config.four_g_port == 5002u);
    CHECK(strcmp(config.four_g_tx_peer_addr, "") == 0);
    CHECK(config.four_g_tx_peer_port == 5002u);
    CHECK(config.four_g_tx_peer_count == 0u);
    CHECK(config.rs485_enabled);
    CHECK(strcmp(config.rs485_uart_device, "/dev/ttyS4") == 0);
    CHECK(config.rs485_baudrate == 115200u);
    CHECK(config.rs485_flags == (uint32_t)(SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND));
    CHECK(app_config_validate(&config) == UNIFIED_OK);
    return 0;
}

/**
 * @brief 测试 IPC 板端映射配置解析。
 *
 * @return 0 表示测试通过，1 表示测试失败。
 */
static int test_load_ipc_config(void)
{
    app_config_t config; /* 应用配置对象 */
    char path[128];      /* 临时配置文件路径 */
    FILE *fp;            /* 临时配置文件句柄 */

    (void)snprintf(path, sizeof(path), "/tmp/put_app_config_ipc_test_%ld.ini", (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[ipc]\n", fp);
    fputs("backend = \"devmem\"\n", fp);
    fputs("physical_base = 0x8F000000\n", fp);
    fputs("region_size = 65536\n", fp);
    fputs("format_on_start = false\n", fp);
    fputs("control_device = \"/dev/put_shm_ipc\"\n", fp);
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_OK);
    CHECK(config.ipc_backend == APP_CONFIG_IPC_BACKEND_DEVMEM);
    CHECK(config.ipc_physical_base == (uintptr_t)0x8F000000u);
    CHECK(config.ipc_region_size == PUT_SHM_REGION_SIZE);
    CHECK(!config.ipc_format_on_start);
    CHECK(strcmp(config.ipc_control_device, "/dev/put_shm_ipc") == 0);
    (void)remove(path);
    return 0;
}

static int test_load_can_config(void)
{
    app_config_t config;
    char path[128];
    FILE *fp;

    (void)snprintf(path, sizeof(path), "/tmp/put_app_config_can_test_%ld.ini", (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[can]\n", fp);
    fputs("enabled = true\n", fp);
    fputs("ifname = \"vcan0\"\n", fp);
    fputs("bitrate = 250000\n", fp);
    fputs("tx_can_id = 0x123457\n", fp);
    fputs("rx_filter_id = 0x123456\n", fp);
    fputs("rx_filter_mask = 0x1FFFFFFF\n", fp);
    fputs("extended_id = true\n", fp);
    fputs("reassembly_timeout_ms = 1000\n", fp);
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_OK);
    CHECK(config.can_enabled);
    CHECK(strcmp(config.can_ifname, "vcan0") == 0);
    CHECK(config.can_bitrate == 250000u);
    CHECK(config.can_tx_can_id == 0x123457u);
    CHECK(config.can_rx_filter_id == 0x123456u);
    CHECK(config.can_rx_filter_mask == 0x1FFFFFFFu);
    CHECK(config.can_extended_id);
    CHECK(config.can_reassembly_timeout_ms == 1000u);
    (void)remove(path);
    return 0;
}

static int test_load_rs485_config(void)
{
    app_config_t config;
    char path[128];
    FILE *fp;

    (void)snprintf(path, sizeof(path), "/tmp/put_app_config_rs485_test_%ld.ini", (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[rs485]\n", fp);
    fputs("enabled = true\n", fp);
    fputs("uart_device = \"/dev/ttyUSB0\"\n", fp);
    fputs("baudrate = 57600\n", fp);
    fputs("rs485_flags = 0\n", fp);
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_OK);
    CHECK(config.rs485_enabled);
    CHECK(strcmp(config.rs485_uart_device, "/dev/ttyUSB0") == 0);
    CHECK(config.rs485_baudrate == 57600u);
    CHECK(config.rs485_flags == 0u);
    (void)remove(path);
    return 0;
}

static int test_load_four_g_config(void)
{
    app_config_t config;
    char path[128];
    FILE *fp;
    struct in_addr expected_addr;

    (void)snprintf(path, sizeof(path), "/tmp/put_app_config_four_g_test_%ld.ini", (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[4g]\n", fp);
    fputs("enabled = true\n", fp);
    fputs("ifname = \"usb0\"\n", fp);
    fputs("bind_to_device = true\n", fp);
    fputs("udp_enabled = true\n", fp);
    fputs("tcp_enabled = false\n", fp);
    fputs("bind_addr = \"127.0.0.3\"\n", fp);
    fputs("port = 6002\n", fp);
    fputs("tx_peer_addr = \"127.0.0.12\"\n", fp);
    fputs("tx_peer_port = 7002\n", fp);
    fputs("tx_peer = \"0xA1000001,192.168.10.100,5002\"\n", fp);
    fputs("tx_peer = \"0xA2000002,127.0.0.1,0\"\n", fp);
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_OK);
    CHECK(config.four_g_enabled);
    CHECK(strcmp(config.four_g_ifname, "usb0") == 0);
    CHECK(config.four_g_bind_to_device);
    CHECK(config.four_g_udp_enabled);
    CHECK(!config.four_g_tcp_enabled);
    CHECK(strcmp(config.four_g_bind_addr, "127.0.0.3") == 0);
    CHECK(config.four_g_port == 6002u);
    CHECK(strcmp(config.four_g_tx_peer_addr, "127.0.0.12") == 0);
    CHECK(config.four_g_tx_peer_port == 7002u);
    CHECK(config.four_g_tx_peer_count == 2u);
    CHECK(config.four_g_tx_peers[0].destination_cid[0] == 0xA1u);
    CHECK(config.four_g_tx_peers[0].destination_cid[1] == 0x00u);
    CHECK(config.four_g_tx_peers[0].destination_cid[2] == 0x00u);
    CHECK(config.four_g_tx_peers[0].destination_cid[3] == 0x01u);
    CHECK(config.four_g_tx_peers[0].port == 5002u);
    CHECK(inet_pton(AF_INET, "192.168.10.100", &expected_addr) == 1);
    CHECK(config.four_g_tx_peers[0].ipv4_addr_be == expected_addr.s_addr);
    CHECK(config.four_g_tx_peers[1].destination_cid[0] == 0xA2u);
    CHECK(config.four_g_tx_peers[1].destination_cid[3] == 0x02u);
    CHECK(config.four_g_tx_peers[1].port == 0u);
    (void)remove(path);
    return 0;
}

static int test_load_ethernet_config(void)
{
    app_config_t config;
    char path[128];
    FILE *fp;
    struct in_addr expected_addr;

    (void)snprintf(path, sizeof(path), "/tmp/put_app_config_test_%ld.ini", (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[status]\n", fp);
    fputs("enabled = false\n", fp);
    fputs("dir = \"/tmp/put_status\"\n", fp);
    fputs("[ethernet]\n", fp);
    fputs("enabled = false\n", fp);
    fputs("udp_enabled = false\n", fp);
    fputs("tcp_enabled = true\n", fp);
    fputs("bind_addr = \"127.0.0.1\"\n", fp);
    fputs("port = 6000\n", fp);
    fputs("tx_peer_addr = \"127.0.0.10\"\n", fp);
    fputs("tx_peer_port = 7000\n", fp);
    fputs("[wifi]\n", fp);
    fputs("enabled = true\n", fp);
    fputs("udp_enabled = true\n", fp);
    fputs("tcp_enabled = false\n", fp);
    fputs("bind_addr = \"127.0.0.2\"\n", fp);
    fputs("port = 6001\n", fp);
    fputs("tx_peer_addr = \"127.0.0.11\"\n", fp);
    fputs("tx_peer_port = 7001\n", fp);
    fputs("tx_peer = \"0x61000001,192.168.1.50,5001\"\n", fp);
    fputs("tx_peer = \"0x62000002,127.0.0.1,0\"\n", fp);
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_OK);
    CHECK(!config.status_enabled);
    CHECK(!config.ethernet_enabled);
    CHECK(!config.ethernet_udp_enabled);
    CHECK(config.ethernet_tcp_enabled);
    CHECK(strcmp(config.ethernet_bind_addr, "127.0.0.1") == 0);
    CHECK(config.ethernet_port == 6000u);
    CHECK(strcmp(config.ethernet_tx_peer_addr, "127.0.0.10") == 0);
    CHECK(config.ethernet_tx_peer_port == 7000u);
    CHECK(config.wifi_enabled);
    CHECK(config.wifi_udp_enabled);
    CHECK(!config.wifi_tcp_enabled);
    CHECK(strcmp(config.wifi_bind_addr, "127.0.0.2") == 0);
    CHECK(config.wifi_port == 6001u);
    CHECK(strcmp(config.wifi_tx_peer_addr, "127.0.0.11") == 0);
    CHECK(config.wifi_tx_peer_port == 7001u);
    CHECK(config.wifi_tx_peer_count == 2u);
    CHECK(config.wifi_tx_peers[0].destination_cid[0] == 0x61u);
    CHECK(config.wifi_tx_peers[0].destination_cid[1] == 0x00u);
    CHECK(config.wifi_tx_peers[0].destination_cid[2] == 0x00u);
    CHECK(config.wifi_tx_peers[0].destination_cid[3] == 0x01u);
    CHECK(config.wifi_tx_peers[0].port == 5001u);
    CHECK(inet_pton(AF_INET, "192.168.1.50", &expected_addr) == 1);
    CHECK(config.wifi_tx_peers[0].ipv4_addr_be == expected_addr.s_addr);
    CHECK(config.wifi_tx_peers[1].destination_cid[0] == 0x62u);
    CHECK(config.wifi_tx_peers[1].destination_cid[3] == 0x02u);
    CHECK(config.wifi_tx_peers[1].port == 0u);
    (void)remove(path);
    return 0;
}

static int test_reject_invalid_ethernet_port(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.ethernet_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);
    return 0;
}

static int test_reject_ethernet_with_no_transport(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.ethernet_udp_enabled = false;
    config.ethernet_tcp_enabled = false;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);
    return 0;
}

static int test_reject_invalid_can_config(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.can_enabled = true;
    config.can_rx_filter_id = 0x800u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.can_enabled = true;
    config.can_tx_can_id = 0x800u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.can_enabled = true;
    config.can_reassembly_timeout_ms = 99u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.can_enabled = true;
    config.can_ifname[0] = '\0';
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);
    return 0;
}

/**
 * @brief 测试非法 IPC 映射配置会被拒绝。
 *
 * @return 0 表示测试通过，1 表示测试失败。
 */
static int test_reject_invalid_ipc_config(void)
{
    app_config_t config; /* 应用配置对象 */

    app_config_set_defaults(&config);
    config.ipc_region_size = PUT_SHM_REGION_SIZE + 1u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.ipc_backend = APP_CONFIG_IPC_BACKEND_DEVMEM;
    config.ipc_physical_base = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.ipc_backend = APP_CONFIG_IPC_BACKEND_DEVMEM;
    config.ipc_physical_base = (uintptr_t)(PUT_SHM_CACHE_LINE_SIZE + 1u);
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    (void)snprintf(config.ipc_control_device,
                   sizeof(config.ipc_control_device),
                   "%s",
                   "/dev/put_shm_ipc");
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.ipc_backend = APP_CONFIG_IPC_BACKEND_DEVMEM;
    config.ipc_physical_base = (uintptr_t)0x8F000000u;
    (void)snprintf(config.ipc_control_device,
                   sizeof(config.ipc_control_device),
                   "%s",
                   "relative-control");
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    return 0;
}

static int test_reject_invalid_rs485_config(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.rs485_enabled = true;
    config.rs485_uart_device[0] = '\0';
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.rs485_enabled = true;
    config.rs485_baudrate = 12345u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.rs485_enabled = true;
    config.rs485_flags = (uint32_t)SER_RS485_RTS_ON_SEND;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);
    return 0;
}

static int test_reject_wifi_with_no_transport(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.wifi_enabled = true;
    config.wifi_udp_enabled = false;
    config.wifi_tcp_enabled = false;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);
    return 0;
}

static int test_reject_four_g_with_no_transport(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.four_g_enabled = true;
    config.four_g_udp_enabled = false;
    config.four_g_tcp_enabled = false;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.four_g_enabled = true;
    config.four_g_ifname[0] = '\0';
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.four_g_enabled = true;
    config.four_g_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);
    return 0;
}

static int test_reject_invalid_tx_peer(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.ethernet_enabled = true;
    (void)snprintf(config.ethernet_tx_peer_addr,
                   sizeof(config.ethernet_tx_peer_addr),
                   "%s",
                   "not-an-ip");
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.ethernet_enabled = true;
    (void)snprintf(config.ethernet_tx_peer_addr,
                   sizeof(config.ethernet_tx_peer_addr),
                   "%s",
                   "127.0.0.1");
    config.ethernet_tx_peer_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.wifi_enabled = true;
    (void)snprintf(config.wifi_tx_peer_addr,
                   sizeof(config.wifi_tx_peer_addr),
                   "%s",
                   "not-an-ip");
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.wifi_enabled = true;
    (void)snprintf(config.wifi_tx_peer_addr,
                   sizeof(config.wifi_tx_peer_addr),
                   "%s",
                   "127.0.0.1");
    config.wifi_tx_peer_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.four_g_enabled = true;
    (void)snprintf(config.four_g_tx_peer_addr,
                   sizeof(config.four_g_tx_peer_addr),
                   "%s",
                   "not-an-ip");
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.four_g_enabled = true;
    (void)snprintf(config.four_g_tx_peer_addr,
                   sizeof(config.four_g_tx_peer_addr),
                   "%s",
                   "127.0.0.1");
    config.four_g_tx_peer_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);
    return 0;
}

static int expect_invalid_wifi_peer_line(const char *line)
{
    app_config_t config;
    char path[128];
    FILE *fp;

    (void)snprintf(path,
                   sizeof(path),
                   "/tmp/put_app_config_bad_wifi_peer_%ld.ini",
                   (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[wifi]\n", fp);
    fputs(line, fp);
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_ERR_INVALID_ARG);
    (void)remove(path);
    return 0;
}

static int test_reject_invalid_wifi_tx_peer(void)
{
    app_config_t config;
    char path[128];
    FILE *fp;

    CHECK(expect_invalid_wifi_peer_line("tx_peer = \"0x20000001,127.0.0.1,5001\"\n") == 0);
    CHECK(expect_invalid_wifi_peer_line("tx_peer = \"0x61000001,not-an-ip,5001\"\n") == 0);
    CHECK(expect_invalid_wifi_peer_line("tx_peer = \"0x61000001,127.0.0.1,65536\"\n") == 0);

    (void)snprintf(path,
                   sizeof(path),
                   "/tmp/put_app_config_many_wifi_peer_%ld.ini",
                   (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[wifi]\n", fp);
    for (uint32_t i = 0u; i <= WIFI_TX_PEER_MAX; ++i) {
        fprintf(fp, "tx_peer = \"0x%02X0000%02X,127.0.0.1,5001\"\n",
                (unsigned)(0x60u + (i & 0x0Fu)),
                (unsigned)i);
    }
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_ERR_INVALID_ARG);
    (void)remove(path);
    return 0;
}

static int expect_invalid_four_g_peer_line(const char *line)
{
    app_config_t config;
    char path[128];
    FILE *fp;

    (void)snprintf(path,
                   sizeof(path),
                   "/tmp/put_app_config_bad_four_g_peer_%ld.ini",
                   (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[4g]\n", fp);
    fputs(line, fp);
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_ERR_INVALID_ARG);
    (void)remove(path);
    return 0;
}

static int test_reject_invalid_four_g_tx_peer(void)
{
    app_config_t config;
    char path[128];
    FILE *fp;

    CHECK(expect_invalid_four_g_peer_line("tx_peer = \"0x20000001,127.0.0.1,5002\"\n") == 0);
    CHECK(expect_invalid_four_g_peer_line("tx_peer = \"0xA1000001,not-an-ip,5002\"\n") == 0);
    CHECK(expect_invalid_four_g_peer_line("tx_peer = \"0xA1000001,127.0.0.1,65536\"\n") == 0);

    (void)snprintf(path,
                   sizeof(path),
                   "/tmp/put_app_config_many_four_g_peer_%ld.ini",
                   (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[4g]\n", fp);
    for (uint32_t i = 0u; i <= FOUR_G_TX_PEER_MAX; ++i) {
        fprintf(fp, "tx_peer = \"0x%02X0000%02X,127.0.0.1,5002\"\n",
                (unsigned)(0xA0u + (i & 0x0Fu)),
                (unsigned)i);
    }
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_ERR_INVALID_ARG);
    (void)remove(path);
    return 0;
}

static int test_ignore_tx_peer_when_interface_disabled(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.ethernet_enabled = false;
    (void)snprintf(config.ethernet_tx_peer_addr,
                   sizeof(config.ethernet_tx_peer_addr),
                   "%s",
                   "not-an-ip");
    config.ethernet_tx_peer_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_OK);

    app_config_set_defaults(&config);
    config.wifi_enabled = false;
    (void)snprintf(config.wifi_tx_peer_addr,
                   sizeof(config.wifi_tx_peer_addr),
                   "%s",
                   "not-an-ip");
    config.wifi_tx_peer_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_OK);

    app_config_set_defaults(&config);
    config.four_g_enabled = false;
    (void)snprintf(config.four_g_tx_peer_addr,
                   sizeof(config.four_g_tx_peer_addr),
                   "%s",
                   "not-an-ip");
    config.four_g_tx_peer_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_OK);
    return 0;
}

static int test_ignore_invalid_rs485_fields_when_disabled(void)
{
    app_config_t config;
    char path[128];
    FILE *fp;

    app_config_set_defaults(&config);
    config.rs485_enabled = false;
    config.rs485_uart_device[0] = '\0';
    config.rs485_baudrate = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_OK);

    (void)snprintf(path,
                   sizeof(path),
                   "/tmp/put_app_config_disabled_rs485_%ld.ini",
                   (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[rs485]\n", fp);
    fputs("enabled = false\n", fp);
    fputs("uart_device = \"\"\n", fp);
    fputs("baudrate = 0\n", fp);
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_OK);
    CHECK(!config.rs485_enabled);
    CHECK(strcmp(config.rs485_uart_device, "") == 0);
    CHECK(config.rs485_baudrate == 0u);
    (void)remove(path);
    return 0;
}

int main(void)
{
    if (test_default_ethernet_config() != 0) {
        return 1;
    }
    if (test_load_can_config() != 0) {
        return 1;
    }
    if (test_load_ipc_config() != 0) {
        return 1;
    }
    if (test_load_rs485_config() != 0) {
        return 1;
    }
    if (test_load_four_g_config() != 0) {
        return 1;
    }
    if (test_load_ethernet_config() != 0) {
        return 1;
    }
    if (test_reject_invalid_ethernet_port() != 0) {
        return 1;
    }
    if (test_reject_ethernet_with_no_transport() != 0) {
        return 1;
    }
    if (test_reject_invalid_can_config() != 0) {
        return 1;
    }
    if (test_reject_invalid_ipc_config() != 0) {
        return 1;
    }
    if (test_reject_invalid_rs485_config() != 0) {
        return 1;
    }
    if (test_reject_wifi_with_no_transport() != 0) {
        return 1;
    }
    if (test_reject_four_g_with_no_transport() != 0) {
        return 1;
    }
    if (test_reject_invalid_tx_peer() != 0) {
        return 1;
    }
    if (test_reject_invalid_wifi_tx_peer() != 0) {
        return 1;
    }
    if (test_reject_invalid_four_g_tx_peer() != 0) {
        return 1;
    }
    if (test_ignore_tx_peer_when_interface_disabled() != 0) {
        return 1;
    }
    if (test_ignore_invalid_rs485_fields_when_disabled() != 0) {
        return 1;
    }
    puts("app_config_test: OK");
    return 0;
}
