/**
 * @file main.c
 * @brief 大核 Linux v2 主线常驻服务入口。
 * @author Yukikaze
 */
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app_config.h"
#include "bluetooth_adapter.h"
#include "can_adapter.h"
#include "egress_manager.h"
#include "ethernet_adapter.h"
#include "four_g_adapter.h"
#include "linux_shm_ipc.h"
#include "rs485_adapter.h"
#include "status_collector.h"
#include "wifi_adapter.h"

static volatile sig_atomic_t g_should_stop = 0;

static void handle_signal(int signo)
{
    (void)signo;
    g_should_stop = 1;
}

static void print_usage(const char *program)
{
    printf("Usage: %s [--config PATH] [--status-dir DIR] [--disable-status]\n", program);
    printf("  --config PATH      INI config path, default linux_app/config/device_config.ini if present\n");
    printf("  --status-dir DIR   status snapshot directory, default /run/put/status\n");
    printf("  --disable-status   do not write status snapshot files\n");
}

static int find_config_arg(int argc, char **argv, const char **out_path, bool *out_explicit)
{
    *out_path = APP_CONFIG_DEFAULT_PATH;
    *out_explicit = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0) {
            if ((i + 1) >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, "invalid --config\n");
                return -1;
            }
            *out_path = argv[++i];
            *out_explicit = true;
        }
    }

    return 0;
}

static int apply_cli_overrides(int argc, char **argv, app_config_t *config)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            return 1;
        }

        if (strcmp(argv[i], "--config") == 0) {
            ++i;
            continue;
        }

        if (strcmp(argv[i], "--status-dir") == 0) {
            if ((i + 1) >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, "invalid --status-dir\n");
                return -1;
            }
            (void)snprintf(config->status_dir, sizeof(config->status_dir), "%s", argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--disable-status") == 0) {
            config->status_enabled = false;
            continue;
        }

        fprintf(stderr, "unknown argument: %s\n", argv[i]);
        return -1;
    }

    return 0;
}

static void configure_status_modules(status_collector_t *collector, const app_config_t *config)
{
    for (int i = 0; i < (int)STATUS_MODULE_COUNT; ++i) {
        status_collector_configure_module(collector,
                                          (status_module_id_t)i,
                                          false,
                                          false,
                                          status_module_display_protocol((status_module_id_t)i),
                                          "planned adapter; not started in current build");
    }

    status_collector_configure_module(collector,
                                      STATUS_MODULE_CAN,
                                      true,
                                      config->can_enabled,
                                      "SocketCAN classic RX",
                                      "Classic CAN private fragments reassemble complete anyMSG into Linux SHM v2");

    status_collector_configure_module(collector,
                                      STATUS_MODULE_ETHERNET,
                                      true,
                                      config->ethernet_enabled,
                                      "Ethernet UDP/TCP raw",
                                      "UDP datagram or TCP stream carries complete anyMSG; RX to Linux SHM v2");
    status_collector_configure_module(collector,
                                      STATUS_MODULE_WIFI,
                                      true,
                                      config->wifi_enabled,
                                      "Wi-Fi UDP/TCP raw",
                                      "Wi-Fi UDP datagram or TCP stream carries complete anyMSG; RX to Linux SHM v2");
    status_collector_configure_module(collector,
                                      STATUS_MODULE_BLUETOOTH,
                                      true,
                                      config->bluetooth_enabled,
                                      "Bluetooth RFCOMM",
                                      "Bluetooth RFCOMM channel carries complete anyMSG; RX to Linux SHM v2");
    status_collector_configure_module(collector,
                                      STATUS_MODULE_4G,
                                      true,
                                      config->four_g_enabled,
                                      "4G EC20 USB UDP/TCP raw",
                                      "EC20 USB network interface carries complete anyMSG; RX to Linux SHM v2");
    status_collector_configure_module(collector,
                                      STATUS_MODULE_RS485,
                                      true,
                                      config->rs485_enabled,
                                      "RS485 raw",
                                      "RS485 UART raw payload bridge; RX to Linux SHM v2");
}

static uint32_t make_linux_epoch(void)
{
    time_t now = time(NULL);

    if (now <= 0) {
        return 1u;
    }
    return (uint32_t)now;
}

