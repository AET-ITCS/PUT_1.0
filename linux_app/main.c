/* linux_app 入口：加载配置并启动多协议 CAN 网关任务。 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_config.h"
#include "frame_packer.h"
#include "protocol_manager.h"

static void print_usage(const char *program)
{
    printf("Usage: %s [--config PATH] [--udp-port PORT] [--max-packets N]\n", program);
    printf("          [--status-dir DIR] [--disable-status]\n");
    printf("  --config PATH      INI config path, default linux_app/config/device_config.ini if present\n");
    printf("  --udp-port PORT    compatibility override for ethernet_udp.port\n");
    printf("  --max-packets N    stop each enabled protocol worker after N frames, default 0 means forever\n");
    printf("  --status-dir DIR   write Web snapshots to DIR, default /run/put/status\n");
    printf("  --disable-status   do not write Web status snapshot files\n");
}

static int parse_u32(const char *text, uint32_t *out_value)
{
    char *end = NULL;
    unsigned long value;

    if ((text == NULL) || (out_value == NULL)) {
        return -1;
    }

    errno = 0;
    value = strtoul(text, &end, 0);
    if ((errno != 0) || (end == text) || (*end != '\0') || (value > UINT32_MAX)) {
        return -1;
    }

    *out_value = (uint32_t)value;
    return 0;
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
        uint32_t value;

        if (strcmp(argv[i], "--help") == 0) {
            return 1;
        }

        if (strcmp(argv[i], "--config") == 0) {
            ++i;
            continue;
        }

        if (strcmp(argv[i], "--udp-port") == 0) {
            if ((i + 1) >= argc ||
                parse_u32(argv[++i], &value) != 0 ||
                value == 0u ||
                value > UINT16_MAX) {
                fprintf(stderr, "invalid --udp-port\n");
                return -1;
            }
            config->ethernet_udp_enabled = true;
            config->ethernet_udp_port = (uint16_t)value;
            continue;
        }

        if (strcmp(argv[i], "--max-packets") == 0) {
            if ((i + 1) >= argc || parse_u32(argv[++i], &value) != 0) {
                fprintf(stderr, "invalid --max-packets\n");
                return -1;
            }
            config->max_packets = value;
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

int main(int argc, char **argv)
{
    app_config_t config;
    const char *config_path;
    bool config_explicit;
    int cli_rc;
    unified_error_t err;

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

    frame_packer_init(1u);
    return (protocol_manager_run(&config) == UNIFIED_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
