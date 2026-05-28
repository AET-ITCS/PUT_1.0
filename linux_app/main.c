/* linux_app 入口：大核 Linux v2 主线常驻服务。 */
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
#include "can_adapter.h"
#include "ethernet_adapter.h"
#include "linux_shm_ipc.h"
#include "status_collector.h"

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
}

static uint32_t make_linux_epoch(void)
{
    time_t now = time(NULL);

    if (now <= 0) {
        return 1u;
    }
    return (uint32_t)now;
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
    uint32_t linux_epoch;
    bool can_started = false;
    bool ethernet_udp_started = false;
    bool ethernet_tcp_started = false;
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

    linux_epoch = make_linux_epoch();
    err = linux_shm_ipc_map(&ipc, 0u, PUT_SHM_REGION_SIZE, NULL);
    if (err != UNIFIED_OK) {
        fprintf(stderr, "failed to map linux shm ipc region: error=%d\n", (int)err);
        status_collector_record_error(&collector,
                                      STATUS_MODULE_ETHERNET,
                                      "ipc_map",
                                      err);
        (void)status_collector_write_all(&collector);
        status_collector_destroy(&collector);
        return EXIT_FAILURE;
    }

    err = linux_shm_ipc_format_region(&ipc, ipc.region, linux_epoch, 0u, NULL);
    if (err != UNIFIED_OK) {
        fprintf(stderr, "failed to format linux shm ipc region: error=%d\n", (int)err);
        status_collector_record_error(&collector,
                                      STATUS_MODULE_ETHERNET,
                                      "ipc_format",
                                      err);
        (void)status_collector_write_all(&collector);
        linux_shm_ipc_unmap(&ipc);
        status_collector_destroy(&collector);
        return EXIT_FAILURE;
    }

    if (config.can_enabled) {
        can_adapter_config_t can_config;

        can_adapter_config_set_defaults(&can_config);
        can_config.enabled = true;
        (void)snprintf(can_config.ifname, sizeof(can_config.ifname), "%s", config.can_ifname);
        can_config.bitrate = config.can_bitrate;
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
            printf("linux_app: CAN RX listening on %s, expected externally configured bitrate=%u, linux_epoch=%u\n",
                   config.can_ifname,
                   (unsigned)config.can_bitrate,
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

    while (!g_should_stop) {
        linux_shm_ipc_stats_t stats;

        linux_shm_ipc_get_stats(&ipc, &stats);
        status_collector_update_ipc_stats(&collector, &stats, false, 0u);
        if (status_collector_write_all(&collector) != 0) {
            fprintf(stderr, "warning: failed to write status snapshots under %s\n", config.status_dir);
        }
        sleep_one_second();
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

    {
        linux_shm_ipc_stats_t stats;
        linux_shm_ipc_get_stats(&ipc, &stats);
        status_collector_update_ipc_stats(&collector, &stats, false, 0u);
        (void)status_collector_write_all(&collector);
    }

    linux_shm_ipc_unmap(&ipc);
    status_collector_destroy(&collector);
    return exit_code;
}