/**
 * @brief 根据配置选择共享内存 IPC 平台后端。
 *
 * @param config 应用配置。
 * @param control control ioctl 上下文。
 * @param out_ops_storage 输出自定义平台操作集合。
 * @param out_ops 输出平台操作集合，NULL 表示使用 IPC 库默认 host 后端。
 * @return UNIFIED_OK 表示选择成功，否则返回公共错误码。
 */
static unified_error_t select_ipc_platform_ops(const app_config_t *config,
                                               linux_shm_platform_control_t *control,
                                               linux_shm_platform_ops_t *out_ops_storage,
                                               const linux_shm_platform_ops_t **out_ops)
{
    unified_error_t result; /**< 平台操作选择结果。 */

    if ((config == NULL) || (out_ops == NULL)) {
        /* 配置和输出指针是选择平台后端的必要输入。 */
        return UNIFIED_ERR_NULL;
    }

    *out_ops = NULL;
    if (config->ipc_backend != APP_CONFIG_IPC_BACKEND_DEVMEM) {
        /* 默认保持 host/mock 后端，方便本机运行和测试。 */
        return UNIFIED_OK;
    }

    if (config->ipc_control_device[0] == '\0') {
        /* 未配置 control 设备时，只启用 /dev/mem 映射后端。 */
        *out_ops = linux_shm_platform_devmem_ops();
        return UNIFIED_OK;
    }

    if ((control == NULL) || (out_ops_storage == NULL)) {
        /* control 后端需要 caller 提供持久上下文和 ops 存储。 */
        return UNIFIED_ERR_NULL;
    }

    linux_shm_platform_control_init(control);
    result = linux_shm_platform_control_configure(control, config->ipc_control_device);
    if (result != UNIFIED_OK) {
        /* control 设备路径非法时不能启用真实 cache/doorbell ops。 */
        return result;
    }

    result = linux_shm_platform_make_devmem_control_ops(out_ops_storage, control);
    if (result != UNIFIED_OK) {
        /* 构造 ops 失败说明 control 上下文仍未就绪。 */
        return result;
    }

    *out_ops = out_ops_storage;
    return UNIFIED_OK;
}

/**
 * @brief 获取 IPC 后端名称。
 *
 * @param backend IPC 后端枚举。
 * @return 后端名称字符串。
 */
static const char *ipc_backend_name(app_config_ipc_backend_t backend)
{
    if (backend == APP_CONFIG_IPC_BACKEND_DEVMEM) {
        /* devmem 表示板端 /dev/mem 后端。 */
        return "devmem";
    }

    /* 未知值也按 host 展示，避免错误路径打印空字符串。 */
    return "host";
}

static void sleep_one_second(void)
{
    struct timespec req;
    struct timespec rem;

    req.tv_sec = 1;
    req.tv_nsec = 0;
    while ((nanosleep(&req, &rem) != 0) && !g_should_stop) {
        req = rem;
    }
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    if ((sigemptyset(&action.sa_mask) != 0) ||
        (sigaction(SIGINT, &action, NULL) != 0) ||
        (sigaction(SIGTERM, &action, NULL) != 0)) {
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    app_config_t config;
    const char *config_path;
    bool config_explicit;
    int cli_rc;
    unified_error_t err;
    linux_shm_ipc_t ipc;
    status_collector_t collector;
    egress_manager_t egress_manager;
    egress_manager_config_t egress_config;
    can_tx_context_t can_tx_context;
    ethernet_tx_context_t ethernet_tx_context;
    wifi_tx_context_t wifi_tx_context;
    four_g_tx_context_t four_g_tx_context;
    uint32_t linux_epoch;
    const linux_shm_platform_ops_t *ipc_ops; /* IPC 平台后端操作集合 */
    linux_shm_platform_ops_t ipc_ops_storage; /* IPC 自定义平台后端存储 */
    linux_shm_platform_control_t ipc_control; /* IPC control ioctl 上下文 */
    bool egress_started = false;
    bool can_started = false;
    bool ethernet_udp_started = false;
    bool ethernet_tcp_started = false;
    bool wifi_udp_started = false;
    bool wifi_tcp_started = false;
    bool four_g_udp_started = false;
    bool four_g_tcp_started = false;
    bool bluetooth_started = false;
    bool rs485_started = false;
    int exit_code = EXIT_SUCCESS;

    if (find_config_arg(argc, argv, &config_path, &config_explicit) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    app_config_set_defaults(&config);
    if (config_explicit || (access(config_path, R_OK) == 0)) {
        err = app_config_load_file(&config, config_path);
        if (err != UNIFIED_OK) {
            fprintf(stderr, "failed to load config: %s\n", config_path);
            return EXIT_FAILURE;
        }
    }

    cli_rc = apply_cli_overrides(argc, argv, &config);
    if (cli_rc > 0) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (cli_rc < 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (app_config_validate(&config) != UNIFIED_OK) {
        fprintf(stderr, "invalid linux_app config\n");
        return EXIT_FAILURE;
    }

    if (install_signal_handlers() != 0) {
        fprintf(stderr, "failed to install signal handlers\n");
        return EXIT_FAILURE;
    }

    status_collector_init(&collector, config.status_dir, config.status_enabled);
    configure_status_modules(&collector, &config);
    egress_manager_init(&egress_manager);
    memset(&egress_config, 0, sizeof(egress_config));
    can_tx_context_init(&can_tx_context, config.can_tx_can_id, config.can_extended_id);
    ethernet_tx_context_init(&ethernet_tx_context, config.ethernet_port);
    wifi_tx_context_init(&wifi_tx_context, config.wifi_port);
    four_g_tx_context_init(&four_g_tx_context, config.four_g_port);
    err = four_g_tx_context_set_interface(&four_g_tx_context,
                                          config.four_g_ifname,
                                          config.four_g_bind_to_device);
    if (err != UNIFIED_OK) {
        fprintf(stderr, "invalid 4G interface config\n");
        egress_manager_destroy(&egress_manager);
        ethernet_tx_context_destroy(&ethernet_tx_context);
        wifi_tx_context_destroy(&wifi_tx_context);
        four_g_tx_context_destroy(&four_g_tx_context);
        status_collector_destroy(&collector);
        return EXIT_FAILURE;
    }
    for (size_t i = 0u; i < config.wifi_tx_peer_count; ++i) {
        if (wifi_tx_context_add_peer(&wifi_tx_context, &config.wifi_tx_peers[i]) != 0) {
            fprintf(stderr, "invalid wifi tx peer at index %zu\n", i);
            egress_manager_destroy(&egress_manager);
            ethernet_tx_context_destroy(&ethernet_tx_context);
            wifi_tx_context_destroy(&wifi_tx_context);
            four_g_tx_context_destroy(&four_g_tx_context);
            status_collector_destroy(&collector);
            return EXIT_FAILURE;
        }
    }
    for (size_t i = 0u; i < config.four_g_tx_peer_count; ++i) {
        if (four_g_tx_context_add_peer(&four_g_tx_context, &config.four_g_tx_peers[i]) != 0) {
            fprintf(stderr, "invalid 4G tx peer at index %zu\n", i);
            egress_manager_destroy(&egress_manager);
            ethernet_tx_context_destroy(&ethernet_tx_context);
            wifi_tx_context_destroy(&wifi_tx_context);
            four_g_tx_context_destroy(&four_g_tx_context);
            status_collector_destroy(&collector);
            return EXIT_FAILURE;
        }
    }

    if (config.ethernet_tx_peer_addr[0] != '\0') {
        err = ethernet_tx_context_set_default_peer(&ethernet_tx_context,
                                                   config.ethernet_tx_peer_addr,
                                                   config.ethernet_tx_peer_port);
        if (err != UNIFIED_OK) {
            fprintf(stderr, "invalid ethernet tx peer config\n");
            status_collector_record_error(&collector,
                                          STATUS_MODULE_ETHERNET,
                                          "ethernet_peer_config",
                                          err);
            (void)status_collector_write_all(&collector);
            egress_manager_destroy(&egress_manager);
            ethernet_tx_context_destroy(&ethernet_tx_context);
            wifi_tx_context_destroy(&wifi_tx_context);
            four_g_tx_context_destroy(&four_g_tx_context);
            status_collector_destroy(&collector);
            return EXIT_FAILURE;
        }
    }
    if (config.wifi_tx_peer_addr[0] != '\0') {
        err = wifi_tx_context_set_default_peer(&wifi_tx_context,
                                               config.wifi_tx_peer_addr,
                                               config.wifi_tx_peer_port);
        if (err != UNIFIED_OK) {
            fprintf(stderr, "invalid wifi tx peer config\n");
            status_collector_record_error(&collector,
                                          STATUS_MODULE_WIFI,
                                          "wifi_peer_config",
                                          err);
            (void)status_collector_write_all(&collector);
            egress_manager_destroy(&egress_manager);
            ethernet_tx_context_destroy(&ethernet_tx_context);
            wifi_tx_context_destroy(&wifi_tx_context);
            four_g_tx_context_destroy(&four_g_tx_context);
            status_collector_destroy(&collector);
            return EXIT_FAILURE;
        }
    }
    if (config.four_g_tx_peer_addr[0] != '\0') {
        err = four_g_tx_context_set_default_peer(&four_g_tx_context,
                                                 config.four_g_tx_peer_addr,
                                                 config.four_g_tx_peer_port);
        if (err != UNIFIED_OK) {
            fprintf(stderr, "invalid 4G tx peer config\n");
            status_collector_record_error(&collector,
                                          STATUS_MODULE_4G,
                                          "four_g_peer_config",
                                          err);
            (void)status_collector_write_all(&collector);
            egress_manager_destroy(&egress_manager);
            ethernet_tx_context_destroy(&ethernet_tx_context);
            wifi_tx_context_destroy(&wifi_tx_context);
            four_g_tx_context_destroy(&four_g_tx_context);
            status_collector_destroy(&collector);
            return EXIT_FAILURE;
        }
    }

    linux_epoch = make_linux_epoch();
    memset(&ipc_ops_storage, 0, sizeof(ipc_ops_storage));
    linux_shm_platform_control_init(&ipc_control);
    err = select_ipc_platform_ops(&config, &ipc_control, &ipc_ops_storage, &ipc_ops);
    if (err != UNIFIED_OK) {
        fprintf(stderr,
                "failed to configure linux shm ipc platform ops: backend=%s control_device=%s error=%d\n",
                ipc_backend_name(config.ipc_backend),
                config.ipc_control_device,
                (int)err);
        status_collector_record_error(&collector,
                                      STATUS_MODULE_ETHERNET,
                                      "ipc_platform",
                                      err);
        (void)status_collector_write_all(&collector);
        egress_manager_destroy(&egress_manager);
        ethernet_tx_context_destroy(&ethernet_tx_context);
        wifi_tx_context_destroy(&wifi_tx_context);
        four_g_tx_context_destroy(&four_g_tx_context);
        status_collector_destroy(&collector);
        return EXIT_FAILURE;
    }
    /* 按配置映射共享内存；host 后端忽略物理地址，devmem 后端使用 DTS 地址。 */
    err = linux_shm_ipc_map(&ipc,
                            config.ipc_physical_base,
                            config.ipc_region_size,
                            ipc_ops);
    if (err != UNIFIED_OK) {
        fprintf(stderr,
                "failed to map linux shm ipc region: backend=%s physical_base=0x%llX size=%u error=%d\n",
                ipc_backend_name(config.ipc_backend),
                (unsigned long long)config.ipc_physical_base,
                (unsigned)config.ipc_region_size,
                (int)err);
        status_collector_record_error(&collector,
                                      STATUS_MODULE_ETHERNET,
                                      "ipc_map",
                                      err);
        (void)status_collector_write_all(&collector);
        egress_manager_destroy(&egress_manager);
        ethernet_tx_context_destroy(&ethernet_tx_context);
        wifi_tx_context_destroy(&wifi_tx_context);
        four_g_tx_context_destroy(&four_g_tx_context);
        status_collector_destroy(&collector);
        return EXIT_FAILURE;
    }

    if (config.ipc_format_on_start) {
        /* Linux 作为 Frame Pool owner，默认启动时重建 v2 region 和 epoch。 */
        err = linux_shm_ipc_format_region(&ipc, ipc.region, linux_epoch, 0u, ipc_ops);
    } else {
        /* attach 模式用于调试或外部初始化场景，沿用共享 header 中的 epoch。 */
        err = linux_shm_ipc_attach(&ipc, ipc.region, ipc_ops);
        if (err == UNIFIED_OK) {
            /* descriptor epoch 必须与共享 header 一致，否则 RTOS 会拒绝 frame。 */
            linux_epoch = ipc.region->header.linux_epoch;
        }
    }
    if (err != UNIFIED_OK) {
        fprintf(stderr,
                "failed to %s linux shm ipc region: backend=%s error=%d\n",
                config.ipc_format_on_start ? "format" : "attach",
                ipc_backend_name(config.ipc_backend),
                (int)err);
        status_collector_record_error(&collector,
                                      STATUS_MODULE_ETHERNET,
                                      config.ipc_format_on_start ? "ipc_format" : "ipc_attach",
                                      err);
        (void)status_collector_write_all(&collector);
        linux_shm_ipc_unmap(&ipc);
        egress_manager_destroy(&egress_manager);
        ethernet_tx_context_destroy(&ethernet_tx_context);
        wifi_tx_context_destroy(&wifi_tx_context);
        four_g_tx_context_destroy(&four_g_tx_context);
        status_collector_destroy(&collector);
        return EXIT_FAILURE;
    }
    printf("linux_app: shared memory IPC backend=%s physical_base=0x%llX size=%u control_device=%s format_on_start=%s linux_epoch=%u\n",
           ipc_backend_name(config.ipc_backend),
           (unsigned long long)config.ipc_physical_base,
           (unsigned)config.ipc_region_size,
           config.ipc_control_device,
           config.ipc_format_on_start ? "true" : "false",
           linux_epoch);

    egress_config.ipc = &ipc;
    egress_config.collector = &collector;
    egress_config.linux_epoch = linux_epoch;
    egress_config.worker_budget = EGRESS_WORKER_DEFAULT_BUDGET;
    egress_config.periodic_drain_ms = EGRESS_MANAGER_DEFAULT_PERIODIC_DRAIN_MS;
    egress_config.adapters[PUT_SHM_INTERFACE_CAN] = &can_adapter;
    egress_config.adapter_contexts[PUT_SHM_INTERFACE_CAN] = &can_tx_context;
    egress_config.enabled[PUT_SHM_INTERFACE_CAN] = config.can_enabled;
    egress_config.adapters[PUT_SHM_INTERFACE_ETHERNET] = &ethernet_adapter;
    egress_config.adapter_contexts[PUT_SHM_INTERFACE_ETHERNET] = &ethernet_tx_context;
    egress_config.enabled[PUT_SHM_INTERFACE_ETHERNET] = config.ethernet_enabled;
    egress_config.adapters[PUT_SHM_INTERFACE_WIFI] = &wifi_adapter;
    egress_config.adapter_contexts[PUT_SHM_INTERFACE_WIFI] = &wifi_tx_context;
    egress_config.enabled[PUT_SHM_INTERFACE_WIFI] = config.wifi_enabled;
    egress_config.adapters[PUT_SHM_INTERFACE_BLUETOOTH] = &bluetooth_adapter;
    egress_config.adapter_contexts[PUT_SHM_INTERFACE_BLUETOOTH] = NULL;
    egress_config.enabled[PUT_SHM_INTERFACE_BLUETOOTH] = config.bluetooth_enabled;
    egress_config.adapters[PUT_SHM_INTERFACE_4G] = &four_g_adapter;
    egress_config.adapter_contexts[PUT_SHM_INTERFACE_4G] = &four_g_tx_context;
    egress_config.enabled[PUT_SHM_INTERFACE_4G] = config.four_g_enabled;
    egress_config.adapters[PUT_SHM_INTERFACE_RS485] = &rs485_adapter;
    egress_config.enabled[PUT_SHM_INTERFACE_RS485] = config.rs485_enabled;
    err = egress_manager_start(&egress_manager, &egress_config);
    if (err != UNIFIED_OK) {
        fprintf(stderr, "failed to start linux egress manager: error=%d\n", (int)err);
        status_collector_record_error(&collector,
                                      STATUS_MODULE_ETHERNET,
                                      "egress_start",
                                      err);
        (void)status_collector_write_all(&collector);
        linux_shm_ipc_unmap(&ipc);
        egress_manager_destroy(&egress_manager);
        ethernet_tx_context_destroy(&ethernet_tx_context);
        wifi_tx_context_destroy(&wifi_tx_context);
        four_g_tx_context_destroy(&four_g_tx_context);
        status_collector_destroy(&collector);
        return EXIT_FAILURE;
    }
    egress_started = true;

    if (config.can_enabled) {
        can_adapter_config_t can_config;

        can_adapter_config_set_defaults(&can_config);
        can_config.enabled = true;
        (void)snprintf(can_config.ifname, sizeof(can_config.ifname), "%s", config.can_ifname);
        can_config.bitrate = config.can_bitrate;
        can_config.tx_can_id = config.can_tx_can_id;
        can_config.rx_filter_id = config.can_rx_filter_id;
        can_config.rx_filter_mask = config.can_rx_filter_mask;
        can_config.extended_id = config.can_extended_id;
        can_config.reassembly_timeout_ms = config.can_reassembly_timeout_ms;
        can_config.ipc = &ipc;
        can_config.collector = &collector;
        can_config.linux_epoch = linux_epoch;

        if (can_adapter_start(&can_config) != 0) {
            fprintf(stderr,
                    "failed to start CAN RX service on %s\n",
                    config.can_ifname);
            exit_code = EXIT_FAILURE;
            g_should_stop = 1;
        } else {
            can_started = true;
            printf("linux_app: CAN RX listening on %s, expected externally configured bitrate=%u, tx_can_id=0x%X, linux_epoch=%u\n",
                   config.can_ifname,
                   (unsigned)config.can_bitrate,
                   (unsigned)config.can_tx_can_id,
                   linux_epoch);
        }
    }

    if (!g_should_stop && config.rs485_enabled) {
        rs485_adapter_config_t rs485_config;

        rs485_adapter_config_set_defaults(&rs485_config);
        rs485_config.enabled = true;
        (void)snprintf(rs485_config.uart_device,
                       sizeof(rs485_config.uart_device),
                       "%s",
                       config.rs485_uart_device);
        rs485_config.baudrate = config.rs485_baudrate;
        rs485_config.rs485_flags = config.rs485_flags;
        rs485_config.ipc = &ipc;
        rs485_config.collector = &collector;
        rs485_config.linux_epoch = linux_epoch;

        if (rs485_adapter_start(&rs485_config) != 0) {
            fprintf(stderr,
                    "failed to start RS485 RX service on %s\n",
                    config.rs485_uart_device);
            exit_code = EXIT_FAILURE;
            g_should_stop = 1;
        } else {
            rs485_started = true;
            printf("linux_app: RS485 RX listening on %s, baudrate=%u, flags=0x%X, linux_epoch=%u\n",
                   config.rs485_uart_device,
                   (unsigned)config.rs485_baudrate,
                   (unsigned)config.rs485_flags,
                   linux_epoch);
        }
    }

    if (!g_should_stop && config.ethernet_enabled && config.ethernet_udp_enabled) {
        ethernet_udp_config_t ethernet_udp_config;

        memset(&ethernet_udp_config, 0, sizeof(ethernet_udp_config));
        ethernet_udp_config.enabled = true;
        (void)snprintf(ethernet_udp_config.bind_addr,
                       sizeof(ethernet_udp_config.bind_addr),
                       "%s",
                       config.ethernet_bind_addr);
        ethernet_udp_config.port = config.ethernet_port;
        ethernet_udp_config.ipc = &ipc;
        ethernet_udp_config.collector = &collector;
        ethernet_udp_config.linux_epoch = linux_epoch;

        if (ethernet_udp_server_start(&ethernet_udp_config) != 0) {
            fprintf(stderr,
                    "failed to start ethernet UDP RX service on %s:%u\n",
                    config.ethernet_bind_addr,
                    (unsigned)config.ethernet_port);
            exit_code = EXIT_FAILURE;
            g_should_stop = 1;
        } else {
            ethernet_udp_started = true;
            printf("linux_app: Ethernet UDP RX listening on %s:%u, linux_epoch=%u\n",
                   config.ethernet_bind_addr,
                   (unsigned)config.ethernet_port,
                   linux_epoch);
        }
    }

    if (!g_should_stop && config.ethernet_enabled && config.ethernet_tcp_enabled) {
        ethernet_tcp_config_t ethernet_tcp_config;

        memset(&ethernet_tcp_config, 0, sizeof(ethernet_tcp_config));
        ethernet_tcp_config.enabled = true;
        (void)snprintf(ethernet_tcp_config.bind_addr,
                       sizeof(ethernet_tcp_config.bind_addr),
                       "%s",
                       config.ethernet_bind_addr);
        ethernet_tcp_config.port = config.ethernet_port;
        ethernet_tcp_config.listen_backlog = ETHERNET_ADAPTER_DEFAULT_TCP_BACKLOG;
        ethernet_tcp_config.ipc = &ipc;
        ethernet_tcp_config.collector = &collector;
        ethernet_tcp_config.linux_epoch = linux_epoch;

        if (ethernet_tcp_server_start(&ethernet_tcp_config) != 0) {
            fprintf(stderr,
                    "failed to start ethernet TCP RX service on %s:%u\n",
                    config.ethernet_bind_addr,
                    (unsigned)config.ethernet_port);
            exit_code = EXIT_FAILURE;
            g_should_stop = 1;
        } else {
            ethernet_tcp_started = true;
            printf("linux_app: Ethernet TCP RX listening on %s:%u, linux_epoch=%u\n",
                   config.ethernet_bind_addr,
                   (unsigned)config.ethernet_port,
                   linux_epoch);
        }
    }

    if (!g_should_stop && config.wifi_enabled && config.wifi_udp_enabled) {
        wifi_udp_config_t wifi_udp_config;

        memset(&wifi_udp_config, 0, sizeof(wifi_udp_config));
        wifi_udp_config.enabled = true;
        (void)snprintf(wifi_udp_config.bind_addr,
                       sizeof(wifi_udp_config.bind_addr),
                       "%s",
                       config.wifi_bind_addr);
        wifi_udp_config.port = config.wifi_port;
        wifi_udp_config.ipc = &ipc;
        wifi_udp_config.collector = &collector;
        wifi_udp_config.linux_epoch = linux_epoch;

        if (wifi_udp_server_start(&wifi_udp_config) != 0) {
            fprintf(stderr,
                    "failed to start wifi UDP RX service on %s:%u\n",
                    config.wifi_bind_addr,
                    (unsigned)config.wifi_port);
            exit_code = EXIT_FAILURE;
            g_should_stop = 1;
        } else {
            wifi_udp_started = true;
            printf("linux_app: Wi-Fi UDP RX listening on %s:%u, linux_epoch=%u\n",
                   config.wifi_bind_addr,
                   (unsigned)config.wifi_port,
                   linux_epoch);
        }
    }

    if (!g_should_stop && config.wifi_enabled && config.wifi_tcp_enabled) {
        wifi_tcp_config_t wifi_tcp_config;

        memset(&wifi_tcp_config, 0, sizeof(wifi_tcp_config));
        wifi_tcp_config.enabled = true;
        (void)snprintf(wifi_tcp_config.bind_addr,
                       sizeof(wifi_tcp_config.bind_addr),
                       "%s",
                       config.wifi_bind_addr);
        wifi_tcp_config.port = config.wifi_port;
        wifi_tcp_config.listen_backlog = WIFI_ADAPTER_DEFAULT_TCP_BACKLOG;
        wifi_tcp_config.ipc = &ipc;
        wifi_tcp_config.collector = &collector;
        wifi_tcp_config.linux_epoch = linux_epoch;

        if (wifi_tcp_server_start(&wifi_tcp_config) != 0) {
            fprintf(stderr,
                    "failed to start wifi TCP RX service on %s:%u\n",
                    config.wifi_bind_addr,
                    (unsigned)config.wifi_port);
            exit_code = EXIT_FAILURE;
            g_should_stop = 1;
        } else {
            wifi_tcp_started = true;
            printf("linux_app: Wi-Fi TCP RX listening on %s:%u, linux_epoch=%u\n",
                   config.wifi_bind_addr,
                   (unsigned)config.wifi_port,
                   linux_epoch);
        }
    }

    if (!g_should_stop && config.four_g_enabled && config.four_g_udp_enabled) {
        four_g_udp_config_t four_g_udp_config;

        memset(&four_g_udp_config, 0, sizeof(four_g_udp_config));
        four_g_udp_config.enabled = true;
        (void)snprintf(four_g_udp_config.ifname,
                       sizeof(four_g_udp_config.ifname),
                       "%s",
                       config.four_g_ifname);
        four_g_udp_config.bind_to_device = config.four_g_bind_to_device;
        (void)snprintf(four_g_udp_config.bind_addr,
                       sizeof(four_g_udp_config.bind_addr),
                       "%s",
                       config.four_g_bind_addr);
        four_g_udp_config.port = config.four_g_port;
        four_g_udp_config.ipc = &ipc;
        four_g_udp_config.collector = &collector;
        four_g_udp_config.linux_epoch = linux_epoch;

        if (four_g_udp_server_start(&four_g_udp_config) != 0) {
            fprintf(stderr,
                    "failed to start 4G UDP RX service on %s/%s:%u\n",
                    config.four_g_ifname,
                    config.four_g_bind_addr,
                    (unsigned)config.four_g_port);
            exit_code = EXIT_FAILURE;
            g_should_stop = 1;
        } else {
            four_g_udp_started = true;
            printf("linux_app: 4G UDP RX listening on %s/%s:%u, linux_epoch=%u\n",
                   config.four_g_ifname,
                   config.four_g_bind_addr,
                   (unsigned)config.four_g_port,
                   linux_epoch);
        }
    }

    if (!g_should_stop && config.four_g_enabled && config.four_g_tcp_enabled) {
        four_g_tcp_config_t four_g_tcp_config;

        memset(&four_g_tcp_config, 0, sizeof(four_g_tcp_config));
        four_g_tcp_config.enabled = true;
        (void)snprintf(four_g_tcp_config.ifname,
                       sizeof(four_g_tcp_config.ifname),
                       "%s",
                       config.four_g_ifname);
        four_g_tcp_config.bind_to_device = config.four_g_bind_to_device;
        (void)snprintf(four_g_tcp_config.bind_addr,
                       sizeof(four_g_tcp_config.bind_addr),
                       "%s",
                       config.four_g_bind_addr);
        four_g_tcp_config.port = config.four_g_port;
        four_g_tcp_config.listen_backlog = FOUR_G_ADAPTER_DEFAULT_TCP_BACKLOG;
        four_g_tcp_config.ipc = &ipc;
        four_g_tcp_config.collector = &collector;
        four_g_tcp_config.linux_epoch = linux_epoch;

        if (four_g_tcp_server_start(&four_g_tcp_config) != 0) {
            fprintf(stderr,
                    "failed to start 4G TCP RX service on %s/%s:%u\n",
                    config.four_g_ifname,
                    config.four_g_bind_addr,
                    (unsigned)config.four_g_port);
            exit_code = EXIT_FAILURE;
            g_should_stop = 1;
        } else {
            four_g_tcp_started = true;
            printf("linux_app: 4G TCP RX listening on %s/%s:%u, linux_epoch=%u\n",
                   config.four_g_ifname,
                   config.four_g_bind_addr,
                   (unsigned)config.four_g_port,
                   linux_epoch);
        }
    }

    if (!g_should_stop && config.bluetooth_enabled) {
        bluetooth_config_t bt_config;

        memset(&bt_config, 0, sizeof(bt_config));
        bt_config.ipc = &ipc;
        bt_config.collector = &collector;
        bt_config.linux_epoch = linux_epoch;
        bt_config.channel = config.bluetooth_channel;

        if (bluetooth_server_start(&bt_config) != 0) {
            fprintf(stderr,
                    "failed to start bluetooth service on channel %u\n",
                    (unsigned)config.bluetooth_channel);
            exit_code = EXIT_FAILURE;
            g_should_stop = 1;
        } else {
            bluetooth_started = true;
            printf("linux_app: Bluetooth RFCOMM listening on channel %u, linux_epoch=%u\n",
                   (unsigned)config.bluetooth_channel,
                   linux_epoch);
        }
    }

    while (!g_should_stop) {
        linux_shm_ipc_stats_t stats;

        linux_shm_ipc_get_stats(&ipc, &stats);
        status_collector_update_ipc_stats(&collector, &stats, false, 0u);
        if (status_collector_write_all(&collector) != 0) {
            fprintf(stderr, "warning: failed to write status snapshots under %s\n", config.status_dir);
        }
        sleep_one_second();
    }

    if (egress_started) {
        egress_manager_stop(&egress_manager);
    }

    if (ethernet_udp_started) {
        ethernet_udp_server_stop();
    }
    if (ethernet_tcp_started) {
        ethernet_tcp_server_stop();
    }
    if (can_started) {
        can_adapter_stop();
    }
    if (wifi_udp_started) {
        wifi_udp_server_stop();
    }
    if (wifi_tcp_started) {
        wifi_tcp_server_stop();
    }
    if (four_g_udp_started) {
        four_g_udp_server_stop();
    }
    if (four_g_tcp_started) {
        four_g_tcp_server_stop();
    }
    if (bluetooth_started) {
        bluetooth_server_stop();
    }
    if (rs485_started) {
        rs485_adapter_stop();
    }

    {
        linux_shm_ipc_stats_t stats;
        linux_shm_ipc_get_stats(&ipc, &stats);
        status_collector_update_ipc_stats(&collector, &stats, false, 0u);
        (void)status_collector_write_all(&collector);
    }

    egress_manager_destroy(&egress_manager);
    linux_shm_ipc_unmap(&ipc);
    ethernet_tx_context_destroy(&ethernet_tx_context);
    wifi_tx_context_destroy(&wifi_tx_context);
    four_g_tx_context_destroy(&four_g_tx_context);
    status_collector_destroy(&collector);
    return exit_code;
}
